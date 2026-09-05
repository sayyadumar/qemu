#!/usr/bin/env python3
#
# Functional tests for the ATmega SPI controller.
#
# Copyright (c) 2025 QEMU Contributors
#
# This work is licensed under the terms of the GNU GPL, version 2 or
# later.  See the COPYING file in the top-level directory.
#
# The firmware for each test is assembled here rather than shipped as a
# binary, so what the guest does is visible next to what it is expected to
# produce.  Results are printed over USART0 as hex, which the console
# pattern matcher then looks for.

import struct

from qemu_test import QemuSystemTest, wait_for_console_pattern

# I/O space addresses (data address minus 0x20), reachable with IN/OUT.
IO_SPCR = 0x2C
IO_SPSR = 0x2D
IO_SPDR = 0x2E

# USART0 lives in extended I/O and needs LDS/STS.
USART0 = 0x00C0
UCSR0B = USART0 + 1
UDR0 = USART0 + 6
TXEN = 1 << 3

SPCR_SPR0 = 1 << 0
SPCR_MSTR = 1 << 4
SPCR_DORD = 1 << 5
SPCR_SPE = 1 << 6
SPCR_SPIE = 1 << 7

SPSR_SPIF = 1 << 7

# SPI serial transfer complete is vector 20 on the ATmega1284P, which the
# core enters at word address (20 - 1) * 2.
SPI_STC_VECTOR_WORD = 38

# Scratch registers the emitted code is free to clobber.
R_TMP = 24
R_TMP2 = 25


class Asm:
    """Just enough AVR assembly to drive the SPI registers."""

    def __init__(self):
        self.words = []

    def _w(self, *words):
        self.words.extend(words)
        return self

    def here(self):
        """Word address of the next instruction."""
        return len(self.words)

    def pad_to(self, word):
        assert len(self.words) <= word, 'code overran word %d' % word
        while len(self.words) < word:
            self.words.append(0x0000)       # NOP
        return self

    def nop(self):
        return self._w(0x0000)

    def ldi(self, d, k):
        assert 16 <= d <= 31
        return self._w(0xE000 | ((k & 0xF0) << 4) | ((d - 16) << 4) | (k & 0x0F))

    def andi(self, d, k):
        assert 16 <= d <= 31
        return self._w(0x7000 | ((k & 0xF0) << 4) | ((d - 16) << 4) | (k & 0x0F))

    def subi(self, d, k):
        assert 16 <= d <= 31
        return self._w(0x5000 | ((k & 0xF0) << 4) | ((d - 16) << 4) | (k & 0x0F))

    def cpi(self, d, k):
        assert 16 <= d <= 31
        return self._w(0x3000 | ((k & 0xF0) << 4) | ((d - 16) << 4) | (k & 0x0F))

    def mov(self, d, r):
        return self._w(0x2C00 | ((r & 0x10) << 5) | (d << 4) | (r & 0x0F))

    def swap(self, d):
        return self._w(0x9402 | (d << 4))

    def out(self, a, r):
        return self._w(0xB800 | ((a & 0x30) << 5) | (r << 4) | (a & 0x0F))

    def inp(self, d, a):
        return self._w(0xB000 | ((a & 0x30) << 5) | (d << 4) | (a & 0x0F))

    def sts(self, k, r):
        return self._w(0x9200 | (r << 4), k & 0xFFFF)

    def lds(self, d, k):
        return self._w(0x9000 | (d << 4), k & 0xFFFF)

    def sbrs(self, r, b):
        return self._w(0xFE00 | (r << 4) | b)

    def sbrc(self, r, b):
        return self._w(0xFC00 | (r << 4) | b)

    def rjmp_to(self, word):
        return self._w(0xC000 | ((word - self.here() - 1) & 0xFFF))

    def jmp(self, word):
        return self._w(0x940C, word & 0xFFFF)

    def brlo(self, skip):
        """Branch forward over `skip` words if the carry (lower) flag is set."""
        return self._w(0xF000 | ((skip & 0x7F) << 3))

    def sei(self):
        return self._w(0x9478)

    def reti(self):
        return self._w(0x9518)

    def spin(self):
        return self.rjmp_to(self.here())

    # -- composites ------------------------------------------------------

    def enable_uart(self):
        """DRE is set out of reset, so only the transmitter has to come up."""
        self.ldi(R_TMP, TXEN)
        return self.sts(UCSR0B, R_TMP)

    def putc(self, ch):
        self.ldi(R_TMP, ord(ch))
        return self.sts(UDR0, R_TMP)

    def puts(self, text):
        for ch in text:
            self.putc(ch)
        return self

    def _put_nibble(self):
        """Print the low nibble of R_TMP as a hex digit."""
        self.andi(R_TMP, 0x0F)
        self.subi(R_TMP, 0xD0)          # += 0x30, i.e. '0'
        self.cpi(R_TMP, ord('9') + 1)
        self.brlo(1)                    # already a decimal digit
        self.subi(R_TMP, 0xD9)          # += 39, i.e. 'a' - '9' - 1
        return self.sts(UDR0, R_TMP)

    def puthex(self, r):
        """Print the byte in Rr as two lowercase hex digits."""
        assert r not in (R_TMP, R_TMP2)
        self.mov(R_TMP, r)
        self.swap(R_TMP)
        self._put_nibble()
        self.mov(R_TMP, r)
        return self._put_nibble()

    def spi_wait(self):
        """Poll SPSR until the transfer completes."""
        top = self.here()
        self.inp(R_TMP2, IO_SPSR)
        self.sbrs(R_TMP2, 7)            # SPIF
        return self.rjmp_to(top)

    def spi_xfer(self, tx, rx):
        """Shift `tx` out and leave what came back in R`rx`."""
        self.ldi(R_TMP, tx)
        self.out(IO_SPDR, R_TMP)
        self.spi_wait()
        return self.inp(rx, IO_SPDR)

    def image(self):
        return b''.join(struct.pack('<H', w) for w in self.words)


