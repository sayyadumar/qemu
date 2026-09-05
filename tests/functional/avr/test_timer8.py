#!/usr/bin/env python3
#
# Functional tests for the ATmega 8-bit timers.
#
# Copyright (c) 2025 QEMU Contributors
#
# This work is licensed under the terms of the GNU GPL, version 2 or
# later.  See the COPYING file in the top-level directory.

from qemu_test import QemuSystemTest, wait_for_console_pattern

from test_spi import Asm, R_TMP, R_TMP2

# Data addresses.  TIMER0 sits in low I/O and TIMER2 in extended I/O, and
# TIMSKn and TIFRn are somewhere else again, so everything goes through
# LDS/STS rather than IN/OUT.
TIMER0 = 0x0044
TIMER2 = 0x00B0
TIMSK0, TIFR0 = 0x006E, 0x0035
TIMSK2, TIFR2 = 0x0070, 0x0037
SREG = 0x3F                     # I/O address

R_TCCRA, R_TCCRB, R_TCNT, R_OCRA, R_OCRB = range(5)

CS_DIV1, CS_DIV8 = 1, 2
# Clock select 3 is where the two timers part company: clk/64 on TIMER0 and
# clk/32 on TIMER2.
CS_3 = 3

WGM_CTC = 0x02                  # TCCRnA WGM01:0 = 10

TOIE, OCIEA, OCIEB = 0x01, 0x02, 0x04
TOV, OCFA, OCFB = 0x01, 0x02, 0x04

# TIMER0 overflow is vector 19 on the ATmega1284P and TIMER2 compare match A
# is vector 10; the core enters vector n at word address (n - 1) * 2.
T0_OVF_VECTOR_WORD = 36
T2_COMPA_VECTOR_WORD = 18


class T8Asm(Asm):
    """Asm plus the few extra instructions the timer tests need."""

    def add(self, d, r):
        return self._w(0x0C00 | ((r & 0x10) << 5) | (d << 4) | (r & 0x0F))

    def push(self, r):
        return self._w(0x920F | (r << 4))

    def pop(self, d):
        return self._w(0x900F | (d << 4))

    def cli(self):
        return self._w(0x94F8)

    def breq_back(self, word):
        return self._w(0xF001 | (((word - self.here() - 1) & 0x7F) << 3))

    def brne_back(self, word):
        return self._w(0xF401 | (((word - self.here() - 1) & 0x7F) << 3))

    def brne_fwd(self, skip):
        return self._w(0xF401 | ((skip & 0x7F) << 3))

    def stsi(self, addr, value):
        self.ldi(R_TMP, value)
        return self.sts(addr, R_TMP)

    def wait_flag(self, tifr, bit):
        """Spin until `bit` shows up in TIFRn."""
        top = self.here()
        self.lds(R_TMP, tifr)
        self.andi(R_TMP, bit)
        return self.breq_back(top)

    def count_flag(self, tifr, bit, counter, limit):
        """
        Wait for `bit` in TIFRn `limit` times over, clearing it each time,
        and leave the number of times it was seen in R`counter`.
        """
        self.ldi(counter, 0)
        top = self.here()
        self.wait_flag(tifr, bit)
        self.ldi(R_TMP, bit)
        self.sts(tifr, R_TMP)           # writing a one clears it
        self.subi(counter, 0xFF)        # counter += 1
        self.cpi(counter, limit)
        return self.brne_back(top)

    def counting_isr(self, counter, limit, timsk):
        """
        A handler that adds one to R`counter` and, once it reaches `limit`,
        turns its own interrupt off through TIMSKn.  It has to stop itself:
        the main line cannot both notice the count and disable interrupts
        without another one landing in between.

        SREG is saved and restored along with the scratch register, so the
        interrupted code's flags survive the visit.
        """
        at = self.here()
        self.push(R_TMP)
        self.inp(R_TMP, SREG)
        self.push(R_TMP)
        self.ldi(R_TMP, 1)
        self.add(counter, R_TMP)
        self.cpi(counter, limit)
        self.brne_fwd(3)
        self.ldi(R_TMP, 0)
        self.sts(timsk, R_TMP)
        self.pop(R_TMP)
        self.out(SREG, R_TMP)
        self.pop(R_TMP)
        self.reti()
        return at


