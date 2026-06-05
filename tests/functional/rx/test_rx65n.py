#!/usr/bin/env python3
#
# Functional test for the rx65n-r5f565ne-evk machine.
#
# Verifies that the RX65N machine boots correctly:
#   - Reads reset vector from 0xFFFFFFFC
#   - Executes from flash base (0xFFF80000)
#   - Drives SCI0 UART output via memory-mapped registers
#
# Copyright (c) 2024 QEMU Contributors
#
# This work is licensed under the terms of the GNU GPL, version 2 or
# later.  See the COPYING file in the top-level directory.

import os
import struct
import tempfile

from qemu_test import QemuSystemTest, wait_for_console_pattern


def build_firmware():
    """
    Build a minimal 512KB RX65N firmware image that:
      1. Loads SCI0 base address (0x00088240) into R0
      2. Enables TX+RX via SCR register (offset 2)
      3. Outputs 'O','K','\\r','\\n' to TDR (offset 3), polling TDRE bit
         (SSR bit7, offset 4) between each character
      4. Loops forever

    All instructions hand-encoded from RX ISA specification.
    """
    size = 512 * 1024
    blob = bytearray(b'\xff' * size)

    def put(off, *bs):
        for i, b in enumerate(bs):
            blob[off + i] = b

    off = 0
    SCI0_BASE = 0x00088240

    # MOV.L #0x00088240, R0  (rd=0, li=0=4-byte-imm, sz=2=L)
    # Encoding: FB 02 <LE32>
    put(off, 0xFB, 0x02,
        SCI0_BASE & 0xFF, (SCI0_BASE >> 8) & 0xFF,
        (SCI0_BASE >> 16) & 0xFF, (SCI0_BASE >> 24) & 0xFF)
    off += 6

    # MOV.B #0x30, 2[R0]  (SCR = TE|RE; rd=0, li=1=1-byte-imm, sz=0=B)
    # Encoding: F9 04 <dsp8=2> <imm8=0x30>
    put(off, 0xF9, 0x04, 0x02, 0x30)
    off += 4

    chars = [ord('O'), ord('K'), ord('\r'), ord('\n')]

    # Write first char directly (TEND=1 right after enabling TE)
    # MOV.B #ch, 3[R0]  (TDR; rd=0, li=1, sz=0)
    put(off, 0xF9, 0x04, 0x03, chars[0])
    off += 4

    # Remaining chars: poll TDRE (SSR bit7) then write
    for ch in chars[1:]:
        # poll:
        poll_start = off
        # MOV.B 4[R0], R1  (read SSR; dsp=4, rs=R0=0, rd=R1=1, sz=0)
        # Byte1: 10001 dsp[4:2]=001 = 0x89
        # Byte2: dsp[1]=0 rs=000 dsp[0]=0 rd=001 = 0x01
        put(off, 0x89, 0x01)
        off += 2
        # AND #0x80, R1  (rd=1, li=1=1-byte-imm)
        # Encoding: 75 21 80  (0111 01 01 = 0x75, 0010 0001 = 0x21)
        put(off, 0x75, 0x21, 0x80)
        off += 3
        # BEQ poll  (cd=0=BZ; target = poll_start; dsp = poll_start - off)
        dsp = poll_start - off
        put(off, 0x20, dsp & 0xFF)
        off += 2
        # MOV.B #ch, 3[R0]
        put(off, 0xF9, 0x04, 0x03, ch)
        off += 4

    # BRA.b 0  (self-loop; dsp=0 -> target = this instruction's address)
    put(off, 0x2E, 0x00)

    # Reset vector at 0x7FFFC points to flash base 0xFFF80000
    struct.pack_into('<I', blob, 0x7FFFC, 0xFFF80000)

    return bytes(blob)


class RX65NMachine(QemuSystemTest):

    timeout = 30

    def test_uart_boot(self):
        """
        Boot the rx65n-r5f565ne-evk machine with a minimal bare-metal
        firmware and verify that SCI0 UART output is produced correctly.
        """
        fw = build_firmware()
        with tempfile.NamedTemporaryFile(suffix='.bin', delete=False) as f:
            f.write(fw)
            fw_path = f.name

        try:
            self.set_machine('rx65n-r5f565ne-evk')
            self.vm.add_args('-bios', fw_path)
            self.vm.set_console()
            self.vm.launch()
            wait_for_console_pattern(self, 'OK')
        finally:
            os.unlink(fw_path)


if __name__ == '__main__':
    QemuSystemTest.main()