class AVRSpiMachine(QemuSystemTest):

    timeout = 60

    def run_asm(self, asm, pattern):
        self.set_machine('mega1284')
        path = self.scratch_file('spi.bin')
        with open(path, 'wb') as f:
            f.write(asm.image())
        self.vm.add_args('-bios', path)
        self.vm.add_args('-nographic')
        self.vm.add_args('-device', 'm25p80,bus=ssi')
        self.vm.set_console()
        self.vm.launch()
        wait_for_console_pattern(self, pattern)

    def test_jedec_id(self):
        """
        A real transfer reaches the peer: the m25p80 answers the JEDEC ID
        command with its manufacturer, type and capacity bytes, 20 20 14.
        """
        a = Asm()
        a.enable_uart()
        a.ldi(16, SPCR_SPE | SPCR_MSTR | SPCR_SPR0)
        a.out(IO_SPCR, 16)
        a.spi_xfer(0x9F, 20)            # command; the peer answers with junk
        a.spi_xfer(0x00, 20)
        a.spi_xfer(0x00, 21)
        a.spi_xfer(0x00, 22)
        a.puts('id=')
        a.puthex(20)
        a.puthex(21)
        a.puthex(22)
        a.puts('\r\n')
        a.spin()

        self.run_asm(a, 'id=202014')

    def test_lsb_first(self):
        """
        With DORD set the byte goes out least significant bit first, so the
        peer sees it reversed and its answer comes back reversed too.  The
        command has to be pre-reversed to reach the flash as 0x9f, and the
        0x20 it replies with then reads as 0x04.
        """
        a = Asm()
        a.enable_uart()
        a.ldi(16, SPCR_SPE | SPCR_MSTR | SPCR_DORD | SPCR_SPR0)
        a.out(IO_SPCR, 16)
        a.spi_xfer(0xF9, 20)            # 0x9f reversed
        a.spi_xfer(0x00, 20)
        a.puts('lsb=')
        a.puthex(20)
        a.puts('\r\n')
        a.spin()

        self.run_asm(a, 'lsb=04')

    def test_disabled_controller(self):
        """
        SPE gates the controller: with it clear a write to SPDR shifts
        nothing, so SPIF stays clear and the peer never sees the command.
        Enabling afterwards must still find the flash in its idle state.
        """
        a = Asm()
        a.enable_uart()
        a.ldi(16, SPCR_MSTR)            # master, but not enabled
        a.out(IO_SPCR, 16)
        a.ldi(17, 0x9F)
        a.out(IO_SPDR, 17)
        a.inp(20, IO_SPSR)              # must read back without SPIF

        a.ldi(16, SPCR_SPE | SPCR_MSTR)
        a.out(IO_SPCR, 16)
        a.spi_xfer(0x9F, 21)
        a.spi_xfer(0x00, 21)            # 0x20 if the flash was still idle

        a.puts('off=')
        a.puthex(20)
        a.puts(' on=')
        a.puthex(21)
        a.puts('\r\n')
        a.spin()

        self.run_asm(a, 'off=00 on=20')

    def test_transfer_complete_interrupt(self):
        """
        SPIE raises SPI_STC on completion.  The handler reads SPSR and then
        SPDR, which is the sequence that clears SPIF, so the flag is gone by
        the time the main line looks at it again.
        """
        a = Asm()
        a.jmp(0)                        # reset vector, filled in below
        a.pad_to(SPI_STC_VECTOR_WORD)
        a.jmp(0)                        # SPI_STC vector, filled in below

        isr = a.here()
        a.ldi(18, 1)                    # r18 marks the handler as reached
        a.inp(19, IO_SPSR)              # capture SPSR as the handler saw it
        a.inp(R_TMP2, IO_SPDR)          # ... and clear SPIF
        a.reti()

        main = a.here()
        a.enable_uart()
        a.ldi(18, 0)
        a.ldi(16, SPCR_SPE | SPCR_MSTR | SPCR_SPIE)
        a.out(IO_SPCR, 16)
        a.sei()
        a.ldi(17, 0x9F)
        a.out(IO_SPDR, 17)
        for _ in range(8):              # let the core take the interrupt
            a.nop()
        a.inp(20, IO_SPSR)              # SPIF must have been acknowledged

        a.puts('isr=')
        a.puthex(18)
        a.puts(' in=')
        a.puthex(19)
        a.puts(' after=')
        a.puthex(20)
        a.puts('\r\n')
        a.spin()

        a.words[1] = main
        a.words[SPI_STC_VECTOR_WORD + 1] = isr

        self.run_asm(a, 'isr=01 in=80 after=00')


if __name__ == '__main__':
    QemuSystemTest.main()
