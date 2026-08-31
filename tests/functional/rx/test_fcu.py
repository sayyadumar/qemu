#!/usr/bin/env python3
#
# Functional tests for the Renesas RX Flash Control Unit (FACI).
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

FCU_BASE = 0x007FE000
DFLASH_BASE = 0x00100000
CFLASH_BASE = 0xFFF80000
CFLASH_SIZE = 512 * 1024


def u32(value):
    return struct.pack('<I', value & 0xffffffff)


def make_image(code):
    blob = bytearray(b'\xff' * CFLASH_SIZE)
    blob[0:len(code)] = code
    struct.pack_into('<I', blob, CFLASH_SIZE - 4, CFLASH_BASE)
    return bytes(blob)


class RXFcuMachine(QemuSystemTest):

    timeout = 30

    def run_image(self, image):
        with tempfile.NamedTemporaryFile(suffix='.bin', delete=False) as f:
            f.write(image)
            path = f.name
        try:
            cmd = [self.qemu_bin, '-machine', 'rx65n-r5f565ne-evk',
                   '-bios', path, '-nographic',
                   '-d', 'cpu,guest_errors']
            proc = subprocess.run(cmd, capture_output=True, timeout=10,
                                  text=True, errors='replace')
            return proc.stdout + proc.stderr
        except subprocess.TimeoutExpired as exc:
            out = exc.stdout or b''
            err = exc.stderr or b''
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

    def test_erased_flash_reads_ones(self):
        """
        Flash that has never been programmed reads as all ones, not as the
        zeroes its backing memory starts out as. Firmware that blank-checks a
        region, or a bootloader testing whether a slot is empty, depends on
        this.
        """
        self.set_machine('rx65n-r5f565ne-evk')

        code = b''
        code += bytes([0xFB, 0x12]) + u32(FCU_BASE + 0x80)   # R1 = &FSTATR
        code += bytes([0xA8, 0x12])                          # R2 = *R1
        code += bytes([0xFB, 0x32]) + u32(DFLASH_BASE)       # R3 = data flash
        code += bytes([0xA8, 0x34])                          # R4 = *R3
        code += bytes([0x2E, 0x00])

        state = self.final_state(self.run_image(make_image(code)))
        # FSTATR reports the sequencer ready (FRDY, b6).
        self.assertIn('r2=0x00000040', state)
        # Unprogrammed data flash reads as erased.
        self.assertIn('r4=0xffffffff', state)

    def test_faci_program_data_flash(self):
        """
        Drive a full FACI program sequence against the data flash: enter P/E
        mode through FENTRYR, issue the program command with a word count and
        one data word, confirm, leave P/E mode and read the value back.

        Programming can only clear bits, so writing 0x1234 into an erased
        half word leaves 0xffff1234 in the first long word.
        """
        self.set_machine('rx65n-r5f565ne-evk')

        code = b''
        code += bytes([0xFB, 0x12]) + u32(FCU_BASE)      # R1 = FCU base
        # FENTRYR at +0x84 is a word register, and the encoded displacement
        # is scaled by the access size, so 0x84 is written as 0x42.
        code += bytes([0xF9, 0x19, 0x42, 0x80, 0xAA])    # FENTRYR = key | DF
        code += bytes([0xFB, 0x32]) + u32(DFLASH_BASE)   # R3 = data flash
        code += bytes([0xF9, 0x34, 0x00, 0xE8])          # program command
        code += bytes([0xF9, 0x34, 0x00, 0x01])          # one 16-bit word
        code += bytes([0xF9, 0x39, 0x00, 0x34, 0x12])    # the data word
        code += bytes([0xF9, 0x34, 0x00, 0xD0])          # confirm
        code += bytes([0xF9, 0x19, 0x42, 0x00, 0xAA])    # leave P/E mode
        code += bytes([0xA8, 0x34])                      # R4 = *R3
        code += bytes([0x2E, 0x00])

        log = self.run_image(make_image(code))
        # The sequence should be accepted without the FCU complaining.
        self.assertNotIn('renesas-rx-fcu:', log)
        self.assertIn('r4=0xffff1234', self.final_state(log))


if __name__ == '__main__':
    QemuSystemTest.main()
