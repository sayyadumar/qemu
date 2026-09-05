#!/usr/bin/env python3
#
# Functional tests for the ATmega analog to digital converter.
#
# Copyright (c) 2025 QEMU Contributors
#
# This work is licensed under the terms of the GNU GPL, version 2 or
# later.  See the COPYING file in the top-level directory.

from qemu_test import QemuSystemTest, wait_for_console_pattern

from test_spi import Asm, R_TMP

# Data addresses; the whole block is in extended I/O.
ADCL = 0x0078
ADCH = 0x0079
ADCSRA = 0x007A
ADCSRB = 0x007B
ADMUX = 0x007C
SREG = 0x3F                     # I/O address

ADPS_DIV8 = 0x03
ADIE = 0x08
ADIF = 0x10
ADATE = 0x20
ADSC = 0x40
ADEN = 0x80

ADLAR = 0x20
MUX_BANDGAP = 0x1E
MUX_GND = 0x1F

# ADC conversion complete is vector 25 on the ATmega1284P, entered at word
# address (25 - 1) * 2.
ADC_VECTOR_WORD = 48

# What the pins are driven to for these tests, against a 5 V reference.
INPUTS = {0: 2500, 3: 1000}
GLOBALS = ['-global', 'avr-adc.vref-mv=5000']
for _ch, _mv in INPUTS.items():
    GLOBALS += ['-global', 'avr-adc.input%d=%d' % (_ch, _mv)]


class AdcAsm(Asm):

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

    def stsi(self, addr, value):
        self.ldi(R_TMP, value)
        return self.sts(addr, R_TMP)

    def convert(self, mux, lo, hi, extra=0):
        """Run one conversion on `mux`, leaving ADCL in Rlo and ADCH in Rhi."""
        self.stsi(ADMUX, mux | extra)
        self.stsi(ADCSRA, ADEN | ADSC | ADPS_DIV8)
        top = self.here()
        self.lds(R_TMP, ADCSRA)
        self.andi(R_TMP, ADSC)
        self.brne_back(top)
        self.lds(lo, ADCL)
        return self.lds(hi, ADCH)