class AVRTimer8Machine(QemuSystemTest):

    timeout = 60

    def run_asm(self, asm, pattern):
        self.set_machine('mega1284')
        path = self.scratch_file('timer8.bin')
        with open(path, 'wb') as f:
            f.write(asm.image())
        self.vm.add_args('-bios', path)
        self.vm.add_args('-nographic')
        self.vm.set_console()
        self.vm.launch()
        wait_for_console_pattern(self, pattern)

    def test_counter_runs(self):
        """
        A stopped counter stays where it is put.  Selecting a clock source
        sets it going, and the loop below only ends once it has moved -- if
        it never does the test hangs rather than passing quietly.
        """
        a = T8Asm()
        a.enable_uart()
        a.stsi(TIMER0 + R_TCNT, 0x40)
        a.lds(20, TIMER0 + R_TCNT)
        for _ in range(8):
            a.nop()
        a.lds(21, TIMER0 + R_TCNT)      # still 0x40, the clock is stopped

        a.stsi(TIMER0 + R_TCCRB, CS_DIV1)
        top = a.here()
        a.lds(22, TIMER0 + R_TCNT)
        a.cpi(22, 0x40)
        a.breq_back(top)

        a.puts('stopped=')
        a.puthex(20)
        a.puthex(21)
        a.puts(' moved\r\n')
        a.spin()

        self.run_asm(a, 'stopped=4040 moved')

    def test_overflow_flag(self):
        """
        In normal mode the counter runs to MAX and rolls over, setting TOV
        as it goes.  With the interrupt masked off the flag is left standing
        for firmware to poll, and writing a one clears it.
        """
        a = T8Asm()
        a.enable_uart()
        a.stsi(TIMER0 + R_TCCRB, CS_DIV1)
        a.count_flag(TIFR0, TOV, 20, 4)
        a.puts('ovf=')
        a.puthex(20)
        a.puts('\r\n')
        a.spin()

        self.run_asm(a, 'ovf=04')

    def test_ctc_compare_flag(self):
        """
        CTC clears the counter at OCRA, so with OCRA below MAX the counter
        never gets there and TOV never turns up -- only OCFA does.
        """
        a = T8Asm()
        a.enable_uart()
        a.stsi(TIMER0 + R_TCCRA, WGM_CTC)
        a.stsi(TIMER0 + R_OCRA, 0x20)
        a.stsi(TIMER0 + R_TCCRB, CS_DIV1)
        a.count_flag(TIFR0, OCFA, 20, 4)
        a.lds(21, TIFR0)
        a.andi(21, TOV)
        a.puts('ctc=')
        a.puthex(20)
        a.puts(' tov=')
        a.puthex(21)
        a.puts('\r\n')
        a.spin()

        self.run_asm(a, 'ctc=04 tov=00')

    def test_overflow_interrupt(self):
        """
        TOIE0 turns the overflow into an interrupt.  The handler counts its
        own entries, so the test waits for the third one rather than just
        the first.
        """
        a = T8Asm()
        a.jmp(0)
        a.pad_to(T0_OVF_VECTOR_WORD)
        a.jmp(0)
        isr = a.counting_isr(18, 3, TIMSK0)

        main = a.here()
        a.enable_uart()
        a.ldi(18, 0)
        a.stsi(TIMSK0, TOIE)
        a.stsi(TIMER0 + R_TCCRB, CS_DIV1)
        a.sei()
        top = a.here()
        a.cpi(18, 3)
        a.brne_back(top)
        a.cli()
        a.puts('irq=')
        a.puthex(18)
        a.puts('\r\n')
        a.spin()

        a.words[1] = main
        a.words[T0_OVF_VECTOR_WORD + 1] = isr

        self.run_asm(a, 'irq=03')

    def test_timer2_prescaler(self):
        """
        TIMER2's clock select does not mean what TIMER0's does.  Give both
        the same TOP and the same encoding, 3, and TIMER2 runs at clk/32
        against TIMER0's clk/64: TIMER2 has to reach its compare value while
        TIMER0 is still only half way there.  TIMER0 is even started first,
        so a shared prescaler table would have it flagged already.
        """
        a = T8Asm()
        a.enable_uart()
        a.stsi(TIMER0 + R_TCCRA, WGM_CTC)
        a.stsi(TIMER0 + R_OCRA, 0x80)
        a.stsi(TIMER2 + R_TCCRA, WGM_CTC)
        a.stsi(TIMER2 + R_OCRA, 0x80)
        a.stsi(TIMER0 + R_TCCRB, CS_3)
        a.stsi(TIMER2 + R_TCCRB, CS_3)

        a.wait_flag(TIFR2, OCFA)
        a.lds(20, TIFR0)
        a.andi(20, OCFA)
        a.wait_flag(TIFR0, OCFA)        # it does get there, just later

        a.puts('t0=')
        a.puthex(20)
        a.puts(' late\r\n')
        a.spin()

        self.run_asm(a, 't0=00 late')

    def test_timer2_compare_interrupt(self):
        """
        The second timer is wired up too: TIMER2 compare match A has its own
        vector and TIMSK2 gates it independently of TIMER0.
        """
        a = T8Asm()
        a.jmp(0)
        a.pad_to(T2_COMPA_VECTOR_WORD)
        a.jmp(0)
        isr = a.counting_isr(18, 5, TIMSK2)

        main = a.here()
        a.enable_uart()
        a.ldi(18, 0)
        a.stsi(TIMER2 + R_TCCRA, WGM_CTC)
        a.stsi(TIMER2 + R_OCRA, 0x10)
        a.stsi(TIMSK2, OCIEA)
        a.stsi(TIMER2 + R_TCCRB, CS_DIV8)
        a.sei()
        top = a.here()
        a.cpi(18, 5)
        a.brne_back(top)
        a.cli()
        a.puts('t2irq=')
        a.puthex(18)
        a.puts('\r\n')
        a.spin()

        a.words[1] = main
        a.words[T2_COMPA_VECTOR_WORD + 1] = isr

        self.run_asm(a, 't2irq=05')


if __name__ == '__main__':
    QemuSystemTest.main()
