# SPDX-License-Identifier: GPL-2.0+
# SPDX-FileCopyrightText: Copyright 2026 Flipper FZCO

"""Support for Rockchip's maskrom loader format

Rockchip SoCs can be booted over USB while in maskrom (BootROM) mode. Two
classes of payload are involved: code 471 blobs, which the BootROM loads into
SRAM and runs (typically DRAM initialisation), and code 472 blobs, which are
loaded into the DRAM that the former set up (typically SPL and its payload).

Rockchip's boot_merger tool packs those blobs, plus an optional copy of the
on-flash boot block, into a single 'loader' container which host-side tools
consume, e.g. 'rkdeveloptool db', 'rockusb download-boot' and 'xrock maskrom'.

The container is a header, then a table of entry descriptors for each of the
three classes, then the payloads, then a checksum of the whole file. All fields
are little-endian and the structures are byte-packed, so nothing beyond the tag
is naturally aligned.

Written from the GPL-2.0+ implementations in Rockchip's downstream U-Boot
(tools/rockchip/boot_merger.c) and in rkdeveloptool (RKBoot.cpp, crc.cpp).
"""

import binascii
import struct

# Container header: tag, header size, version, merge version, release time as
# year/month/day/hour/minute/second, chip code, then a count/offset/entry-size
# triplet for each of the three entry classes, then the signing and RC4 flags.
# Nothing is aligned, hence the '<' and the lack of any padding.
HEADER_FORMAT = '<4sHIIHBBBBBI' + 'BIB' * 3 + 'BB'
HEADER_SIZE = 102
HEADER_TAG = b'LDR '

# boot_merger writes this in every loader; rkdeveloptool reports it but does
# not act on it
MERGE_VERSION = 0x01000000

# Entry descriptor. One table per entry class, all three contiguous after the
# header.
ENTRY_FORMAT = '<BI40sIII'
ENTRY_SIZE = 57

# Payload sizes are a whole number of these; offsets are not aligned at all
BLOCK_SIZE = 512

# Entry names are UTF-16LE, and the field cannot be grown
MAX_NAME_LEN = 20

# Entry classes, as used in the descriptor's type field
ENTRY_471 = 1
ENTRY_472 = 2
ENTRY_LOADER = 4

# Maps the 'rockchip,entry-type' property to the above
ENTRY_TYPES = {
    '471': ENTRY_471,
    '472': ENTRY_472,
    'loader': ENTRY_LOADER,
}

# Fixed key used for every RC4-encrypted Rockchip boot payload. The same key is
# in tools/rkcommon.c, which uses lib/rc4.c to apply it.
RC4_KEY = bytes([124, 78, 3, 4, 85, 5, 9, 7, 45, 44, 123, 56, 23, 13, 23, 17])

# Rockchip's CRC-32 uses this in place of the usual 0x04c11db7
CRC32_POLY = 0x04c10db7

# Initial value for the CRC-16/CCITT-FALSE (polynomial 0x1021) which the BootROM
# checks each USB transfer with
CRC16_INIT = 0xffff


# How many 32-bit words to unpack from the data at a time. Doing it in bulk
# avoids allocating an object per word, which otherwise dominates; the size is a
# compromise against the tuple that each unpack builds.
CRC32_CHUNK_WORDS = 4096


def _make_crc32_tables():
    """Build the lookup tables used by crc32_rk()

    Loader images run to tens of megabytes, so the checksum is worth taking
    four bytes at a time. Table n holds the contribution of a byte n places
    from the end of the four being consumed.

    Returns:
        list of list of int: Four tables of 256 entries each
    """
    table = []
    for byte in range(256):
        crc = byte << 24
        for _ in range(8):
            if crc & 0x80000000:
                crc = ((crc << 1) ^ CRC32_POLY) & 0xffffffff
            else:
                crc = (crc << 1) & 0xffffffff
        table.append(crc)

    tables = [table]
    for _ in range(3):
        tables.append([((crc << 8) ^ table[crc >> 24]) & 0xffffffff
                       for crc in tables[-1]])
    return tables


CRC32_TABLES = _make_crc32_tables()


