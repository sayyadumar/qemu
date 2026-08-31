#!/usr/bin/env python3
#
# Functional tests for the Renesas RX CRC calculator (CRCA).
#
# Copyright (c) 2024 QEMU Contributors
#
# This work is licensed under the terms of the GNU GPL, version 2 or
# later.  See the COPYING file in the top-level directory.

import os
import re
import struct
import subprocess
import tempfile

from qemu_test import QemuSystemTest

CRC_BASE = 0x00088280
R_CRCCR = 0x00
R_CRCDIR = 0x04
R_CRCDOR = 0x08

CFLASH_BASE = 0xFFE80000        # rx651-r5f5651c
CFLASH_SIZE = 1536 * 1024

# CRCCR fields: GPS[2:0] selects the polynomial, LMS the bit order, and
# DORCLR clears the running result.
GPS_CRC8 = 1            # X8 + X2 + X + 1
GPS_CRC16 = 2           # X16 + X15 + X2 + 1
GPS_CCITT = 3           # X16 + X12 + X5 + 1
GPS_CRC32 = 4           # the IEEE 802.3 polynomial
LMS_MSB = 0x40
DORCLR = 0x80


class Asm:
    """Just enough RX assembly to drive the CRC registers."""

    def __init__(self):
        self.code = bytearray()

    def _put(self, *values):
        self.code.extend(values)

    def movi(self, imm, rd):
        """MOV.L #imm32, Rd"""
        self._put(0xFB, (rd << 4) | 0x02)
        self.code.extend(struct.pack('<I', imm & 0xffffffff))

    def stb(self, value, byte_off, rd=1):
        """MOV.B #imm8, dsp8[Rd]"""
        self._put(0xF9, (rd << 4) | 0x04, byte_off, value)

    def stl(self, rs, byte_off, rd=1):
        """MOV.L Rs, dsp[Rd]; the encoded displacement is scaled by 4."""
        d = byte_off // 4
        self._put(0xA0 | ((d >> 2) & 7),
                  ((d >> 1 & 1) << 7) | (rd << 4) | ((d & 1) << 3) | rs)

    def ldl(self, byte_off, rd, rs=1):
        """MOV.L dsp[Rs], Rd"""
        d = byte_off // 4
        self._put(0xA8 | ((d >> 2) & 7),
                  ((d >> 1 & 1) << 7) | (rs << 4) | ((d & 1) << 3) | rd)

    def spin(self):
        self._put(0x2E, 0x00)

    def image(self):
        blob = bytearray(b'\xff' * CFLASH_SIZE)
        blob[0:len(self.code)] = self.code
        struct.pack_into('<I', blob, CFLASH_SIZE - 4, CFLASH_BASE)
        return bytes(blob)


