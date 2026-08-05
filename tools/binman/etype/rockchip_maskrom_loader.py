# SPDX-License-Identifier: GPL-2.0+
# SPDX-FileCopyrightText: Copyright 2026 Flipper FZCO
#
# Entry-type module for a Rockchip maskrom loader image
#

import collections
import os
import time

from binman import rockchip_maskrom_util as rkloader
from binman.etype.section import Entry_section
from dtoc import fdt_util
from u_boot_pylib import tools

# The loader properties of one subentry
RkEntry = collections.namedtuple(
    'RkEntry', ['entry_type', 'name', 'delay_ms', 'rc4', 'no_brom_crc'])

class Entry_rockchip_maskrom_loader(Entry_section):
    """Rockchip maskrom loader image

    Properties / Entry arguments:
        - rockchip,chip: Chip name, e.g. "rk3576" (see below)
        - rockchip,version-major: Loader major version, default 1
        - rockchip,version-minor: Loader minor version, default 0
        - rockchip,align: Payload alignment in 512-byte blocks, default 4.
          RK3576 uses 8

    Rockchip SoCs can be booted over USB while in maskrom (BootROM) mode. Two
    classes of payload are involved: code 471 blobs, which the BootROM loads
    into SRAM and runs (typically DRAM initialisation), and code 472 blobs,
    which are loaded into the DRAM that the former set up (typically SPL and
    its payload).

    This entry packs those blobs into the single container that host-side tools
    expect, as produced by Rockchip's boot_merger, e.g.::

        rkdeveloptool db u-boot-rockchip-loader.bin

    Each subnode is one entry in the container and is described by:

        - rockchip,entry-type: "471", "472" or "loader" - which class this
          payload belongs to
        - rockchip,entry-name: Name recorded for the payload, at most 20
          characters, defaulting to the name of the subnode
        - rockchip,data-delay-ms: How long the host should wait after sending
          this payload, in milliseconds, default 0
        - rockchip,rc4: Store this payload RC4-encrypted. BootROMs up to about
          RK3399 require it, and some want it for the code 471 payload only,
          so it is set per entry
        - rockchip,no-brom-crc: Have the BootROM skip verifying this payload
          (see below)

    A "loader" entry holds a copy of the on-flash boot block rather than
    something the BootROM runs, for host tools which can write it to storage.
    Those are always RC4-encrypted, as boot_merger encrypts them, whatever
    rockchip,rc4 says.

    The chip name is what boot_merger records, which is not always the SoC
    number: rk3399 calls itself RK330C, rk3328 RK322H, rk3288 RK320A, rk3368
    RK330A, rk3506 RK350F and rk3128 RK312A, among others. No open-source host
    tool validates it against the connected device, so it is informational
    there, but Rockchip's own tools may be less forgiving.

    Since each payload is handed to the BootROM on its own, the offsets and
    image positions reported for subentries are relative to the payload they
    are part of, not to the container.

    Example::

        rockchip-maskrom-loader {
            rockchip,chip = "rk3576";
            rockchip,align = <8>;

            ddr {
                type = "section";
                rockchip,entry-type = "471";
                rockchip,data-delay-ms = <1>;

                rockchip-tpl {
                };
            };

            spl {
                type = "section";
                rockchip,entry-type = "472";
                rockchip,no-brom-crc;

                u-boot-spl {
                };

                fit {
                };
            };
        };

    About rockchip,no-brom-crc: the BootROM verifies every payload it is sent
    against a CRC-16 which the host appends to the transfer. On some SoCs
    (notably RK3576) it does so with a bit-serial routine running with the MMU
    and caches off, at around 124 KB/s, so a large payload can stall the
    maskrom for minutes with nothing on the console. The BootROM skips the
    check when the CRC it reads is zero, and since the CRC-16 in use has no
    final XOR, a payload ending in its own CRC-16 makes the value the host
    appends come out as exactly that. This property arranges for that, at the
    cost of the payload no longer being checked - which is the point. It is
    worth setting only where the payload is large enough for the check to
    hurt, so it is per entry.
    """
    def __init__(self, section, etype, node):
        super().__init__(section, etype, node)
        self.required_props = ['rockchip,chip']

        # Loader properties of each subentry, keyed by Entry
        self._rk_entries = {}

        # Release time, worked out once so that every pass over the image
        # records the same one
        self._timestamp = None

    def ReadNode(self):
        super().ReadNode()
        self.chip = fdt_util.GetString(self._node, 'rockchip,chip')
        self.align = fdt_util.GetInt(self._node, 'rockchip,align', 4)
        self.version = (
            fdt_util.GetInt(self._node, 'rockchip,version-major', 1) << 8 |
            fdt_util.GetInt(self._node, 'rockchip,version-minor', 0))

        try:
            self.chip_code = rkloader.chip_code(self.chip)
        except ValueError as exc:
            self.Raise(str(exc))

        if not self.align:
            self.Raise("'rockchip,align' must be non-zero")

        for entry in self._entries.values():
            self._rk_entries[entry] = self._ReadEntryNode(entry)

    def _ReadEntryNode(self, entry):
        """Read the loader properties of one subnode

        Args:
            entry (Entry): Entry to read the properties of

        Returns:
            RkEntry: Properties of the entry
        """
        node = entry._node
        etype = fdt_util.GetString(node, 'rockchip,entry-type')
        if etype not in rkloader.ENTRY_TYPES:
            entry.Raise("'rockchip,entry-type' must be one of %s" %
                        ', '.join(f"'{name}'" for name in rkloader.ENTRY_TYPES))
        name = fdt_util.GetString(node, 'rockchip,entry-name', node.name)
        try:
            rkloader.encode_name(name)
        except ValueError as exc:
            entry.Raise(str(exc))

        return RkEntry(
            entry_type=rkloader.ENTRY_TYPES[etype], name=name,
            delay_ms=fdt_util.GetInt(node, 'rockchip,data-delay-ms', 0),
            rc4=fdt_util.GetBool(node, 'rockchip,rc4'),
            no_brom_crc=fdt_util.GetBool(node, 'rockchip,no-brom-crc'))

    def _GetPayload(self, rk_entry, data):
        """Pad and encrypt the data of one entry

        Args:
            rk_entry (RkEntry): Properties of the entry
            data (bytes): Data to process

        Returns:
            bytearray: Payload to store in the container
        """
        align = self.align * rkloader.BLOCK_SIZE
        entry_type = rk_entry.entry_type

        # Nothing sends a loader entry to the BootROM, so it has no CRC to skip
        want_crc = rk_entry.no_brom_crc and entry_type != rkloader.ENTRY_LOADER

        # The CRC-16 goes in the last two bytes, so make room for it in case the
        # data happens to fill the last block exactly
        size = tools.align(len(data) + (2 if want_crc else 0), align)
        payload = bytearray(data)
        payload += tools.get_bytes(0, size - len(payload))

        if entry_type == rkloader.ENTRY_LOADER:
            payload = bytearray(rkloader.rc4_encode(payload,
                                                    rkloader.BLOCK_SIZE))
        elif rk_entry.rc4:
            payload = bytearray(rkloader.rc4_encode(payload))

        # The host appends its own CRC-16 over all of this, which comes out as
        # zero when the data already ends in its own CRC-16, telling the BootROM
        # to skip the check. A payload can be multiple megabytes, so take the
        # checksum through a memoryview rather than copying it to slice off the
        # last two bytes.
        if want_crc:
            crc = rkloader.crc16_ccitt(memoryview(payload)[:-2])
            payload[-2:] = crc.to_bytes(2, 'big')
        return payload

    def _GetTimestamp(self):
        """Get the time to record as the loader's release time

        binman builds a section several times over a run, so this is worked out
        once, to keep the time recorded in the image consistent.

        Returns:
            time.struct_time: Time to use, honouring SOURCE_DATE_EPOCH so that
                builds stay reproducible
        """
        if self._timestamp is None:
            epoch = os.environ.get('SOURCE_DATE_EPOCH')
            self._timestamp = (time.gmtime(int(epoch)) if epoch
                               else time.localtime())
        return self._timestamp

    def BuildSectionData(self, required):
        payloads = {entry_type: [] for entry_type in
                    (rkloader.ENTRY_471, rkloader.ENTRY_472,
                     rkloader.ENTRY_LOADER)}

        for entry in self._entries.values():
            rk_entry = self._rk_entries[entry]
            data = entry.GetData(required)
            if data is None:
                # A subsection has no contents until it is built, which is the
                # case when this entry is reached from a collection earlier in
                # the image description; see testCollectionSection()
                return None
            payloads[rk_entry.entry_type].append(
                (rk_entry, self._GetPayload(rk_entry, data)))

        counts = {entry_type: len(items)
                  for entry_type, items in payloads.items()}

        # boot_merger records RC4 as disabled when the code 472 payload is
        # stored as plain data, whatever it did with the code 471 one
        rc4_disable = not any(rk_entry.rc4 for rk_entry, _ in
                              payloads[rkloader.ENTRY_472])
        data = bytearray(rkloader.build_header(
            self.chip_code, self.version, counts, self._GetTimestamp(),
            rc4_disable))

        # The payloads start right after the three entry tables, and are not
        # aligned themselves - only their sizes are
        offset = (rkloader.HEADER_SIZE +
                  sum(counts.values()) * rkloader.ENTRY_SIZE)
        for entry_type, items in payloads.items():
            for rk_entry, payload in items:
                data += rkloader.build_entry(entry_type, rk_entry.name, offset,
                                             len(payload), rk_entry.delay_ms)
                offset += len(payload)

        for items in payloads.values():
            for _, payload in items:
                data += payload

        data += rkloader.crc32_rk(data).to_bytes(4, 'little')
        return bytes(data)

    def SetImagePos(self, image_pos):
        """Set the position of this entry and its subentries in the image

        Symbols written into a payload (e.g. SPL's reference to the image which
        follows it in a code 472 payload) are resolved as the load address of
        the payload holding the symbol, taken from its ELF, plus the image
        position of the entry being referred to. So each subentry has to be
        placed as if it started the image, making those image positions offsets
        within the payload; the load address then comes from the ELF, and DRAM
        need not start at zero for this to work out.

        Args:
            image_pos (int): Position of this entry in the image
        """
        super(Entry_section, self).SetImagePos(image_pos)

        for entry in self._entries.values():
            entry.SetOffsetSize(0, None)
            entry.SetImagePos(0)

    def CheckEntries(self):
        """Check the subentries are correctly laid out

        There is nothing to check: the subentries are not placed contiguously
        from the start of this entry, since the header and the entry tables come
        first, and each payload is padded independently.
        """
