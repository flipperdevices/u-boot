# SPDX-License-Identifier: GPL-2.0+
# Copyright (c) 2024 Flipper Devices
#
# Entry-type module for Rockchip LDR (loader) format images
#

"""Entry-type module for Rockchip LDR format boot images

This module creates Rockchip LDR format images used for USB download mode
(Maskrom boot). The LDR format is the native format expected by tools like
rkdeveloptool and upgrade_tool.

Format structure:
    - RKBootHead header (102 bytes)
    - Entry descriptors for 471, 472, and loader entries
    - Entry data sections
    - CRC32 checksum (4 bytes)

Entry types:
    - 471: SRAM code (typically DDR trainer/TPL) - runs in SRAM before DRAM init
    - 472: DRAM code (typically SPL/USB loader) - runs after DRAM is initialized
    - Loader: Flash loader entries (not sent via USB, used for flashing)
"""

from collections import OrderedDict
import struct

from binman.entry import Entry
from binman.etype.section import Entry_section
from dtoc import fdt_util
from u_boot_pylib import tools


# Magic tags
RKBOOT_TAG = 0x544F4F42  # "BOOT"
RKLDR_TAG = 0x2052444C   # "LDR "

# Header size (fixed)
RKBOOT_HEAD_SIZE = 102

# Entry descriptor size
RKBOOT_ENTRY_SIZE = 57  # 1 + 4 + 40 + 4 + 4 + 4

# Entry types
ENTRY_TYPE_471 = 1    # SRAM entries (DDR trainer)
ENTRY_TYPE_472 = 2    # DRAM entries (USB loader)
ENTRY_TYPE_LOADER = 4 # Flash loader entries


