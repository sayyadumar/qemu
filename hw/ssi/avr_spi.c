/*
 * AVR SPI controller
 *
 * Copyright (c) 2025 QEMU Contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Three registers: SPCR configures the controller, SPSR reports completion
 * and SPDR is the shift register. Writing SPDR in master mode starts a
 * transfer; QEMU has no wire delay, so the exchange happens immediately and
 * SPDR then holds what the peer shifted back.
 *
 * SPIF is set when a transfer completes and raises the interrupt if SPIE is
 * set. Clearing it takes the sequence the hardware defines: read SPSR while
 * SPIF is set, then access SPDR.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/bitops.h"
#include "hw/ssi/avr_spi.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "migration/vmstate.h"

REG8(SPCR, 0)
    FIELD(SPCR, SPR, 0, 2)      /* clock rate select                     */
    FIELD(SPCR, CPHA, 2, 1)
    FIELD(SPCR, CPOL, 3, 1)
    FIELD(SPCR, MSTR, 4, 1)     /* 1 = master                            */
    FIELD(SPCR, DORD, 5, 1)     /* 1 = LSB of the data word first        */
    FIELD(SPCR, SPE, 6, 1)      /* SPI enable                            */
    FIELD(SPCR, SPIE, 7, 1)     /* interrupt enable                      */

REG8(SPSR, 1)
    FIELD(SPSR, SPI2X, 0, 1)    /* double speed                          */
    FIELD(SPSR, WCOL, 6, 1)     /* write collision                       */
    FIELD(SPSR, SPIF, 7, 1)     /* transfer complete                     */

#define R_SPDR  2

/* SPSR is read/write only in its SPI2X bit; the flags are set by hardware. */
#define SPSR_WRITE_MASK     R_SPSR_SPI2X_MASK

static void avr_spi_reset(DeviceState *dev);

static void avr_spi_update_irq(AVRSPIState *s)
{
    bool raise = FIELD_EX8(s->spsr, SPSR, SPIF) &&
                 FIELD_EX8(s->spcr, SPCR, SPIE);

    qemu_set_irq(s->irq, raise);
}

/* SPIF clears once SPDR is accessed after SPSR was read with SPIF set. */
static void avr_spi_ack_spif(AVRSPIState *s)
{
    if (s->spsr_read_with_spif) {
        s->spsr_read_with_spif = false;
        s->spsr = FIELD_DP8(s->spsr, SPSR, SPIF, 0);
        s->spsr = FIELD_DP8(s->spsr, SPSR, WCOL, 0);
        avr_spi_update_irq(s);
    }
}

static void avr_spi_transfer(AVRSPIState *s, uint8_t value)
{
    uint8_t rx;

    if (!s->enabled) {
        return;         /* gated off by the power reduction register */
    }
    if (!FIELD_EX8(s->spcr, SPCR, SPE)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "avr_spi: SPDR written while the controller is off\n");
        return;
    }
    if (!FIELD_EX8(s->spcr, SPCR, MSTR)) {
        /*
         * A slave only shifts when an external master drives the clock, and
         * nothing in this machine can do that.
         */
        qemu_log_mask(LOG_UNIMP,
                      "avr_spi: slave mode is not implemented\n");
        return;
    }

    /*
     * DORD selects which end of the byte goes out first. Peers on the bus
     * take a byte MSB first, so a DORD of 1 is modelled by reversing both
     * directions, which is what a peer would see on the wire.
     */
    if (FIELD_EX8(s->spcr, SPCR, DORD)) {
        rx = revbit8(ssi_transfer(s->bus, revbit8(value)));
    } else {
        rx = ssi_transfer(s->bus, value);
    }

    s->spdr = rx;
    s->spsr = FIELD_DP8(s->spsr, SPSR, SPIF, 1);
    s->spsr_read_with_spif = false;
    avr_spi_update_irq(s);
}

static uint64_t avr_spi_read(void *opaque, hwaddr offset, unsigned size)
{
    AVRSPIState *s = opaque;

    switch (offset) {
    case A_SPCR:
        return s->spcr;
    case A_SPSR:
        if (FIELD_EX8(s->spsr, SPSR, SPIF)) {
            s->spsr_read_with_spif = true;
        }
        return s->spsr;
    case R_SPDR:
        avr_spi_ack_spif(s);
        return s->spdr;
    default:
        g_assert_not_reached();
    }
}

static void avr_spi_write(void *opaque, hwaddr offset, uint64_t value,
                          unsigned size)
{
    AVRSPIState *s = opaque;

    switch (offset) {
    case A_SPCR:
        s->spcr = value;
        avr_spi_update_irq(s);
        break;
    case A_SPSR:
        s->spsr = (s->spsr & ~SPSR_WRITE_MASK) | (value & SPSR_WRITE_MASK);
        break;
    case R_SPDR:
        avr_spi_ack_spif(s);
        avr_spi_transfer(s, value);
        break;
    default:
        g_assert_not_reached();
    }
}

static const MemoryRegionOps avr_spi_ops = {
    .read = avr_spi_read,
    .write = avr_spi_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 1 },
    .valid = { .min_access_size = 1, .max_access_size = 1 },
};

/*
 * The power reduction register can gate the controller off, which resets it
 * and makes its registers read back as cleared.
 */
static void avr_spi_pr(void *opaque, int irq, int level)
{
    AVRSPIState *s = AVR_SPI(opaque);

    s->enabled = !level;
    if (!s->enabled) {
        avr_spi_reset(DEVICE(s));
    }
}

static void avr_spi_reset(DeviceState *dev)
{
    AVRSPIState *s = AVR_SPI(dev);

    s->spcr = 0;
    s->spsr = 0;
    s->spdr = 0;
    s->spsr_read_with_spif = false;
    avr_spi_update_irq(s);
}

static void avr_spi_init(Object *obj)
{
    AVRSPIState *s = AVR_SPI(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->mmio, obj, &avr_spi_ops, s,
                          TYPE_AVR_SPI, AVR_SPI_REGS_SIZE);
    sysbus_init_mmio(sbd, &s->mmio);
    sysbus_init_irq(sbd, &s->irq);
    s->bus = ssi_create_bus(DEVICE(obj), "ssi");
    s->enabled = true;
    qdev_init_gpio_in(DEVICE(obj), avr_spi_pr, 1);
}

static const VMStateDescription vmstate_avr_spi = {
    .name = "avr-spi",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(spcr, AVRSPIState),
        VMSTATE_UINT8(spsr, AVRSPIState),
        VMSTATE_UINT8(spdr, AVRSPIState),
        VMSTATE_BOOL(spsr_read_with_spif, AVRSPIState),
        VMSTATE_BOOL(enabled, AVRSPIState),
        VMSTATE_END_OF_LIST()
    }
};

static void avr_spi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_avr_spi;
    device_class_set_legacy_reset(dc, avr_spi_reset);
}

static const TypeInfo avr_spi_info = {
    .name          = TYPE_AVR_SPI,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AVRSPIState),
    .instance_init = avr_spi_init,
    .class_init    = avr_spi_class_init,
};

static void avr_spi_register_types(void)
{
    type_register_static(&avr_spi_info);
}

type_init(avr_spi_register_types)
