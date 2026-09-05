#!/usr/bin/env python3
#
# Functional tests for the ATmega watchdog timer.
#
# Copyright (c) 2025 QEMU Contributors
#
# This work is licensed under the terms of the GNU GPL, version 2 or
# later.  See the COPYING file in the top-level directory.

from qemu_test import QemuSystemTest, wait_for_console_pattern

from test_spi import Asm, R_TMP

WDTCSR = 0x0060                 # data address, extended I/O
SREG = 0x3F                     # I/O address

WDE = 0x08
WDCE = 0x10
WDIE = 0x40
WDIF = 0x80

# The shortest prescaler, about 16 ms.
WDP_16MS = 0x00

# TIMER0, used as a stopwatch that runs on the system clock rather than on
# however fast the host happens to get through a delay loop.
TCCR0B = 0x0045
TIFR0 = 0x0035
T0_CS_DIV1024 = 5
TOV = 0x01

# Watchdog time-out is vector 9 on the ATmega1284P, entered at word address
# (9 - 1) * 2.
WDT_VECTOR_WORD = 16

# Somewhere in SRAM that nothing else touches.  A reset does not clear it.
MARK = 0x0300
MARK_MAGIC = 0x5A


class WdtAsm(Asm):

    def push(self, r):
        return self._w(0x920F | (r << 4))

    def pop(self, d):
        return self._w(0x900F | (d << 4))

    def wdr(self):
        return self._w(0x95A8)

    def cli(self):
        return self._w(0x94F8)

    def breq_back(self, word):
        return self._w(0xF001 | (((word - self.here() - 1) & 0x7F) << 3))

    def brne_back(self, word):
        return self._w(0xF401 | (((word - self.here() - 1) & 0x7F) << 3))

    def stsi(self, addr, value):
        self.ldi(R_TMP, value)
        return self.sts(addr, R_TMP)

    def wdt_arm(self, value):
        """The timed sequence: WDCE and WDE together, then the real value."""
        self.stsi(WDTCSR, WDCE | WDE)
        return self.stsi(WDTCSR, value)

    def marking_isr(self, mark, saved):
        """
        A handler that records WDTCSR as the hardware left it in R`saved`,
        sets R`mark` to one, and stops the watchdog so nothing else fires.
        """
        at = self.here()
        self.push(R_TMP)
        self.inp(R_TMP, SREG)
        self.push(R_TMP)
        self.lds(saved, WDTCSR)
        self.ldi(mark, 1)
        self.wdt_arm(0)
        self.pop(R_TMP)
        self.out(SREG, R_TMP)
        self.pop(R_TMP)
        self.reti()
        return at


class AVRWdtMachine(QemuSystemTest):

    timeout = 60

    def run_asm(self, asm, pattern):
        self.set_machine('mega1284')
        path = self.scratch_file('wdt.bin')
        with open(path, 'wb') as f:
            f.write(asm.image())
        self.vm.add_args('-bios', path)
        self.vm.add_args('-nographic')
        self.vm.set_console()
        self.vm.launch()
        wait_for_console_pattern(self, pattern)

    def test_interrupt_mode(self):
        """
        WDIE on its own turns a time-out into an interrupt.  The hardware
        raises WDIF and clears WDIE as it takes it, so an unattended second
        time-out falls through to whatever WDE says.
        """
        a = WdtAsm()
        a.jmp(0)
        a.pad_to(WDT_VECTOR_WORD)
        a.jmp(0)
        isr = a.marking_isr(18, 19)

        main = a.here()
        a.enable_uart()
        a.ldi(18, 0)
        a.wdt_arm(WDIE | WDP_16MS)
        a.sei()
        top = a.here()
        a.cpi(18, 1)
        a.brne_back(top)
        a.cli()
        a.mov(20, 19)
        a.andi(20, WDIE)
        a.mov(21, 19)
        a.andi(21, WDIF)
        a.puts('fired=')
        a.puthex(18)
        a.puts(' wdie=')
        a.puthex(20)
        a.puts(' wdif=')
        a.puthex(21)
        a.puts('\r\n')
        a.spin()

        a.words[1] = main
        a.words[WDT_VECTOR_WORD + 1] = isr

        self.run_asm(a, 'fired=01 wdie=00 wdif=80')

    def test_wdr_holds_it_off(self):
        """
        WDR restarts the count, so a loop that keeps kicking never reaches
        the time-out.  Kick for twenty TIMER0 periods at clk/1024, which is
        around a third of a second and some twenty watchdog periods, then
        stop and wait for it -- proving it was armed the whole way through.
        """
        a = WdtAsm()
        a.jmp(0)
        a.pad_to(WDT_VECTOR_WORD)
        a.jmp(0)
        isr = a.marking_isr(18, 19)

        main = a.here()
        a.enable_uart()
        a.ldi(18, 0)
        a.stsi(TCCR0B, T0_CS_DIV1024)
        a.wdt_arm(WDIE | WDP_16MS)
        a.sei()

        a.ldi(20, 0)
        top = a.here()
        a.wdr()
        a.lds(R_TMP, TIFR0)
        a.andi(R_TMP, TOV)
        a.breq_back(top)
        a.ldi(R_TMP, TOV)
        a.sts(TIFR0, R_TMP)
        a.subi(20, 0xFF)                # one more TIMER0 period gone
        a.cpi(20, 20)
        a.brne_back(top)

        a.cli()
        a.mov(22, 18)                   # must still be zero
        a.sei()
        bite = a.here()
        a.cpi(18, 1)
        a.brne_back(bite)               # now let it bite
        a.cli()

        a.puts('kicked=')
        a.puthex(22)
        a.puts(' then=')
        a.puthex(18)
        a.puts('\r\n')
        a.spin()

        a.words[1] = main
        a.words[WDT_VECTOR_WORD + 1] = isr

        self.run_asm(a, 'kicked=00 then=01')

    def test_timed_sequence_protects_wde(self):
        """
        A bare write cannot turn the watchdog on or change how long it runs
        for; without WDCE first, WDE and the prescaler keep their values.
        """
        a = WdtAsm()
        a.enable_uart()
        a.stsi(WDTCSR, WDE | 0x05)      # no WDCE: ignored
        a.lds(20, WDTCSR)
        a.wdt_arm(WDE | 0x05)           # with the sequence: accepted
        a.lds(21, WDTCSR)
        a.wdt_arm(0)                    # and off again
        a.lds(22, WDTCSR)
        a.puts('bare=')
        a.puthex(20)
        a.puts(' timed=')
        a.puthex(21)
        a.puts(' off=')
        a.puthex(22)
        a.puts('\r\n')
        a.spin()

        self.run_asm(a, 'bare=00 timed=0d off=00')

    def test_system_reset(self):
        """
        WDE without WDIE resets the part.  The firmware leaves a mark in
        SRAM, which the reset does not clear, so the second time through it
        knows where it has been.
        """
        a = WdtAsm()
        a.enable_uart()
        a.lds(20, MARK)
        a.cpi(20, MARK_MAGIC)
        rebooted = a.here()
        a._w(0xF001)                    # BREQ, patched once the target is known

        a.puts('arming\r\n')
        a.stsi(MARK, MARK_MAGIC)
        a.wdt_arm(WDE | WDP_16MS)
        a.spin()                        # wait to be reset

        target = a.here()
        a.words[rebooted] = 0xF001 | (((target - rebooted - 1) & 0x7F) << 3)
        a.wdt_arm(0)                    # so it does not go round again
        a.puts('rebooted\r\n')
        a.spin()

        self.run_asm(a, 'rebooted')


if __name__ == '__main__':
    QemuSystemTest.main()