class Entry_rockchip_ldr(Entry_section):
    """Rockchip LDR format boot image

    Properties / Entry arguments:
        - ldr-version: Version number (default: 0x100)
        - merge-version: Merge version (default: 0x100)
        - chip: Chip type code (default: 0x80 for RK32)
        - rc4-on: Enable RC4 encryption (default: False)
        - filename: Output filename (optional, no separate file written if not set)

    The subnodes define the entries to include. Each subnode should have:
        - entry-type: "471", "472", or "loader"
        - entry-name: Name stored in the entry (max 20 chars)
        - entry-delay: Delay in ms after sending (default: 0)

    Example::

        rockchip-ldr {
            filename = "download.bin";
            ldr-version = <0x109>;

            entry-471 {
                type = "section";
                entry-type = "471";
                entry-name = "rk3576_ddr";
                entry-delay = <1>;
                rockchip-tpl {
                };
            };

            entry-472 {
                type = "section";
                entry-type = "472";
                entry-name = "rk3576_spl";
                u-boot-spl {
                };
            };
        };

    This creates an LDR format image with TPL as the 471 entry and SPL as
    the 472 entry, suitable for use with rkdeveloptool/upgrade_tool.
    """
    def __init__(self, section, etype, node):
        super().__init__(section, etype, node)
        self._ldr_entries = []  # List of (type, name, delay, entry_name)

    def ReadNode(self):
        super().ReadNode()
        self._ldr_version = fdt_util.GetInt(self._node, 'ldr-version',
                                            0x100)
        self._merge_version = fdt_util.GetInt(self._node,
                                               'merge-version', 0x100)
        self._chip_type = fdt_util.GetInt(self._node, 'chip', 0x80)
        self._rc4_on = fdt_util.GetBool(self._node, 'rc4-on')
        self._filename = fdt_util.GetString(self._node, 'filename', None)

    def ReadEntries(self):
        """Read the subnodes to find out what should go in this LDR image"""
        for node in self._node.subnodes:
            if self.IsSpecialSubnode(node):
                continue

            # Read entry metadata
            entry_type_str = fdt_util.GetString(node, 'entry-type', '')
            entry_name = fdt_util.GetString(node, 'entry-name', '')
            entry_delay = fdt_util.GetInt(node, 'entry-delay', 0)

            if entry_type_str == '471':
                entry_type = ENTRY_TYPE_471
            elif entry_type_str == '472':
                entry_type = ENTRY_TYPE_472
            elif entry_type_str == 'loader':
                entry_type = ENTRY_TYPE_LOADER
            else:
                # Not a Rockchip entry, but still process it as a regular section
                entry_type = None

            if entry_type is not None:
                self._ldr_entries.append((entry_type, entry_name, entry_delay,
                                          node.name))

            # Let parent class handle reading child entries
            entry = Entry.Create(self, node)
            entry.ReadNode()
            entry.SetPrefix(self._name_prefix)
            self._entries[node.name] = entry

    def _make_crc32_table(self):
        """Generate Rockchip's custom CRC32 table

        Rockchip uses a non-standard CRC32 polynomial (0x04C10DB7)
        """
        poly = 0x04C10DB7
        table = []
        for i in range(256):
            crc = i << 24
            for _ in range(8):
                if crc & 0x80000000:
                    crc = (crc << 1) ^ poly
                else:
                    crc <<= 1
                crc &= 0xFFFFFFFF
            table.append(crc)
        return table

    def _calc_crc32(self, data):
        """Calculate CRC32 using Rockchip's algorithm"""
        table = self._make_crc32_table()
        crc = 0
        for byte in data:
            crc = ((crc << 8) ^ table[((crc >> 24) ^ byte) & 0xFF]) & 0xFFFFFFFF
        return crc

    def _encode_utf16le_name(self, name, max_chars=20):
        """Encode a name as UTF-16LE, padded to max_chars characters"""
        # Each char is 2 bytes in UTF-16LE
        encoded = name.encode('utf-16le')
        max_bytes = max_chars * 2
        if len(encoded) > max_bytes:
            encoded = encoded[:max_bytes]
        return encoded.ljust(max_bytes, b'\x00')

    def _build_entry_descriptor(self, entry_type, name, data_offset, data_size,
                                 delay):
        """Build a single entry descriptor (57 bytes)

        Format:
            - 1 byte: entry count (always 1 for our use)
            - 4 bytes: entry type
            - 40 bytes: name (UTF-16LE, 20 chars max)
            - 4 bytes: data offset
            - 4 bytes: data size
            - 4 bytes: delay in ms
        """
        desc = struct.pack('<BI', 1, entry_type)
        desc += self._encode_utf16le_name(name, 20)
        desc += struct.pack('<III', data_offset, data_size, delay)
        return desc

    def _build_header(self, entry_471_count, entry_472_count, loader_count,
                      entry_471_offset, entry_472_offset, loader_offset):
        """Build the RKBootHead header (102 bytes)

        Reference:
            https://github.com/rockchip-linux/rkdeveloptool/blob/21b25fd4a70331819b557fe93015b635b9594543/RKBoot.h#L7-L26

        Format (from RKBootHead struct):
            - 4 bytes: LDR tag ("LDR ")
            - 2 bytes: header size (always 102)
            - 4 bytes: version
            - 4 bytes: merge version
            - 7 bytes: release time (RKTime: year(2)+month(1)+day(1)+hour(1)+min(1)+sec(1))
            - 4 bytes: chip type
            - 1 byte: entry 471 count
            - 4 bytes: entry 471 offset
            - 1 byte: entry 471 size (57)
            - 1 byte: entry 472 count
            - 4 bytes: entry 472 offset
            - 1 byte: entry 472 size (57)
            - 1 byte: loader count
            - 4 bytes: loader offset
            - 1 byte: loader size (57)
            - 1 byte: sign flag
            - 1 byte: RC4 flag (0 = RC4 on, 1 = RC4 off)
            - 57 bytes: reserved
        """
        # Get current date/time
        import datetime
        now = datetime.datetime.now()

        header = struct.pack('<I', RKLDR_TAG)  # "LDR "
        header += struct.pack('<H', RKBOOT_HEAD_SIZE)  # header size
        header += struct.pack('<I', self._ldr_version)
        header += struct.pack('<I', self._merge_version)
        # RKTime: year(2) + month(1) + day(1) + hour(1) + minute(1) + second(1) = 7 bytes
        header += struct.pack('<HBBBBB', now.year, now.month, now.day,
                              now.hour, now.minute, now.second)
        header += struct.pack('<I', self._chip_type)

        # Entry 471
        header += struct.pack('<B', entry_471_count)
        header += struct.pack('<I', entry_471_offset)
        header += struct.pack('<B', RKBOOT_ENTRY_SIZE)

        # Entry 472
        header += struct.pack('<B', entry_472_count)
        header += struct.pack('<I', entry_472_offset)
        header += struct.pack('<B', RKBOOT_ENTRY_SIZE)

        # Loader entries
        header += struct.pack('<B', loader_count)
        header += struct.pack('<I', loader_offset)
        header += struct.pack('<B', RKBOOT_ENTRY_SIZE)

        # Sign flag (0 = not signed)
        header += struct.pack('<B', 0)

        # RC4 flag (1 = RC4 off)
        header += struct.pack('<B', 0 if self._rc4_on else 1)

        # Reserved bytes to reach 102 bytes total
        current_len = len(header)
        reserved_len = RKBOOT_HEAD_SIZE - current_len
        header += b'\x00' * reserved_len

        return header

    def BuildSectionData(self, required):
        """Build the LDR format image data"""
        # First, let the parent class build all child entry data
        # We need the raw data from child sections
        for entry in self._entries.values():
            entry.Pack(0)

        # Collect data from each entry type
        entry_471_list = []  # List of (name, delay, data)
        entry_472_list = []
        loader_list = []

        for entry_type, name, delay, entry_name in self._ldr_entries:
            entry = self._entries.get(entry_name)
            if entry is None:
                self.Raise(f"Entry '{entry_name}' not found")
            data = entry.GetData()
            if data is None:
                if required:
                    self.Raise(f"Entry '{entry_name}' has no data")
                data = b''

            if entry_type == ENTRY_TYPE_471:
                entry_471_list.append((name, delay, data))
            elif entry_type == ENTRY_TYPE_472:
                entry_472_list.append((name, delay, data))
            elif entry_type == ENTRY_TYPE_LOADER:
                loader_list.append((name, delay, data))

        # Calculate offsets for entry descriptors
        # Header is at offset 0, size RKBOOT_HEAD_SIZE (102)
        current_offset = RKBOOT_HEAD_SIZE

        entry_471_offset = current_offset
        current_offset += len(entry_471_list) * RKBOOT_ENTRY_SIZE

        entry_472_offset = current_offset
        current_offset += len(entry_472_list) * RKBOOT_ENTRY_SIZE

        loader_offset = current_offset
        current_offset += len(loader_list) * RKBOOT_ENTRY_SIZE

        # Data section starts after all descriptors
        data_offset = current_offset

        # Build entry descriptors and collect data offsets
        entry_descriptors = b''
        entry_data = b''

        # Process 471 entries
        for name, delay, data in entry_471_list:
            desc = self._build_entry_descriptor(
                ENTRY_TYPE_471, name, data_offset + len(entry_data),
                len(data), delay)
            entry_descriptors += desc
            entry_data += data

        # Process 472 entries
        for name, delay, data in entry_472_list:
            desc = self._build_entry_descriptor(
                ENTRY_TYPE_472, name, data_offset + len(entry_data),
                len(data), delay)
            entry_descriptors += desc
            entry_data += data

        # Process loader entries
        for name, delay, data in loader_list:
            desc = self._build_entry_descriptor(
                ENTRY_TYPE_LOADER, name, data_offset + len(entry_data),
                len(data), delay)
            entry_descriptors += desc
            entry_data += data

        # Build header
        header = self._build_header(
            len(entry_471_list), len(entry_472_list), len(loader_list),
            entry_471_offset, entry_472_offset, loader_offset)

        # Assemble final image (without CRC)
        image_data = header + entry_descriptors + entry_data

        # Calculate and append CRC32
        crc = self._calc_crc32(image_data)
        image_data += struct.pack('<I', crc)

        return image_data

    def SetImagePos(self, image_pos):
        """Override to prevent setting image_pos for child entries

        We handle the layout ourselves in BuildSectionData
        """
        self.image_pos = image_pos
        # Don't call parent - we manage child positions ourselves

    def CheckEntries(self):
        """Override to skip offset overlap checks

        We handle the layout ourselves in BuildSectionData, so the child
        entries don't have meaningful offsets within the section.
        """
        pass

    def SetCalculatedProperties(self):
        """Set properties that depend on the final image layout"""
        Entry.SetCalculatedProperties(self)
        # Don't call Entry_section's version as we handle layout differently

    def GetEntryContents(self, skip_entry=None):
        """Get contents of all entries

        This ensures child entries have their contents ready
        """
        return super().GetEntryContents(skip_entry)

    def ObtainContents(self):
        """Obtain contents for the LDR image"""
        # First make sure all child entries have their contents
        for entry in self._entries.values():
            if not entry.ObtainContents():
                return False
        return True

    def Pack(self, offset):
        """Pack the entry into the image at the given offset"""
        # Pack children first
        for entry in self._entries.values():
            entry.Pack(0)

        # Build our data
        data = self.BuildSectionData(False)
        self.SetContents(data)
        return super(Entry_section, self).Pack(offset)

    def GetDefaultFilename(self):
        return self._filename