class RXCrcMachine(QemuSystemTest):

    timeout = 30

    def run_asm(self, asm):
        with tempfile.NamedTemporaryFile(suffix='.bin', delete=False) as f:
            f.write(asm.image())
            path = f.name
        try:
            cmd = [self.qemu_bin, '-machine', 'rx651-r5f5651c', '-bios', path,
                   '-nographic', '-d', 'cpu,unimp,guest_errors']
            try:
                proc = subprocess.run(cmd, capture_output=True, timeout=10,
                                      text=True, errors='replace')
                out = proc.stdout + proc.stderr
            except subprocess.TimeoutExpired as exc:
                o = exc.stdout or ''
                e = exc.stderr or ''
                if isinstance(o, bytes):
                    o = o.decode(errors='replace')
                if isinstance(e, bytes):
                    e = e.decode(errors='replace')
                out = o + e
        finally:
            os.unlink(path)
        # The CRC block must be a real device, not the catch-all.
        self.assertNotIn('unimplemented device', out)
        parts = re.split(r'^pc=', out, flags=re.M)
        self.assertGreater(len(parts), 1, 'no CPU state dump in log')
        return 'pc=' + parts[-1]

    def test_manual_worked_examples(self):
        """
        Section 42.3 works through the CCITT polynomial with a cleared
        CRCDOR: feeding 0xf0 gives 0xf78f LSB first and 0xef1f MSB first.
        """
        self.set_machine('rx651-r5f5651c')

        a = Asm()
        a.movi(CRC_BASE, 1)
        a.stb(DORCLR | GPS_CCITT, R_CRCCR)          # 0x83, LSB first
        a.stb(0xF0, R_CRCDIR)
        a.ldl(R_CRCDOR, 2)
        a.stb(DORCLR | LMS_MSB | GPS_CCITT, R_CRCCR)  # 0xc3, MSB first
        a.stb(0xF0, R_CRCDIR)
        a.ldl(R_CRCDOR, 3)
        a.spin()

        state = self.run_asm(a)
        self.assertIn('r2=0x0000f78f', state)
        self.assertIn('r3=0x0000ef1f', state)

    def test_standard_16bit_vectors(self):
        """
        The usual check value, "123456789". With CRCDOR left at zero and no
        final XOR, LSB first with the X16+X15+X2+1 polynomial is CRC-16/ARC
        and MSB first with the CCITT polynomial is CRC-16/XMODEM.
        """
        self.set_machine('rx651-r5f5651c')

        msg = b"123456789"
        a = Asm()
        a.movi(CRC_BASE, 1)
        a.stb(DORCLR | GPS_CRC16, R_CRCCR)
        for ch in msg:
            a.stb(ch, R_CRCDIR)
        a.ldl(R_CRCDOR, 2)
        a.stb(DORCLR | LMS_MSB | GPS_CCITT, R_CRCCR)
        for ch in msg:
            a.stb(ch, R_CRCDIR)
        a.ldl(R_CRCDOR, 3)
        a.stb(DORCLR | LMS_MSB | GPS_CRC8, R_CRCCR)
        for ch in msg:
            a.stb(ch, R_CRCDIR)
        a.ldl(R_CRCDOR, 4)
        a.spin()

        state = self.run_asm(a)
        self.assertIn('r2=0x0000bb3d', state)   # CRC-16/ARC
        self.assertIn('r3=0x000031c3', state)   # CRC-16/XMODEM
        self.assertIn('r4=0x000000f4', state)   # CRC-8/SMBUS

    def test_32bit_crc_and_seeding(self):
        """
        The 32-bit polynomial takes longword writes, and CRCDOR doubles as
        the seed, so a firmware wanting the familiar CRC-32 writes all ones
        to it first.
        """
        self.set_machine('rx651-r5f5651c')

        a = Asm()
        a.movi(CRC_BASE, 1)
        a.stb(DORCLR | GPS_CRC32, R_CRCCR)
        a.movi(0xFFFFFFFF, 6)
        a.stl(6, R_CRCDOR)                      # seed
        a.movi(0x34333231, 7)                   # "1234", little endian
        a.stl(7, R_CRCDIR)
        a.ldl(R_CRCDOR, 2)
        a.spin()

        self.assertIn('r2=0x641c1f5c', self.run_asm(a))

    def test_no_calculation_selects(self):
        """
        GPS values 0, 6 and 7 select no calculation, so CRCDOR keeps its
        seed however much data is written.
        """
        self.set_machine('rx651-r5f5651c')

        a = Asm()
        a.movi(CRC_BASE, 1)
        a.stb(DORCLR, R_CRCCR)                  # GPS = 000
        a.movi(0x0000A5A5, 6)
        a.stl(6, R_CRCDOR)
        a.stb(0xF0, R_CRCDIR)
        a.stb(0x0F, R_CRCDIR)
        a.ldl(R_CRCDOR, 2)
        a.spin()

        self.assertIn('r2=0x0000a5a5', self.run_asm(a))



class RXRomCacheMachine(QemuSystemTest):
    """
    Start-up code enables the ROM cache and spins until the enable register
    reads back, so without a device at 0x00081000 the machine hangs before
    anything else in the firmware runs.
    """

    timeout = 30

    def test_romce_readback_releases_the_spin(self):
        self.set_machine('rx651-r5f5651c')

        ROMCE = 0x00081000
        a = Asm()
        a.movi(ROMCE, 1)
        a._put(0xF9, 0x19, 0x00, 0x01, 0x00)      # FLASH.ROMCE.WORD = 1
        loop = len(a.code)
        a._put(0xB8, 0x12)                        # MOVU.W [R1], R2
        a._put(0x61, 0x12)                        # CMP #1, R2
        here = len(a.code)
        a._put(0x21, (loop - here) & 0xFF)        # BNE loop
        a.movi(0x00C0FFEE, 7)                     # reached only past the spin
        a.spin()

        with tempfile.NamedTemporaryFile(suffix='.bin', delete=False) as f:
            f.write(a.image())
            path = f.name
        try:
            cmd = [self.qemu_bin, '-machine', 'rx651-r5f5651c', '-bios', path,
                   '-nographic', '-d', 'cpu,unimp']
            try:
                proc = subprocess.run(cmd, capture_output=True, timeout=10,
                                      text=True, errors='replace')
                out = proc.stdout + proc.stderr
            except subprocess.TimeoutExpired as exc:
                o = exc.stdout or ''
                e = exc.stderr or ''
                if isinstance(o, bytes):
                    o = o.decode(errors='replace')
                if isinstance(e, bytes):
                    e = e.decode(errors='replace')
                out = o + e
        finally:
            os.unlink(path)

        # The ROM cache registers must be a real device, not the catch-all.
        self.assertNotIn('unimplemented device', out)
        parts = re.split(r'^pc=', out, flags=re.M)
        self.assertGreater(len(parts), 1, 'no CPU state dump in log')
        self.assertIn('r7=0x00c0ffee', 'pc=' + parts[-1])


if __name__ == '__main__':
    QemuSystemTest.main()
