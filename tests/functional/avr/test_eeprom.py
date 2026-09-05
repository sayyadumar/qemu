#!/usr/bin/env python3
#
# Functional tests for the ATmega EEPROM controller.
#
# Copyright (c) 2025 QEMU Contributors
#
# This work is licensed under the terms of the GNU GPL, version 2 or
# later.  See the COPYING file in the top-level directory.

from qemu_test import QemuSystemTest, wait_for_console_pattern

from test_spi import Asm, R_TMP

# EECR, EEDR, EEARL and EEARH, as I/O addresses -- they are low enough for
# IN and OUT.
IO_EECR = 0x1F
IO_EEDR = 0x20
IO_EEARL = 0x21
IO_EEARH = 0x22

EERE = 0x01
EEPE = 0x02
EEMPE = 0x04
EERIE = 0x08

EEPM_ERASE = 0x10               # erase only
EEPM_WRITE = 0x20               # write only

EEPROM_SIZE = 4096              # ATmega1284P


class EepromAsm(Asm):
    """Asm plus the EEPROM access sequences."""

    def set_addr(self, addr):
        self.ldi(R_TMP, addr & 0xFF)
        self.out(IO_EEARL, R_TMP)
        self.ldi(R_TMP, (addr >> 8) & 0xFF)
        return self.out(IO_EEARH, R_TMP)

    def ee_read(self, addr, rd):
        self.set_addr(addr)
        self.ldi(R_TMP, EERE)
        self.out(IO_EECR, R_TMP)
        return self.inp(rd, IO_EEDR)

    def ee_write(self, addr, value, mode=0):
        """The two step sequence hardware insists on: EEMPE, then EEPE."""
        self.set_addr(addr)
        self.ldi(R_TMP, value)
        self.out(IO_EEDR, R_TMP)
        self.ldi(R_TMP, mode | EEMPE)
        self.out(IO_EECR, R_TMP)
        self.ldi(R_TMP, mode | EEPE)
        return self.out(IO_EECR, R_TMP)


class AVREepromMachine(QemuSystemTest):

    timeout = 60

    def run_asm(self, asm, pattern, extra_args=()):
        self.set_machine('mega1284')
        path = self.scratch_file('eeprom.bin')
        with open(path, 'wb') as f:
            f.write(asm.image())
        self.vm.add_args('-bios', path)
        self.vm.add_args('-nographic')
        self.vm.add_args(*extra_args)
        self.vm.set_console()
        self.vm.launch()
        wait_for_console_pattern(self, pattern)

    def test_erased_at_reset(self):
        """An EEPROM with nothing behind it reads as erased."""
        a = EepromAsm()
        a.enable_uart()
        a.ee_read(0, 20)
        a.ee_read(EEPROM_SIZE - 1, 21)
        a.puts('blank=')
        a.puthex(20)
        a.puthex(21)
        a.puts('\r\n')
        a.spin()

        self.run_asm(a, 'blank=ffff')

    def test_write_and_read_back(self):
        """
        A byte written through the EEMPE/EEPE sequence comes back, and the
        neighbouring cells are left alone.
        """
        a = EepromAsm()
        a.enable_uart()
        a.ee_write(0x100, 0xA5)
        a.ee_write(0x101, 0x5A)
        a.ee_read(0x100, 20)
        a.ee_read(0x101, 21)
        a.ee_read(0x102, 22)
        a.puts('rw=')
        a.puthex(20)
        a.puthex(21)
        a.puthex(22)
        a.puts('\r\n')
        a.spin()

        self.run_asm(a, 'rw=a55aff')

    def test_write_needs_eempe(self):
        """
        Setting EEPE on its own does nothing: the interlock is the whole
        point of the two step sequence.
        """
        a = EepromAsm()
        a.enable_uart()
        a.set_addr(0x200)
        a.ldi(R_TMP, 0x33)
        a.out(IO_EEDR, R_TMP)
        a.ldi(R_TMP, EEPE)              # no EEMPE first
        a.out(IO_EECR, R_TMP)
        a.ee_read(0x200, 20)

        # And EEMPE does not stay armed for a second write either.
        a.set_addr(0x201)
        a.ldi(R_TMP, 0x44)
        a.out(IO_EEDR, R_TMP)
        a.ldi(R_TMP, EEMPE)
        a.out(IO_EECR, R_TMP)
        a.ldi(R_TMP, EEPE)
        a.out(IO_EECR, R_TMP)           # this one goes through
        a.set_addr(0x202)
        a.ldi(R_TMP, 0x55)
        a.out(IO_EEDR, R_TMP)
        a.ldi(R_TMP, EEPE)              # this one does not
        a.out(IO_EECR, R_TMP)
        a.ee_read(0x201, 21)
        a.ee_read(0x202, 22)

        a.puts('lock=')
        a.puthex(20)
        a.puthex(21)
        a.puthex(22)
        a.puts('\r\n')
        a.spin()

        self.run_asm(a, 'lock=ff44ff')

    def test_programming_modes(self):
        """
        Erase leaves 0xff behind and write on its own can only clear bits in
        what is already there, so writing 0xf0 over 0x0f leaves nothing.
        """
        a = EepromAsm()
        a.enable_uart()
        a.ee_write(0x300, 0x0F)                     # erase and write
        a.ee_write(0x300, 0xF0, EEPM_WRITE)         # write only
        a.ee_read(0x300, 20)

        a.ee_write(0x301, 0x3C)
        a.ee_write(0x301, 0x00, EEPM_ERASE)         # erase only
        a.ee_read(0x301, 21)

        a.puts('modes=')
        a.puthex(20)
        a.puthex(21)
        a.puts('\r\n')
        a.spin()

        self.run_asm(a, 'modes=00ff')

    def test_address_wraps(self):
        """
        EEAR is only as wide as the array, so an address past the end folds
        back into it rather than reaching off the end of the storage.
        """
        a = EepromAsm()
        a.enable_uart()
        a.ee_write(0x010, 0x77)
        a.ee_read(EEPROM_SIZE + 0x010, 20)
        a.puts('wrap=')
        a.puthex(20)
        a.puts('\r\n')
        a.spin()

        self.run_asm(a, 'wrap=77')

    def test_persistence(self):
        """
        With a drive behind it the array is loaded at start-up and each
        programmed byte written back, so a second run sees the first one's
        work.  Run the same firmware twice over one image to show it.
        """
        image = self.scratch_file('eeprom.img')
        with open(image, 'wb') as f:
            f.write(b'\xff' * EEPROM_SIZE)

        a = EepromAsm()
        a.enable_uart()
        a.ee_read(0x400, 20)            # what the last run left
        a.ee_write(0x400, 0xC3)
        a.ee_read(0x400, 21)
        a.puts('seen=')
        a.puthex(20)
        a.puts(' now=')
        a.puthex(21)
        a.puts('\r\n')
        a.spin()

        drive = ['-drive', 'if=pflash,format=raw,file=' + image]
        self.run_asm(a, 'seen=ff now=c3', drive)
        self.vm.shutdown()

        with open(image, 'rb') as f:
            self.assertEqual(f.read(EEPROM_SIZE)[0x400], 0xC3)


if __name__ == '__main__':
    QemuSystemTest.main()
