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
FACI_BASE = 0x007E0000      # FACI command-issuing area
R_FSADDR = 0x30             # FACI command start address register
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


def make_ofsm(bankmd, bankswp):
    """
    Build an option-setting memory image. Unwritten OFSM reads as all ones,
    which is BANKMD = 111b (linear mode) and BANKSWP = 111b.
    """
    blob = bytearray(b'\xff' * 512)
    struct.pack_into('<I', blob, 0x00, 0xfffffff8 | bankmd)   # MDE
    struct.pack_into('<I', blob, 0x20, 0xfffffff8 | bankswp)  # BANKSEL
    return bytes(blob)


def make_bank_half(marker):
    """
    A 1 MB code flash half whose firmware loads a distinctive marker into R1,
    and whose own reset vector points at the low half of the address map. Which
    marker turns up therefore says which bank got mapped low.
    """
    code = bytes([0xFB, 0x12]) + u32(marker) + bytes([0x2E, 0x00])
    blob = bytearray(b'\xff' * (1024 * 1024))
    blob[0:len(code)] = code
    struct.pack_into('<I', blob, 1024 * 1024 - 4, 0xFFE00000)
    return bytes(blob)


def faci_setup(dest, pe_bits=0x80):
    """
    Enter P/E mode for the selected array and point FSADDR at dest.

    R1 addresses the FACI register block throughout. FENTRYR at +0x84 is a
    word register and FSADDR at +0x30 a long one, and the encoded
    displacement is scaled by the access size, hence 0x42 and 0x0c.
    """
    code = bytes([0xFB, 0x12]) + u32(FCU_BASE)
    code += bytes([0xF9, 0x19, 0x42, pe_bits, 0xAA])   # FENTRYR = key | array
    code += bytes([0xF9, 0x12, 0x0C]) + u32(dest)      # FSADDR = dest
    code += bytes([0xFB, 0x32]) + u32(FACI_BASE)       # R3 = command-issuing
    return code


def faci_byte(value):
    """Write one command byte to the FACI command-issuing area."""
    return bytes([0xF9, 0x34, 0x00, value])


def faci_word(value):
    """Write one 16-bit data word to the FACI command-issuing area."""
    return bytes([0xF9, 0x39, 0x00, value & 0xff, (value >> 8) & 0xff])


def faci_leave():
    """Leave P/E mode so the array reads normally again."""
    return bytes([0xF9, 0x19, 0x42, 0x00, 0xAA])


