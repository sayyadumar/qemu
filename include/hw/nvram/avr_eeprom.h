/*
 * AVR EEPROM controller
 *
 * Copyright (c) 2025 QEMU Contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_NVRAM_AVR_EEPROM_H
#define HW_NVRAM_AVR_EEPROM_H

#include "hw/sysbus.h"
#include "system/block-backend.h"
#include "qom/object.h"

#define TYPE_AVR_EEPROM "avr-eeprom"
OBJECT_DECLARE_SIMPLE_TYPE(AVREepromState, AVR_EEPROM)

/* EECR, EEDR, EEARL and EEARH are four consecutive bytes. */
#define AVR_EEPROM_REGS_SIZE    4

struct AVREepromState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq irq;               /* EEPROM ready */

    uint8_t eecr;
    uint8_t eedr;
    uint8_t eearl;
    uint8_t eearh;

    uint32_t size;              /* bytes of storage; a power of two */
    uint8_t *storage;
    BlockBackend *blk;
    bool blk_ro;
};

#endif /* HW_NVRAM_AVR_EEPROM_H */
