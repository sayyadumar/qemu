/*
 * ATmega TWI (2-wire Serial Interface) Controller
 *
 * Copyright (c) 2025 Your Name
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef HW_I2C_ATMEGA_TWI_H
#define HW_I2C_ATMEGA_TWI_H

#include "hw/sysbus.h"
#include "hw/i2c/i2c.h"
#include "qom/object.h"

/* Register offsets (relative to TWI base) */
#define AVR_TWI_TWBR   0x00 // Bit Rate Register
#define AVR_TWI_TWSR   0x01 // Status Register
#define AVR_TWI_TWAR   0x02 // (Slave) Address Register
#define AVR_TWI_TWDR   0x03 // Data Register
#define AVR_TWI_TWCR   0x04 // Control Register
#define AVR_TWI_TWAMR  0x05 // Address Mask Register

#define TYPE_AVR_TWI "avr-twi"
OBJECT_DECLARE_SIMPLE_TYPE(AVRTWIState, AVR_TWI)

struct AVRTWIState {
    /* <private> */
    SysBusDevice parent_obj;

    /* <public> */
    MemoryRegion iomem;
    I2CBus *bus;
    qemu_irq irq;
    uint8_t twbr; // Bit Rate
    uint8_t twsr; // Status
    uint8_t twar; // Address
    uint8_t twdr; // Data
    uint8_t twcr; // Control
    uint8_t twamr; // Address Mask

    bool enabled;

};

#endif /* HW_I2C_ATMEGA_TWI_H */
