#!/usr/bin/env python3
#
# Functional tests for RXv3 (rsk-rx72m) and for ISA revision gating.
#
# Verifies that:
#   - RXv3 instructions execute correctly on an RXv3 core (rsk-rx72m)
#   - RXv3 instructions are rejected on an RXv2 core (rx65n)
#   - RXv2 instructions are rejected on an RXv1 core (gdbsim-r5f562n8)
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


def make_image(flash_base, size, code):
    """
    Wrap hand-assembled code in a flash image whose reset vector, at the
    top of the image, points at the start of the code.
    """
    blob = bytearray(b'\xff' * size)
    blob[0:len(code)] = code
    struct.pack_into('<I', blob, size - 4, flash_base)
    return bytes(blob)


def u32(value):
    return struct.pack('<I', value & 0xffffffff)


# DADD DR0, DR1, DR2 -- an RXv3 DPFPU instruction, then a self-loop.
RXV3_INSN = bytes([0x76, 0x90, 0x10, 0x20, 0x2E, 0x00])
# STZ #1, R1 -- an RXv2 instruction, then a self-loop.
RXV2_INSN = bytes([0xFD, 0x74, 0xE1, 0x01, 0x2E, 0x00])


class RXv3Machine(QemuSystemTest):

    timeout = 30

    def run_image(self, machine, image, extra_args=()):
        """
        Run an image to the point where it settles into its self-loop and
        return the combined -d in_asm,cpu log.
        """
        with tempfile.NamedTemporaryFile(suffix='.bin', delete=False) as f:
            f.write(image)
            path = f.name
        try:
            cmd = [self.qemu_bin, '-machine', machine, '-bios', path,
                   '-nographic', '-d', 'in_asm,cpu'] + list(extra_args)
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
        """
        Return the last CPU register dump from a -d cpu log.

        The log holds one dump per translation block, so matching against the
        whole thing would happily accept a value the program only held
        transiently. Assertions about results belong against this.
        """
        parts = re.split(r'^pc=', log, flags=re.M)
        self.assertGreater(len(parts), 1, 'no CPU state dump in log')
        return 'pc=' + parts[-1]

    def test_rxv3_dpfpu(self):
        """
        Exercise the RXv3 DPFPU, bit field and register bank instructions on
        the RXv3 rsk-rx72m machine and check the resulting register state.
        """
        self.set_machine('rsk-rx72m')

        code = b''
        # DMOV.D #0x3FF00000, DRH0 -> DR0 = 1.0
        code += bytes([0xF9, 0x03, 0x03]) + u32(0x3FF00000)
        # DMOV.D #0x40000000, DRH1 -> DR1 = 2.0
        code += bytes([0xF9, 0x03, 0x13]) + u32(0x40000000)
        # DADD DR0, DR1, DR2 -> DR2 = 3.0
        code += bytes([0x76, 0x90, 0x10, 0x20])
        # DMUL DR0, DR1, DR3 -> DR3 = 2.0
        code += bytes([0x76, 0x90, 0x12, 0x30])
        # DMOV.L DRH2, R1 -> R1 = 0x40080000
        code += bytes([0xFD, 0x75, 0x81, 0x22])
        # MOV.L #0x12345678, R4
        code += bytes([0xFB, 0x42]) + u32(0x12345678)
        # XOR R4, R4, R5 -> R5 = 0 (three-operand XOR is RXv3)
        code += bytes([0xFF, 0x65, 0x44])
        # MVTDC R4, DPSW ; MVFDC DPSW, R6. DPSW does not take the value
        # verbatim: reserved bits drop out, the DC* cause bits cannot be set
        # by writing 1, and DFS is recomputed. 0x12345678 lands as
        # 0x90005400, which is what MVFDC reads back.
        code += bytes([0xFD, 0x77, 0x84, 0x04])
        code += bytes([0xFD, 0x75, 0x86, 0x04])
        # SAVE #1 ; MOV.L #0, R1 ; RSTR #1 -> R1 is restored
        code += bytes([0xFD, 0x76, 0xE0, 0x01])
        code += bytes([0x66, 0x01])
        code += bytes([0xFD, 0x76, 0xF0, 0x01])
        # BFMOVZ #0, #8, #8, R4, R7 -> R7 = (R4 & 0xff) << 8 = 0x7800
        bf = (16 << 10) | (8 << 5) | 8
        code += bytes([0xFC, 0x5A, 0x47]) + struct.pack('<H', bf)
        code += bytes([0x2E, 0x00])

        log = self.run_image('rsk-rx72m',
                             make_image(0xFFC00000, 4 * 1024 * 1024, code))

        # Instructions decoded as expected.
        for mnemonic in ('dmov.d', 'dadd', 'dmul', 'dmov.l', 'xor',
                         'mvtdc', 'mvfdc', 'save', 'rstr', 'bfmovz'):
            self.assertIn(mnemonic, log)

        # Arithmetic and transfers produced the expected values.
        state = self.final_state(log)
        self.assertIn('dr0=0x3ff0000000000000', state)
        self.assertIn('dr2=0x4008000000000000', state)   # 1.0 + 2.0
        self.assertIn('dr3=0x4000000000000000', state)   # 1.0 * 2.0
        self.assertIn('dpsw=0x90005400', state)          # MVTDC write rules
        self.assertIn('r4=0x12345678 r5=0x00000000 r6=0x90005400 '
                      'r7=0x00007800', state)            # XOR/MVFDC/BFMOVZ
        self.assertIn('r1=0x40080000', state)            # survived SAVE/RSTR

    def test_rxv3_memory_and_stack(self):
        """
        DMOV.D to and from memory, and DPUSHM.D/DPOPM.D round trips.
        """
        self.set_machine('rsk-rx72m')

        code = b''
        code += bytes([0xFB, 0x52]) + u32(0x00010000)   # R5 = scratch
        code += bytes([0xFB, 0x02]) + u32(0x00020000)   # R0 = stack
        code += bytes([0xF9, 0x03, 0x03]) + u32(0x41200000)
        code += bytes([0xF9, 0x03, 0x13]) + u32(0x41300000)
        code += bytes([0xFC, 0x78, 0x58, 0x00])         # DMOV.D DR0, [R5]
        code += bytes([0xFC, 0x79, 0x58, 0x02, 0x10])   # DMOV.D DR1, 8[R5] (dsp*4)
        code += bytes([0xFC, 0xC8, 0x58, 0x40])         # DMOV.D [R5], DR4
        code += bytes([0xFC, 0xC9, 0x58, 0x02, 0x50])   # DMOV.D 8[R5], DR5 (dsp*4)
        code += bytes([0x75, 0xB0, 0x01])               # DPUSHM.D DR0-DR1
        code += bytes([0x75, 0xB8, 0x61])               # DPOPM.D  DR6-DR7
        # Pin the displacement scale: read back what the dsp=2 store wrote
        # using a base register pointing at scratch+8 and no displacement.
        # Only a scale of 4 puts DR1 there; a scale of 8 would have put it
        # at scratch+16 and this would load zero.
        code += bytes([0xFB, 0x62]) + u32(0x00010008)   # R6 = scratch + 8
        code += bytes([0xFC, 0xC8, 0x68, 0x80])         # DMOV.D [R6], DR8
        code += bytes([0x2E, 0x00])

        log = self.run_image('rsk-rx72m',
                             make_image(0xFFC00000, 4 * 1024 * 1024, code))

        # Values survived the store/load round trip, including the
        # displacement scaled by 4, and the push/pop pair.
        state = self.final_state(log)
        self.assertIn('dr4=0x4120000000000000 dr5=0x4130000000000000', state)
        self.assertIn('dr6=0x4120000000000000 dr7=0x4130000000000000', state)
        self.assertIn('r0=0x00020000', state)   # stack pointer restored
        # dsp=2 addressed scratch+8, so the .D displacement scale is 4.
        self.assertIn('dr8=0x4130000000000000', state)

    def test_rxv3_dcmp_mvfdr(self):
        """
        DCMP records its answer in DCMR.RES and MVFDR moves that into PSW.Z,
        so a conditional branch after the pair sees the comparison result.

        The manual defines every DCMP relation as "src2 REL src", so the
        second operand is the left hand side; both operand orders are tested
        here because getting that backwards still passes a one-sided test.
        """
        self.set_machine('rsk-rx72m')

        code = b''
        code += bytes([0xF9, 0x03, 0x03]) + u32(0x3FF00000)  # DR0 = 1.0
        code += bytes([0xF9, 0x03, 0x13]) + u32(0x40000000)  # DR1 = 2.0
        # DCMPlt src=DR1, src2=DR0 -> RES = (DR0 < DR1) = 1.0 < 2.0 = true
        code += bytes([0x76, 0x90, 0x08, 0x41])
        code += bytes([0x75, 0x90, 0x1B])   # MVFDR -> Z = 1
        code += bytes([0x66, 0x11])         # R1 = 1
        code += bytes([0x20, 0x04])         # BEQ +4, taken, skips the clear
        code += bytes([0x66, 0x01])         # R1 = 0 (skipped)
        # DCMPlt src=DR0, src2=DR1 -> RES = (DR1 < DR0) = 2.0 < 1.0 = false.
        # Comparing the operands the wrong way round makes this one true.
        code += bytes([0x76, 0x90, 0x18, 0x40])
        code += bytes([0x75, 0x90, 0x1B])   # MVFDR -> Z = 0
        code += bytes([0x66, 0x12])         # R2 = 1
        code += bytes([0x20, 0x04])         # BEQ +4, not taken
        code += bytes([0x66, 0x02])         # R2 = 0 (executed)
        code += bytes([0x2E, 0x00])

        log = self.run_image('rsk-rx72m',
                             make_image(0xFFC00000, 4 * 1024 * 1024, code))

        self.assertIn('dcmplt', log)
        self.assertIn('mvfdr', log)
        # R1 kept its value (branch taken), R2 was cleared (not taken).
        # Comparing the operands the wrong way round inverts both.
        state = self.final_state(log)
        self.assertIn('r1=0x00000001 r2=0x00000000', state)
        # The second, false comparison left DCMR.RES clear.
        self.assertIn('dcmr=0x00000000', state)

    def test_rxv3_dpsw_write_rules(self):
        """
        MVTDC does not write DPSW verbatim: reserved bits read back as 0,
        the DC* cause bits clear on a written 0 but keep their value on a
        written 1 (so they cannot be set from software), and DFS is a
        read-only summary of DFV, DFO, DFZ and DFU.

        Writing all ones therefore reads back as 0xfc007d03: DRM keeps both
        bits, the causes stay clear, the enables and DF* flags are set, the
        reserved bits drop out and DFS is computed.
        """
        self.set_machine('rsk-rx72m')

        code = b''
        code += bytes([0xFB, 0x42]) + u32(0xFFFFFFFF)  # R4 = all ones
        code += bytes([0xFD, 0x77, 0x84, 0x04])        # MVTDC R4, DPSW
        code += bytes([0xFD, 0x75, 0x86, 0x04])        # MVFDC DPSW, R6
        code += bytes([0x2E, 0x00])

        log = self.run_image('rsk-rx72m',
                             make_image(0xFFC00000, 4 * 1024 * 1024, code))
        state = self.final_state(log)
        self.assertIn('dpsw=0xfc007d03', state)
        self.assertIn('r6=0xfc007d03', state)

    def test_rxv3_dpsw_rounding_mode(self):
        """
        DPSW.DRM selects the double-precision rounding mode, independently of
        the FPSW rounding mode used by the single-precision unit.

        1.0/10.0 is inexact, so the mode is visible in the result: rounding
        to nearest gives ...99a and rounding towards zero truncates to ...999.
        The inexact cause and flag bits should be set either way.
        """
        self.set_machine('rsk-rx72m')

        code = b''
        code += bytes([0xF9, 0x03, 0x03]) + u32(0x3FF00000)  # DR0 = 1.0
        code += bytes([0xF9, 0x03, 0x13]) + u32(0x40240000)  # DR1 = 10.0
        code += bytes([0x76, 0x90, 0x15, 0x20])   # DDIV -> DR2, DRM = nearest
        code += bytes([0x66, 0x14])               # R4 = 1
        code += bytes([0xFD, 0x77, 0x84, 0x04])   # MVTDC R4, DPSW: DRM = to 0
        code += bytes([0x76, 0x90, 0x15, 0x30])   # DDIV -> DR3, DRM = to zero
        code += bytes([0x2E, 0x00])

        log = self.run_image('rsk-rx72m',
                             make_image(0xFFC00000, 4 * 1024 * 1024, code))
        state = self.final_state(log)
        self.assertIn('dr2=0x3fb999999999999a', state)   # round to nearest
        self.assertIn('dr3=0x3fb9999999999999', state)   # round towards zero
        # DRM kept, and the inexact cause (DCX, b6) and flag (DFX, b30) set.
        self.assertIn('dpsw=0x40000041', state)

    def test_rxv3_insn_rejected_on_rxv2(self):
        """
        An RXv3 DPFPU instruction must raise an illegal instruction
        exception on the RXv2 RX65N core.
        """
        self.set_machine('rx65n-r5f565ne-evk')
        log = self.run_image('rx65n-r5f565ne-evk',
                             make_image(0xFFF80000, 512 * 1024, RXV3_INSN),
                             extra_args=('-d', 'in_asm,int'))
        self.assertIn('illegal instruction', log)

    def test_rxv2_insn_accepted_on_rxv2(self):
        """
        The RXv2 instruction used by the gating test does run on RX65N, so
        the rejection below is about the ISA revision and nothing else.
        """
        self.set_machine('rx65n-r5f565ne-evk')
        log = self.run_image('rx65n-r5f565ne-evk',
                             make_image(0xFFF80000, 512 * 1024, RXV2_INSN),
                             extra_args=('-d', 'in_asm,int'))
        self.assertIn('stz', log)
        self.assertNotIn('illegal instruction', log)

    def test_rxv2_insn_rejected_on_rxv1(self):
        """
        The same RXv2 instruction must raise an illegal instruction
        exception on the RXv1 RX62N core.
        """
        self.set_machine('gdbsim-r5f562n8')
        log = self.run_image('gdbsim-r5f562n8',
                             make_image(0xFFF80000, 512 * 1024, RXV2_INSN),
                             extra_args=('-d', 'in_asm,int'))
        self.assertIn('illegal instruction', log)


if __name__ == '__main__':
    QemuSystemTest.main()
