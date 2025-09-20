/* QEMU AVR TWI (2-wire) Implementation Scaffold
 *
 * Copyright (c) 2025 Your Name
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/i2c/avr_twi.h"
#include "hw/irq.h"
#include "migration/vmstate.h"
#include "hw/sysbus.h"
#include "qapi/error.h"


static const VMStateDescription vmstate_avr_twi = {
    .name = TYPE_AVR_TWI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(twbr, AVRTWIState),
        VMSTATE_UINT8(twsr, AVRTWIState),
        VMSTATE_UINT8(twar, AVRTWIState),
        VMSTATE_UINT8(twdr, AVRTWIState),
        VMSTATE_UINT8(twcr, AVRTWIState),
        VMSTATE_UINT8(twamr, AVRTWIState),
        VMSTATE_END_OF_LIST()
    }
};

static uint64_t avr_twi_read(void *opaque, hwaddr addr, unsigned size)
{
    AVRTWIState *s = opaque;
    switch (addr) {
        case AVR_TWI_TWBR: return s->twbr;
        case AVR_TWI_TWSR: return s->twsr;
        case AVR_TWI_TWAR: return s->twar;
        case AVR_TWI_TWDR: return s->twdr;
        case AVR_TWI_TWCR: return s->twcr;
        case AVR_TWI_TWAMR: return s->twamr;
        default: return 0xff;
    }
}

static void avr_twi_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    AVRTWIState *s = opaque;
    assert((val & 0xff) == val);
    assert(size == 1);
    switch (addr) {
        case AVR_TWI_TWBR: s->twbr = val; break;
        case AVR_TWI_TWSR: s->twsr = val; break;
        case AVR_TWI_TWAR: s->twar = val; break;
        case AVR_TWI_TWDR: s->twdr = val; break;
        case AVR_TWI_TWCR: s->twcr = val; break;
        case AVR_TWI_TWAMR: s->twamr = val; break;
    }
    // TODO: Implement I2C protocol logic
}

static const MemoryRegionOps avr_twi_ops = {
    .read = avr_twi_read,
    .write = avr_twi_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static void avr_twi_reset(DeviceState *dev)
{
    AVRTWIState *s = AVR_TWI(dev);

    /* Reset values according to AVR peripheral documentation see datasheet section 21.9 */
    s->twbr = 0x0; // Bit Rate
    s->twcr = 0x0; // Control
    s->twsr = 0xf8; // Status
    s->twdr = 0xff; // Data
    s->twar = 0xfe; // Address
    s->twamr = 0x0; // Address Mask
}

static void avr_twi_pr(void *opaque, int irq, int level)
{
    AVRTWIState *s = AVR_TWI(opaque);

    s->enabled = !level;

    if (!s->enabled) {
        avr_twi_reset(DEVICE(s));
    }
}

static void avr_twi_realize(DeviceState *dev, Error **errp)
{
    AVRTWIState *s = AVR_TWI(dev);
    s->bus = i2c_init_bus(dev, NULL);

    memory_region_init_io(&s->iomem, OBJECT(dev), &avr_twi_ops, s,
                          TYPE_AVR_TWI, 0x06);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    qdev_init_gpio_in(DEVICE(s), avr_twi_pr, 1);
}

static void avr_twi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, avr_twi_reset);
    dc->vmsd = &vmstate_avr_twi;
    dc->realize = avr_twi_realize;
}

static const TypeInfo avr_twi_info = {
    .name          = TYPE_AVR_TWI,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AVRTWIState),
    .class_init = avr_twi_class_init,
};

static void avr_twi_register_types(void)
{
    type_register_static(&avr_twi_info);
}

type_init(avr_twi_register_types)
