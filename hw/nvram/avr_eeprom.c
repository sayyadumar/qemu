/*
 * AVR EEPROM controller
 *
 * Copyright (c) 2025 QEMU Contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * EEAR addresses a byte, EEDR carries it and EECR says what to do with it.
 * A read is a single bit; a write takes the two step sequence the hardware
 * insists on, EEMPE and then EEPE, which stops a runaway program from
 * scribbling on the array by accident.
 *
 * Erasing sets a cell to 0xff and programming clears bits within it, so the
 * three programming modes differ in what they leave behind: erase and write
 * replaces the byte, erase alone returns it to 0xff, and write alone can
 * only clear further bits in whatever is already there.
 *
 * Programming takes milliseconds on hardware and no time at all here, so
 * EEPE always reads back clear.  Firmware that polls it before its next
 * access simply never waits.
 *
 * With a block backend attached the array is loaded from it at reset and
 * each programmed byte is written back, so it survives across runs.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "hw/block/block.h"
#include "hw/nvram/avr_eeprom.h"
#include "migration/vmstate.h"

/* Register offsets */
#define R_EECR      0x0
#define R_EEDR      0x1
#define R_EEARL     0x2
#define R_EEARH     0x3

/* EECR */
#define EECR_EERE       0x01    /* read enable                          */
#define EECR_EEPE       0x02    /* programming enable                   */
#define EECR_EEMPE      0x04    /* master programming enable            */
#define EECR_EERIE      0x08    /* ready interrupt enable               */
#define EECR_EEPM       0x30    /* programming mode                     */
#define EECR_EEPM_SHIFT 4

#define EEPM_ERASE_AND_WRITE    0
#define EEPM_ERASE              1
#define EEPM_WRITE              2

static uint32_t avr_eeprom_addr(AVREepromState *s)
{
    return ((s->eearh << 8) | s->eearl) & (s->size - 1);
}

static void avr_eeprom_update_irq(AVREepromState *s)
{
    /*
     * The request stands for as long as the controller is idle and the
     * interrupt is enabled.  Nothing here is ever busy, so enabling the
     * interrupt is what raises it.
     */
    qemu_set_irq(s->irq, !!(s->eecr & EECR_EERIE));
}

static void avr_eeprom_flush(AVREepromState *s, uint32_t addr)
{
    if (!s->blk || s->blk_ro) {
        return;
    }
    if (blk_pwrite(s->blk, addr, 1, s->storage + addr, 0) < 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "avr_eeprom: writing back offset 0x%x failed\n", addr);
    }
}

static void avr_eeprom_program(AVREepromState *s)
{
    uint32_t addr = avr_eeprom_addr(s);

    switch ((s->eecr & EECR_EEPM) >> EECR_EEPM_SHIFT) {
    case EEPM_ERASE_AND_WRITE:
        s->storage[addr] = s->eedr;
        break;
    case EEPM_ERASE:
        s->storage[addr] = 0xff;
        break;
    case EEPM_WRITE:
        /* The cell is assumed erased; programming can only clear bits. */
        s->storage[addr] &= s->eedr;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "avr_eeprom: reserved programming mode selected\n");
        return;
    }

    avr_eeprom_flush(s, addr);
}

static uint64_t avr_eeprom_read(void *opaque, hwaddr offset, unsigned size)
{
    AVREepromState *s = opaque;

    switch (offset) {
    case R_EECR:
        return s->eecr;
    case R_EEDR:
        return s->eedr;
    case R_EEARL:
        return s->eearl;
    case R_EEARH:
        return s->eearh;
    default:
        g_assert_not_reached();
    }
}

