#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+
# SPDX-FileCopyrightText: Copyright 2026 Flipper FZCO

"""Tests for rockchip_maskrom_util

These cover the checksums and the cipher, since getting any of them wrong
produces a loader image which host tools quietly refuse to use, or which the
BootROM rejects with no diagnostic at all.
"""

import os
import struct
import sys
import time
import unittest

# Make the binman package importable when run directly, without overriding the
# first path in PYTHONPATH
OUR_PATH = os.path.dirname(os.path.realpath(__file__))
sys.path.insert(2, os.path.join(OUR_PATH, '..'))

# pylint: disable=C0413
from binman import rockchip_maskrom_util as rkloader


class TestRockchipMaskrom(unittest.TestCase):
    """Tests for the Rockchip maskrom loader format helpers"""

    def test_crc32(self):
        """Test Rockchip's CRC-32 variant"""
        self.assertEqual(0, rkloader.crc32_rk(b''))
        self.assertEqual(0x889a9615, rkloader.crc32_rk(b'123456789'))

        # It must not be mistaken for the common CRC-32, which would give
        # 0xcbf43926 here
        self.assertNotEqual(0xcbf43926, rkloader.crc32_rk(b'123456789'))

    def test_crc16(self):
        """Test the CRC-16 the BootROM checks each transfer against"""
        # The published check value for CRC-16/CCITT-FALSE
        self.assertEqual(0x29b1, rkloader.crc16_ccitt(b'123456789'))

    def test_crc16_self_cancelling(self):
        """Test that appending the CRC-16 makes the CRC-16 come out as zero

        This is what lets a payload tell the BootROM to skip its (very slow)
        verification: the host appends its own CRC-16 over everything it sends,
        so a payload ending in its own CRC-16 makes that appended value zero.
        """
        for data in (b'\0', b'binman', bytes(4096), b'\xff' * 1000):
            crc = rkloader.crc16_ccitt(data)
            self.assertEqual(
                0, rkloader.crc16_ccitt(data + struct.pack('>H', crc)))

    def test_rc4(self):
        """Test RC4 encoding with Rockchip's fixed key"""
        data = b'binman rockchip loader'
        enc = rkloader.rc4_encode(data)
        self.assertEqual('0c4f429edff1bd2385535fad48386fb8936393526022',
                         enc.hex())

        # RC4 is symmetric, so encoding twice returns the original data
        self.assertEqual(data, rkloader.rc4_encode(enc))

    def test_rc4_blocks(self):
        """Test that block mode restarts the cipher for each block"""
        data = bytes(2 * rkloader.BLOCK_SIZE)
        enc = rkloader.rc4_encode(data, rkloader.BLOCK_SIZE)
        self.assertEqual(enc[:rkloader.BLOCK_SIZE], enc[rkloader.BLOCK_SIZE:])
        self.assertNotEqual(rkloader.rc4_encode(data), enc)

    def test_chip_code(self):
        """Test conversion of a chip name into the header's chip code"""
        self.assertEqual(0x33353736, rkloader.chip_code('rk3576'))
        self.assertEqual(0x33353736, rkloader.chip_code('RK3576'))
        self.assertEqual(0x33353736, rkloader.chip_code('3576'))
        self.assertEqual(0x33353838, rkloader.chip_code('rk3588'))

        # Not every name is the SoC number
        self.assertEqual(0x33333043, rkloader.chip_code('RK330C'))
        self.assertEqual(0x50583330, rkloader.chip_code('RKPX30'))

        for bad in ('rk3', 'rk3576x', 'rk-576'):
            with self.assertRaises(ValueError) as exc:
                rkloader.chip_code(bad)
            self.assertIn('must provide four alphanumeric characters',
                          str(exc.exception))

    def test_name(self):
        """Test encoding of an entry name"""
        self.assertEqual('550073006200'.ljust(80, '0'),
                         rkloader.encode_name('Usb').hex())

        with self.assertRaises(ValueError) as exc:
            rkloader.encode_name('x' * (rkloader.MAX_NAME_LEN + 1))
        self.assertIn('is longer than 20 characters', str(exc.exception))

    def test_header(self):
        """Test that the header matches one produced by boot_merger

        The bytes here are from the header of rkbin's
        rk3576_spl_loader_v1.03.102.bin, less the two vendor-specific flag bytes
        which boot_merger writes into the reserved area and which no known
        consumer reads.
        """
        counts = {
            rkloader.ENTRY_471: 2,
            rkloader.ENTRY_472: 1,
            rkloader.ENTRY_LOADER: 4,
        }
        hdr = rkloader.build_header(
            rkloader.chip_code('rk3576'), 0x164, counts,
            time.struct_time((2025, 6, 23, 16, 27, 56, 0, 0, 0)), True)
        self.assertEqual(rkloader.HEADER_SIZE, len(hdr))
        self.assertEqual(
            '4c44522066006401000000000001e9070617101b3836373533'
            '02660000003901d8000000390411010000390001', hdr[:45].hex())
        self.assertEqual(bytes(rkloader.HEADER_SIZE - 45), hdr[45:])

    def test_entry(self):
        """Test that an entry descriptor matches one produced by boot_merger

        These bytes are the first code-471 descriptor of the same loader.
        """
        entry = rkloader.build_entry(rkloader.ENTRY_471, 'UsbHead', 0x1f5,
                                     0x1000, 1)
        self.assertEqual(rkloader.ENTRY_SIZE, len(entry))
        self.assertEqual(
            '390100000055007300620048006500610064000000000000000000000000'
            '000000000000000000000000000000f50100000010000001000000',
            entry.hex())


if __name__ == '__main__':
    unittest.main()