def last_cpu_state(log):
    """Return the last CPU register dump from a -d cpu log."""
    parts = re.split(r'^pc=', log, flags=re.M)
    assert len(parts) > 1, 'no CPU state dump in log'
    return 'pc=' + parts[-1]


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
        Drive a full FACI program sequence. Commands and data go to the
        command-issuing area at 0x007E0000, not to the flash array; the
        destination comes from FSADDR. Data flash takes two 16-bit words per
        program command.
        """
        self.set_machine('rx65n-r5f565ne-evk')

        code = faci_setup(DFLASH_BASE)
        code += faci_byte(0xE8)          # program
        code += faci_byte(0x02)          # two data words
        code += faci_word(0x1234)
        code += faci_word(0x5678)
        code += faci_byte(0xD0)          # confirm
        code += faci_leave()
        code += bytes([0xFB, 0x52]) + u32(DFLASH_BASE)
        code += bytes([0xA8, 0x56])      # R6 = first long of the data flash
        code += bytes([0x2E, 0x00])

        log = self.run_image(make_image(code))
        self.assertNotIn('renesas-rx-fcu:', log)
        self.assertIn('r6=0x56781234', last_cpu_state(log))

    def test_fcmdr_records_command_pair(self):
        """
        FCMDR pairs the command just received (CMDR, low byte) with the one
        before it (PCMDR, high byte), so the two-byte block erase sequence
        leaves 0x20d0 behind.
        """
        self.set_machine('rx65n-r5f565ne-evk')

        code = faci_setup(DFLASH_BASE)
        code += faci_byte(0x20)          # block erase
        code += faci_byte(0xD0)          # confirm
        code += faci_leave()
        code += bytes([0xFB, 0x52]) + u32(FCU_BASE + 0xA0)
        code += bytes([0xA8, 0x56])      # R6 = FCMDR
        code += bytes([0x2E, 0x00])

        log = self.run_image(make_image(code))
        self.assertNotIn('renesas-rx-fcu:', log)
        self.assertIn('r6=0x000020d0', last_cpu_state(log))

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
            code = faci_setup(DFLASH_BASE)
            code += faci_byte(0xE8)
            code += faci_byte(0x02)
            code += faci_word(0x1234)
            code += faci_word(0x5678)
            code += faci_byte(0xD0)
            code += faci_leave()
            code += bytes([0xFB, 0x52]) + u32(DFLASH_BASE)
            code += bytes([0xA8, 0x56])
            code += bytes([0x2E, 0x00])
            log = self.run_image(make_image(code), drives=((1, dflash),))
            self.assertIn('r6=0x56781234', last_cpu_state(log))

            # It reached the image on disk.
            with open(dflash, 'rb') as f:
                self.assertEqual(f.read(4), b'\x34\x12\x78\x56')

            # A second, read-only firmware sees it in a fresh QEMU.
            readback = bytes([0xFB, 0x52]) + u32(DFLASH_BASE)
            readback += bytes([0xA8, 0x56])
            readback += bytes([0x2E, 0x00])
            log = self.run_image(make_image(readback), drives=((1, dflash),))
            self.assertIn('r6=0x56781234', last_cpu_state(log))

            # Without the drive the array is erased again.
            log = self.run_image(make_image(readback))
            self.assertIn('r6=0xffffffff', last_cpu_state(log))
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



class RXDualBankMachine(QemuSystemTest):
    """
    Dual mode splits the 2 MB code flash into two 1 MB banks whose position
    in the address map is chosen by BANKSEL.BANKSWP, sampled at reset. That
    is what lets an update program the inactive bank and reboot into it.
    """

    timeout = 30

    def run_banked(self, ofsm_bankmd, ofsm_bankswp):
        cflash = make_bank_half(0x11111111) + make_bank_half(0xB0B0B0B0)
        with tempfile.NamedTemporaryFile(suffix='.img', delete=False) as f:
            f.write(cflash)
            cf = f.name
        with tempfile.NamedTemporaryFile(suffix='.img', delete=False) as f:
            f.write(make_ofsm(ofsm_bankmd, ofsm_bankswp))
            of = f.name
        try:
            cmd = [self.qemu_bin, '-machine', 'rsk-rx65n-2mb', '-nographic',
                   '-d', 'cpu',
                   '-drive', 'if=pflash,unit=0,format=raw,file=%s' % cf,
                   '-drive', 'if=pflash,unit=2,format=raw,file=%s' % of]
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
            os.unlink(cf)
            os.unlink(of)

    def test_linear_mode(self):
        """BANKMD = 111b leaves the array flat: the low half stays low."""
        self.set_machine('rsk-rx65n-2mb')
        state = last_cpu_state(self.run_banked(0x7, 0x7))
        self.assertIn('r1=0x11111111', state)

    def test_dual_mode_unswapped(self):
        """
        BANKMD = 000b with BANKSWP = 111b keeps bank 0 in the upper half,
        which is the layout a linearly programmed image already has, so the
        result matches linear mode.
        """
        self.set_machine('rsk-rx65n-2mb')
        state = last_cpu_state(self.run_banked(0x0, 0x7))
        self.assertIn('r1=0x11111111', state)

    def test_dual_mode_swapped(self):
        """
        BANKSWP = 000b exchanges the banks, so the same address now runs the
        other bank's image.
        """
        self.set_machine('rsk-rx65n-2mb')
        state = last_cpu_state(self.run_banked(0x0, 0x0))
        self.assertIn('r1=0xb0b0b0b0', state)


if __name__ == '__main__':
    QemuSystemTest.main()