static void avr_eeprom_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    AVREepromState *s = opaque;
    uint8_t val8 = value;

    switch (offset) {
    case R_EECR:
        /*
         * EEPE and EERE act on the write and are not stored: neither ever
         * reads back set, because both operations finish immediately.
         */
        s->eecr = (s->eecr & EECR_EEMPE) | (val8 & (EECR_EERIE | EECR_EEPM));
        /*
         * EEMPE is set by software and cleared by hardware, so a zero
         * written to it leaves it alone.  That matters: the second half of
         * the sequence is often a plain store of EEPE, with no EEMPE in it.
         */
        if (val8 & EECR_EEMPE) {
            s->eecr |= EECR_EEMPE;
        }

        if (val8 & EECR_EEPE) {
            if (s->eecr & EECR_EEMPE) {
                avr_eeprom_program(s);
            } else {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "avr_eeprom: EEPE set without EEMPE\n");
            }
            /*
             * Hardware drops EEMPE four cycles after it is set, which is
             * why the sequence has to be back to back.  Timing that out
             * here would depend on how fast the host happens to be, so it
             * is dropped on use instead: firmware that follows the sequence
             * sees the same thing either way.
             */
            s->eecr &= ~EECR_EEMPE;
        }
        if (val8 & EECR_EERE) {
            s->eedr = s->storage[avr_eeprom_addr(s)];
        }
        avr_eeprom_update_irq(s);
        break;
    case R_EEDR:
        s->eedr = val8;
        break;
    case R_EEARL:
        s->eearl = val8;
        break;
    case R_EEARH:
        s->eearh = val8;
        break;
    default:
        g_assert_not_reached();
    }
}

static const MemoryRegionOps avr_eeprom_ops = {
    .read = avr_eeprom_read,
    .write = avr_eeprom_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 1 },
    .valid = { .min_access_size = 1, .max_access_size = 1 },
};

static void avr_eeprom_reset(DeviceState *dev)
{
    AVREepromState *s = AVR_EEPROM(dev);

    s->eecr = 0;
    s->eedr = 0;
    s->eearl = 0;
    s->eearh = 0;
    qemu_set_irq(s->irq, 0);
}

static void avr_eeprom_realize(DeviceState *dev, Error **errp)
{
    AVREepromState *s = AVR_EEPROM(dev);

    if (s->size == 0 || !is_power_of_2(s->size) || s->size > 0x10000) {
        error_setg(errp,
                   "AVR EEPROM: size must be a power of two up to 64 KiB");
        return;
    }

    s->storage = g_malloc(s->size);
    memset(s->storage, 0xff, s->size);      /* erased */

    if (s->blk) {
        uint64_t perm;

        s->blk_ro = !blk_supports_write_perm(s->blk);
        perm = BLK_PERM_CONSISTENT_READ | (s->blk_ro ? 0 : BLK_PERM_WRITE);
        if (blk_set_perm(s->blk, perm, BLK_PERM_ALL, errp) < 0) {
            return;
        }
        if (!blk_check_size_and_read_all(s->blk, dev, s->storage,
                                         s->size, errp)) {
            return;
        }
    }
}

static void avr_eeprom_unrealize(DeviceState *dev)
{
    AVREepromState *s = AVR_EEPROM(dev);

    g_free(s->storage);
    s->storage = NULL;
}

static void avr_eeprom_init(Object *obj)
{
    AVREepromState *s = AVR_EEPROM(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->mmio, obj, &avr_eeprom_ops, s,
                          TYPE_AVR_EEPROM, AVR_EEPROM_REGS_SIZE);
    sysbus_init_mmio(sbd, &s->mmio);
    sysbus_init_irq(sbd, &s->irq);
}

static const Property avr_eeprom_properties[] = {
    DEFINE_PROP_UINT32("size", AVREepromState, size, 0),
    DEFINE_PROP_DRIVE("drive", AVREepromState, blk),
};

static const VMStateDescription vmstate_avr_eeprom = {
    .name = "avr-eeprom",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(eecr, AVREepromState),
        VMSTATE_UINT8(eedr, AVREepromState),
        VMSTATE_UINT8(eearl, AVREepromState),
        VMSTATE_UINT8(eearh, AVREepromState),
        VMSTATE_VBUFFER_UINT32(storage, AVREepromState, 1, NULL, size),
        VMSTATE_END_OF_LIST()
    }
};

static void avr_eeprom_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = avr_eeprom_realize;
    dc->unrealize = avr_eeprom_unrealize;
    dc->vmsd = &vmstate_avr_eeprom;
    device_class_set_legacy_reset(dc, avr_eeprom_reset);
    device_class_set_props(dc, avr_eeprom_properties);
}

static const TypeInfo avr_eeprom_info = {
    .name          = TYPE_AVR_EEPROM,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AVREepromState),
    .instance_init = avr_eeprom_init,
    .class_init    = avr_eeprom_class_init,
};

static void avr_eeprom_register_types(void)
{
    type_register_static(&avr_eeprom_info);
}

type_init(avr_eeprom_register_types)
