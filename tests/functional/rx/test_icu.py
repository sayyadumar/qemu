#!/usr/bin/env python3
#
# Functional tests for the RX65N/RX651 interrupt controller.
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

ICU_BASE = 0x00087000
R_IER = 0x200
R_IPR = 0x300
R_SWINTR = 0x2E0
SCI4_BASE = 0x0008A080

CFLASH_BASE = 0xFFE80000        # rx651-r5f5651c, 1.5 MB
CFLASH_SIZE = 1536 * 1024
VECTOR_TABLE = 0x00020000
HANDLER = CFLASH_BASE + 0x200
MARKER = 0x00C0FFEE

SWINT_VECTOR = 27
SCI4_TXI_VECTOR = 83


def u32(value):
    return struct.pack('<I', value & 0xffffffff)


def movl(imm, rd):
    """MOV.L #imm32, Rd"""
    return bytes([0xFB, (rd << 4) | 0x02]) + u32(imm)


def build_image(vector, ipr_number, trigger):
    """
    Firmware that installs a handler for one vector, gives it a priority,
    unmasks it, enables interrupts and then triggers it. The handler loads a
    marker into R7, so R7 says whether the interrupt was ever delivered.

    trigger is (address, displacement, value) for the byte write that raises
    the interrupt.
    """
    blob = bytearray(b'\xff' * CFLASH_SIZE)

    def put(off, *values):
        for i, v in enumerate(values):
            blob[off + i] = v

    off = 0

    def emit(data):
        nonlocal off
        blob[off:off + len(data)] = data
        off += len(data)

    emit(movl(0x00030000, 0))               # stack
    emit(movl(VECTOR_TABLE, 1))
    emit(bytes([0xFD, 0x68, (1 << 4) | 12]))   # MVTC R1, INTB
    emit(movl(HANDLER, 2))
    emit(movl(VECTOR_TABLE + vector * 4, 3))
    emit(bytes([0xA0, 0x32]))               # MOV.L R2, [R3]
    emit(movl(ICU_BASE + R_IPR + ipr_number, 4))
    emit(bytes([0xF9, 0x44, 0x00, 0x0F]))   # priority 15
    emit(movl(ICU_BASE + R_IER + vector // 8, 5))
    emit(bytes([0xF9, 0x54, 0x00, 1 << (vector % 8)]))
    emit(bytes([0x7F, 0xA8]))               # SETPSW I
    emit(movl(trigger[0], 6))
    emit(bytes([0xF9, 0x64, trigger[1], trigger[2]]))
    emit(bytes([0x2E, 0x00]))               # spin awaiting the interrupt

    # Handler: record the marker, then spin.
    hoff = 0x200
    blob[hoff:hoff + 6] = movl(MARKER, 7)
    put(hoff + 6, 0x2E, 0x00)

    struct.pack_into('<I', blob, CFLASH_SIZE - 4, CFLASH_BASE)
    return bytes(blob)


class RXIcuMachine(QemuSystemTest):

    timeout = 30

    def run_image(self, image):
        with tempfile.NamedTemporaryFile(suffix='.bin', delete=False) as f:
            f.write(image)
            path = f.name
        try:
            cmd = [self.qemu_bin, '-machine', 'rx651-r5f5651c',
                   '-bios', path, '-nographic', '-d', 'cpu']
            try:
                proc = subprocess.run(cmd, capture_output=True, timeout=10,
                                      text=True, errors='replace')
                return proc.stdout + proc.stderr
            except subprocess.TimeoutExpired as exc:
                out = exc.stdout or ''
                err = exc.stderr or ''
                if isinstance(out, bytes):
                    out = out.decode(errors='replace')
                if isinstance(err, bytes):
                    err = err.decode(errors='replace')
                return out + err
        finally:
            os.unlink(path)

    def final_state(self, log):
        parts = re.split(r'^pc=', log, flags=re.M)
        self.assertGreater(len(parts), 1, 'no CPU state dump in log')
        return 'pc=' + parts[-1]

    def test_software_interrupt(self):
        """
        The software interrupt is vector 27 sharing IPR003, one of the few
        sources whose IPR number is not its vector number.
        """
        self.set_machine('rx651-r5f5651c')
        image = build_image(SWINT_VECTOR, 3, (ICU_BASE + R_SWINTR, 0x00, 0x01))
        self.assertIn('r7=0x00c0ffee', self.final_state(self.run_image(image)))

    def test_sci4_txi_interrupt(self):
        """
        Enabling transmission with TIE set on SCI4 raises TXI4, vector 83.
        Its priority lives in IPR083: for all but a handful of low-numbered
        sources the RX65N IPR number is simply the vector number, which is
        what Table 15.5 of the hardware manual gives.
        """
        self.set_machine('rx651-r5f5651c')
        image = build_image(SCI4_TXI_VECTOR, SCI4_TXI_VECTOR,
                            (SCI4_BASE, 0x02, 0xA0))   # SCR = TE | TIE
        self.assertIn('r7=0x00c0ffee', self.final_state(self.run_image(image)))

    def test_interrupt_masked_without_priority(self):
        """
        A vector left at priority 0 is never delivered, so the handler does
        not run and the marker never appears.
        """
        self.set_machine('rx651-r5f5651c')
        image = build_image(SCI4_TXI_VECTOR, 0xFF,   # scribble on a spare IPR
                            (SCI4_BASE, 0x02, 0xA0))
        self.assertNotIn('r7=0x00c0ffee',
                         self.final_state(self.run_image(image)))


if __name__ == '__main__':
    QemuSystemTest.main()
