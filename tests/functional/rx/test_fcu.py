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

    def run_image(self, image, drives=(), bios=True):
        with tempfile.NamedTemporaryFile(suffix='.bin', delete=False) as f:
            f.write(image)
            path = f.name
        try:
            cmd = [self.qemu_bin, '-machine', 'rx65n-r5f565ne-evk',
                   '-nographic', '-d', 'cpu,guest_errors']
            if bios:
                cmd += ['-bios', path]
            for unit, image_path in drives:
                cmd += ['-drive', 'if=pflash,unit=%d,format=raw,file=%s'
                        % (unit, image_path)]
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


    def test_fcmdr_records_command_pair(self):
        """
        FCMDR pairs the command just received (CMDR, low byte) with the one
        before it (PCMDR, high byte), so the two-byte block erase sequence
        leaves 0x20d0 behind.
        """
        self.set_machine('rx65n-r5f565ne-evk')

        code = b''
        code += bytes([0xFB, 0x12]) + u32(FCU_BASE)
        code += bytes([0xF9, 0x19, 0x42, 0x80, 0xAA])   # FENTRYR = key | DF
        code += bytes([0xFB, 0x32]) + u32(DFLASH_BASE)
        code += bytes([0xF9, 0x34, 0x00, 0x20])         # block erase
        code += bytes([0xF9, 0x34, 0x00, 0xD0])         # confirm
        code += bytes([0xF9, 0x19, 0x42, 0x00, 0xAA])   # leave P/E mode
        code += bytes([0xFB, 0x52]) + u32(FCU_BASE + 0xA0)
        code += bytes([0xA8, 0x56])                     # R6 = FCMDR
        code += bytes([0x2E, 0x00])

        log = self.run_image(make_image(code))
        self.assertNotIn('renesas-rx-fcu:', log)
        self.assertIn('r6=0x000020d0', self.final_state(log))

    def test_data_flash_persists_across_runs(self):
        """
        A data flash image supplied with -drive keeps what the guest
        programmed into it, so a later run sees the value.
        """
        self.set_machine('rx65n-r5f565ne-evk')

        with tempfile.NamedTemporaryFile(suffix='.img', delete=False) as f:
            f.write(b'\xff' * (32 * 1024))
            dflash = f.name
        try:
            # First run programs 0x1234 into the first half word.
            code = b''
            code += bytes([0xFB, 0x12]) + u32(FCU_BASE)
            code += bytes([0xF9, 0x19, 0x42, 0x80, 0xAA])
            code += bytes([0xFB, 0x32]) + u32(DFLASH_BASE)
            code += bytes([0xF9, 0x34, 0x00, 0xE8])
            code += bytes([0xF9, 0x34, 0x00, 0x01])
            code += bytes([0xF9, 0x39, 0x00, 0x34, 0x12])
            code += bytes([0xF9, 0x34, 0x00, 0xD0])
            code += bytes([0xF9, 0x19, 0x42, 0x00, 0xAA])
            code += bytes([0xA8, 0x34])
            code += bytes([0x2E, 0x00])
            log = self.run_image(make_image(code), drives=((1, dflash),))
            self.assertIn('r4=0xffff1234', self.final_state(log))

            # It reached the image on disk.
            with open(dflash, 'rb') as f:
                self.assertEqual(f.read(4), b'\x34\x12\xff\xff')

            # A second, read-only firmware sees it in a fresh QEMU.
            readback = b''
            readback += bytes([0xFB, 0x32]) + u32(DFLASH_BASE)
            readback += bytes([0xA8, 0x34])
            readback += bytes([0x2E, 0x00])
            log = self.run_image(make_image(readback), drives=((1, dflash),))
            self.assertIn('r4=0xffff1234', self.final_state(log))

            # Without the drive the array is erased again.
            log = self.run_image(make_image(readback))
            self.assertIn('r4=0xffffffff', self.final_state(log))
        finally:
            os.unlink(dflash)

    def test_boot_from_code_flash_drive(self):
        """
        A code flash image supplied with -drive is bootable on its own: the
        reset vector is taken from it even though no -bios was given.
        """
        self.set_machine('rx65n-r5f565ne-evk')

        code = b''
        code += bytes([0xFB, 0x32]) + u32(DFLASH_BASE)
        code += bytes([0xA8, 0x34])
        code += bytes([0x2E, 0x00])

        with tempfile.NamedTemporaryFile(suffix='.img', delete=False) as f:
            f.write(make_image(code))
            cflash = f.name
        try:
            log = self.run_image(b'', drives=((0, cflash),), bios=False)
            # It ran the firmware out of the drive rather than sitting at 0.
            self.assertIn('r3=0x00100000', self.final_state(log))
            self.assertIn('r4=0xffffffff', self.final_state(log))
        finally:
            os.unlink(cflash)


if __name__ == '__main__':
    QemuSystemTest.main()