def crc32_rk(data):
    """Calculate the checksum stored at the end of a loader file

    This is not the common CRC-32: Rockchip's variant uses polynomial
    0x04c10db7, starts from zero and neither reflects its input and output nor
    XORs the result. rkdeveloptool refuses to open a loader whose trailing
    checksum does not match, so it must be recalculated on any change.

    Args:
        data (bytes): Data to checksum

    Returns:
        int: Checksum of the data
    """
    tab0, tab1, tab2, tab3 = CRC32_TABLES
    view = memoryview(data)
    end = len(data) - len(data) % 4
    crc = 0
    pos = 0
    while pos < end:
        count = min(CRC32_CHUNK_WORDS, (end - pos) // 4)
        for word in struct.unpack_from(f'>{count}I', view, pos):
            crc ^= word
            crc = (tab3[crc >> 24] ^ tab2[(crc >> 16) & 0xff] ^
                   tab1[(crc >> 8) & 0xff] ^ tab0[crc & 0xff])
        pos += count * 4
    for byte in view[end:]:
        crc = ((crc << 8) ^ tab0[(crc >> 24) ^ byte]) & 0xffffffff
    return crc


def crc16_ccitt(data):
    """Calculate the CRC-16 which the BootROM checks a USB transfer against

    The host appends this over everything it is about to send, so a payload
    ending in its own CRC-16 makes the value the host appends come out as zero.

    binascii provides exactly this CRC - polynomial 0x1021, most-significant
    bit first, no reflection and no final XOR - which matters, since it covers
    every byte of every payload.

    Args:
        data (bytes): Data to checksum

    Returns:
        int: Checksum of the data
    """
    return binascii.crc_hqx(data, CRC16_INIT)


def rc4_encode(data, block_size=None):
    """Encrypt (or decrypt) data with Rockchip's fixed RC4 key

    Args:
        data (bytes): Data to process
        block_size (int): Restart the cipher every this many bytes, or None to
            run a single stream over all of the data. boot_merger uses 512-byte
            blocks for the on-flash boot block and a single stream for code
            471/472 payloads.

    Returns:
        bytes: Processed data
    """
    if block_size:
        return b''.join(
            rc4_encode(data[pos:pos + block_size])
            for pos in range(0, len(data), block_size))

    sbox = list(range(256))
    j = 0
    for i in range(256):
        j = (j + sbox[i] + RC4_KEY[i % len(RC4_KEY)]) % 256
        sbox[i], sbox[j] = sbox[j], sbox[i]

    out = bytearray(len(data))
    i = j = 0
    for pos, byte in enumerate(data):
        i = (i + 1) % 256
        j = (j + sbox[i]) % 256
        sbox[i], sbox[j] = sbox[j], sbox[i]
        out[pos] = byte ^ sbox[(sbox[i] + sbox[j]) % 256]
    return bytes(out)


def chip_code(name):
    """Convert a chip name into the code stored in the header

    boot_merger drops the vendor prefix from the [CHIP_NAME] name and packs the
    next four characters big-endian, so RK3576 becomes 0x33353736. Note that
    the name is not always the SoC number - rk3399 uses RK330C.

    Args:
        name (str): Chip name, with or without an 'rk'/'rv' prefix, e.g.
            'rk3576', 'RK3576' or '3576'

    Returns:
        int: Chip code for the header

    Raises:
        ValueError: The name does not hold four usable characters
    """
    code = name.upper()
    if len(code) == 6 and code[:2] in ('RK', 'RV'):
        code = code[2:]
    if len(code) != 4 or not code.isalnum() or not code.isascii():
        raise ValueError(
            f"Chip name '{name}' must provide four alphanumeric characters")
    return int.from_bytes(code.encode('ascii'), 'big')


def encode_name(name):
    """Encode an entry name for its descriptor

    Args:
        name (str): Name to encode

    Returns:
        bytes: Name as NUL-padded UTF-16LE, MAX_NAME_LEN * 2 bytes long

    Raises:
        ValueError: The name is too long for the field
    """
    if len(name) > MAX_NAME_LEN:
        raise ValueError(
            f"Entry name '{name}' is longer than {MAX_NAME_LEN} characters")
    return name.encode('utf-16-le').ljust(MAX_NAME_LEN * 2, b'\0')


def build_header(chip, version, counts, timestamp, rc4_disable):
    """Build the loader header

    Args:
        chip (int): Chip code, from chip_code()
        version (int): Loader version, (major << 8) | minor
        counts (dict): Number of entries, keyed by entry class
        timestamp (time.struct_time): Time to record as the release time
        rc4_disable (bool): True if the code 471/472 payloads are stored as
            plain data rather than RC4-encrypted

    Returns:
        bytes: The header, HEADER_SIZE bytes long
    """
    offset = HEADER_SIZE
    table = {}
    for entry_type in (ENTRY_471, ENTRY_472, ENTRY_LOADER):
        table[entry_type] = offset
        offset += counts[entry_type] * ENTRY_SIZE

    hdr = struct.pack(
        HEADER_FORMAT, HEADER_TAG, HEADER_SIZE, version, MERGE_VERSION,
        timestamp.tm_year, timestamp.tm_mon, timestamp.tm_mday,
        timestamp.tm_hour, timestamp.tm_min, timestamp.tm_sec, chip,
        counts[ENTRY_471], table[ENTRY_471], ENTRY_SIZE,
        counts[ENTRY_472], table[ENTRY_472], ENTRY_SIZE,
        counts[ENTRY_LOADER], table[ENTRY_LOADER], ENTRY_SIZE,
        0, int(rc4_disable))
    return hdr + bytes(HEADER_SIZE - len(hdr))


def build_entry(entry_type, name, offset, size, delay_ms):
    """Build one entry descriptor

    Args:
        entry_type (int): Entry class, one of ENTRY_471/472/LOADER
        name (str): Entry name
        offset (int): Offset of the payload within the file
        size (int): Size of the payload
        delay_ms (int): How long the host should wait after sending the payload

    Returns:
        bytes: The descriptor, ENTRY_SIZE bytes long
    """
    return struct.pack(ENTRY_FORMAT, ENTRY_SIZE, entry_type, encode_name(name),
                       offset, size, delay_ms)
