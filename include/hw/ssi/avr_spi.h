/*
 * AVR SPI controller
 *
 * Copyright (c) 2025 QEMU Contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_SSI_AVR_SPI_H
#define HW_SSI_AVR_SPI_H

#include "hw/sysbus.h"
#include "hw/ssi/ssi.h"
#include "qom/object.h"

#define TYPE_AVR_SPI "avr-spi"
OBJECT_DECLARE_SIMPLE_TYPE(AVRSPIState, AVR_SPI)

/* SPCR, SPSR and SPDR are three consecutive bytes. */
#define AVR_SPI_REGS_SIZE   3

struct AVRSPIState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    SSIBus *bus;
    qemu_irq irq;           /* SPI serial transfer complete */

    uint8_t spcr;
    uint8_t spsr;
    uint8_t spdr;

    /*
     * SPIF is cleared by reading SPSR while it is set and then accessing
     * SPDR, so the first half of that sequence has to be remembered.
     */
    bool spsr_read_with_spif;

    /* Cleared while the power reduction register gates the block off. */
    bool enabled;
};

#endif /* HW_SSI_AVR_SPI_H */