class AVRAdcMachine(QemuSystemTest):

    timeout = 60

    def run_asm(self, asm, pattern, extra_args=GLOBALS):
        self.set_machine('mega1284')
        path = self.scratch_file('adc.bin')
        with open(path, 'wb') as f:
            f.write(asm.image())
        self.vm.add_args('-bios', path)
        self.vm.add_args('-nographic')
        self.vm.add_args(*extra_args)
        self.vm.set_console()
        self.vm.launch()
        wait_for_console_pattern(self, pattern)

    def test_single_conversion(self):
        """
        2500 mV against a 5000 mV reference is half scale, 0x200, and
        1000 mV is 0x0cc.  A channel nothing is driving reads as ground.
        """
        a = AdcAsm()
        a.enable_uart()
        a.convert(0, 20, 21)
        a.convert(3, 22, 23)
        a.convert(5, 2, 3)
        a.puts('ch0=')
        a.puthex(21)
        a.puthex(20)
        a.puts(' ch3=')
        a.puthex(23)
        a.puthex(22)
        a.puts(' ch5=')
        a.puthex(3)
        a.puthex(2)
        a.puts('\r\n')
        a.spin()

        self.run_asm(a, 'ch0=0200 ch3=00cc ch5=0000')

    def test_left_adjust(self):
        """
        ADLAR shifts the ten bits up so that ADCH alone is an eight bit
        result: half scale becomes 0x80.
        """
        a = AdcAsm()
        a.enable_uart()
        a.convert(0, 20, 21, ADLAR)
        a.puts('adlar=')
        a.puthex(21)
        a.puthex(20)
        a.puts('\r\n')
        a.spin()

        self.run_asm(a, 'adlar=8000')

    def test_internal_channels(self):
        """
        The last two MUX settings are not pins: one is ground and the other
        the 1.1 V bandgap, which against 5 V lands on 225.
        """
        a = AdcAsm()
        a.enable_uart()
        a.convert(MUX_GND, 20, 21)
        a.convert(MUX_BANDGAP, 22, 23)
        a.puts('gnd=')
        a.puthex(21)
        a.puthex(20)
        a.puts(' bg=')
        a.puthex(23)
        a.puthex(22)
        a.puts('\r\n')
        a.spin()

        self.run_asm(a, 'gnd=0000 bg=00e1')

    def test_needs_enabling(self):
        """
        ADSC does nothing while ADEN is clear: no conversion starts and the
        result stays where it was.  Enabling afterwards produces the real
        reading, so the first zero is not just the converter being idle.
        """
        a = AdcAsm()
        a.enable_uart()
        a.stsi(ADMUX, 0)
        a.stsi(ADCSRA, ADSC | ADPS_DIV8)        # no ADEN
        a.lds(20, ADCSRA)
        a.andi(20, ADSC)                        # must not have taken
        a.lds(21, ADCL)
        a.lds(22, ADCH)
        a.convert(0, 23, 2)
        a.puts('adsc=')
        a.puthex(20)
        a.puts(' off=')
        a.puthex(22)
        a.puthex(21)
        a.puts(' on=')
        a.puthex(2)
        a.puthex(23)
        a.puts('\r\n')
        a.spin()

        self.run_asm(a, 'adsc=00 off=0000 on=0200')

    def test_conversion_complete_interrupt(self):
        """
        ADIE turns completion into an interrupt rather than a flag to poll,
        and the result is there by the time the handler runs.
        """
        a = AdcAsm()
        a.jmp(0)
        a.pad_to(ADC_VECTOR_WORD)
        a.jmp(0)

        isr = a.here()
        a.push(R_TMP)
        a.inp(R_TMP, SREG)
        a.push(R_TMP)
        a.lds(19, ADCL)
        a.lds(20, ADCH)
        a.ldi(18, 1)
        a.stsi(ADCSRA, 0)               # stop, so nothing fires again
        a.pop(R_TMP)
        a.out(SREG, R_TMP)
        a.pop(R_TMP)
        a.reti()

        main = a.here()
        a.enable_uart()
        a.ldi(18, 0)
        a.stsi(ADMUX, 0)
        a.stsi(ADCSRA, ADEN | ADIE | ADSC | ADPS_DIV8)
        a.sei()
        top = a.here()
        a.cpi(18, 1)
        a.brne_back(top)
        a.cli()
        a.puts('isr=')
        a.puthex(18)
        a.puts(' val=')
        a.puthex(20)
        a.puthex(19)
        a.puts('\r\n')
        a.spin()

        a.words[1] = main
        a.words[ADC_VECTOR_WORD + 1] = isr

        self.run_asm(a, 'isr=01 val=0200')

    def test_free_running(self):
        """
        With ADATE set and the trigger source left at zero the converter
        starts the next conversion as each one lands, so ADIF keeps coming
        back after being cleared.
        """
        a = AdcAsm()
        a.enable_uart()
        a.stsi(ADMUX, 0)
        a.stsi(ADCSRA, ADEN | ADATE | ADSC | ADPS_DIV8)

        a.ldi(20, 0)
        top = a.here()
        wait = a.here()
        a.lds(R_TMP, ADCSRA)
        a.andi(R_TMP, ADIF)
        a.breq_back(wait)
        a.ldi(R_TMP, ADEN | ADATE | ADIF | ADPS_DIV8)
        a.sts(ADCSRA, R_TMP)            # clearing ADIF, keeping it running
        a.subi(20, 0xFF)
        a.cpi(20, 4)
        a.brne_back(top)

        a.puts('runs=')
        a.puthex(20)
        a.puts('\r\n')
        a.spin()

        self.run_asm(a, 'runs=04')


if __name__ == '__main__':
    QemuSystemTest.main()
