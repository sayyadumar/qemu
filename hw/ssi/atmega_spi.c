/* QEMU ATmega1280 SPI Master Implementation
 *
 * Copyright (c) 2025 Your Name
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "qapi/error.h"
#include "qemu/log.h"

#define TYPE_ATMEGA1280_SPI "atmega1280-spi"
OBJECT_DECLARE_SIMPLE_TYPE(ATMega1280SPIState, ATMEGA1280_SPI)

/* ATmega1280 SPI Register Offsets */
#define SPCR_OFFSET 0x4C  // Control Register
#define SPSR_OFFSET 0x4D  // Status Register
#define SPDR_OFFSET 0x4E  // Data Register

struct ATMega1280SPIState {
    SysBusDevice parent_obj;
    uint8_t spcr; // Control
    uint8_t spsr; // Status
    uint8_t spdr; // Data
    bool master_mode;
    // TODO: Add IRQ and pin logic
};

static uint64_t atmega1280_spi_read(void *opaque, hwaddr addr, unsigned size)
{
    ATMega1280SPIState *s = opaque;
    switch (addr) {
        case SPCR_OFFSET: return s->spcr;
        case SPSR_OFFSET: return s->spsr;
        case SPDR_OFFSET: return s->spdr;
        default: return 0xff;
    }
}

static void atmega1280_spi_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    ATMega1280SPIState *s = opaque;
    switch (addr) {
        case SPCR_OFFSET:
            s->spcr = val;
            s->master_mode = (val & (1 << 4)); // MSTR bit
            break;
        case SPSR_OFFSET:
            s->spsr = val;
            break;
        case SPDR_OFFSET:
            s->spdr = val;
            // TODO: Implement SPI transfer logic (MOSI/MISO)
            break;
    }
}

static const MemoryRegionOps atmega1280_spi_ops = {
    .read = atmega1280_spi_read,
    .write = atmega1280_spi_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static void atmega1280_spi_init(Object *obj)
{
    ATMega1280SPIState *s = ATMEGA1280_SPI(obj);
    memory_region_init_io(&s->parent_obj.mmio, obj, &atmega1280_spi_ops, s, TYPE_ATMEGA1280_SPI, 3);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->parent_obj.mmio);
}

static void atmega1280_spi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    // TODO: Set up IRQs and device properties
}

static const TypeInfo atmega1280_spi_info = {
    .name          = TYPE_ATMEGA1280_SPI,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ATMega1280SPIState),
    .instance_init = atmega1280_spi_init,
    .class_init    = atmega1280_spi_class_init,
};

static void atmega1280_spi_register_types(void)
{
    type_register_static(&atmega1280_spi_info);
}

type_init(atmega1280_spi_register_types)
