/*
 * Renesas ADS1263 32-bit ADC mock (SPI chardev backend)
 *
 * This is a minimal responder that intercepts SPI bytes on an SCI channel
 * configured as SPI master and returns valid register values so that the
 * ADS1263 driver stack (FIT → BSP → SCI) completes its init sequence.
 *
 * Datasheet: ADS1262, ADS1263 32-Bit, Precision, 38-kSPS, Analog-to-Digital
 *            Converter (SBAS752D)
 *
 * Copyright (c) 2024 QEMU Contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ADC_RENESAS_ADS1263_H
#define HW_ADC_RENESAS_ADS1263_H

#include "chardev/char.h"

#define TYPE_CHARDEV_ADS1263 "chardev-ads1263"

/* ADS1263 register map (subset used by the firmware). */
enum {
    ADS1263_REG_ID        = 0x00,
    ADS1263_REG_POWER     = 0x01,
    ADS1263_REG_INTERFACE = 0x02,
    ADS1263_REG_MODE0     = 0x03,
    ADS1263_REG_MODE1     = 0x04,
    ADS1263_REG_MODE2     = 0x05,
    ADS1263_REG_INPMUX    = 0x06,
    ADS1263_REG_OFCAL0    = 0x07,
    ADS1263_REG_OFCAL1    = 0x08,
    ADS1263_REG_OFCAL2    = 0x09,
    ADS1263_REG_FSCAL0    = 0x0A,
    ADS1263_REG_FSCAL1    = 0x0B,
    ADS1263_REG_FSCAL2    = 0x0C,
    ADS1263_REG_IDACMUX   = 0x0D,
    ADS1263_REG_IDACMAG   = 0x0E,
    ADS1263_REG_REFMUX    = 0x0F,
    ADS1263_NUM_REGS      = 16,
};

/* SPI commands */
enum {
    ADS1263_CMD_NOP       = 0x00,
    ADS1263_CMD_RESET     = 0x06,
    ADS1263_CMD_START1    = 0x08,
    ADS1263_CMD_STOP1     = 0x0A,
    ADS1263_CMD_RDATA1    = 0x12,
    ADS1263_CMD_SYOCAL1   = 0x16,
    ADS1263_CMD_SYGCAL1   = 0x17,
    ADS1263_CMD_SFOCAL1   = 0x19,
};

/* Bit 6 = W (write), Bit 5 = R (read) in the command byte for register ops */
#define ADS1263_CMD_RREG  0x20
#define ADS1263_CMD_WREG  0x40

#endif /* HW_ADC_RENESAS_ADS1263_H */
