/*
 * Renesas ADS1263 32-bit ADC mock (SPI chardev backend)
 *
 * Minimal SPI responder that allows the ADS1263 driver stack to complete
 * its init sequence and enter the sampling loop.  Connected to an SCI
 * channel configured as SPI master (e.g. SCI1 on the RSK RX72M).
 *
 * Datasheet: ADS1262, ADS1263 32-Bit, Precision, 38-kSPS,
 *            Analog-to-Digital Converter (SBAS752D)
 *
 * Copyright (c) 2024 QEMU Contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "chardev/char.h"
#include "hw/adc/renesas_ads1263.h"
#include "qom/object.h"

struct Ads1263Chardev {
    Chardev parent;

    uint8_t regs[ADS1263_NUM_REGS];
    uint8_t spi_buf[8];
    uint8_t spi_len;
    uint8_t spi_pos;
    uint32_t sample_count;
};
typedef struct Ads1263Chardev Ads1263Chardev;

#define TYPE_CHARDEV_ADS1263 "chardev-ads1263"
DECLARE_INSTANCE_CHECKER(Ads1263Chardev, ADS1263_CHARDEV,
                         TYPE_CHARDEV_ADS1263)

/*
 * Called by the SCI SPI master for each transmitted byte.
 * For every byte the master sends the slave simultaneously sends
 * one back — build that response here and push it to the SCI's RDR.
 */
static int ads1263_chr_write(Chardev *chr, const uint8_t *buf, int len)
{
    Ads1263Chardev *s = ADS1263_CHARDEV(chr);
    uint8_t resp[8];
    int resp_len = 0;

    for (int i = 0; i < len; i++) {
        uint8_t b = buf[i];
        uint8_t addr;

        if (s->spi_len == 0) {
            /* First byte of a new command. */
            s->spi_pos = 0;
            if ((b & ADS1263_CMD_WREG) == ADS1263_CMD_WREG) {
                /* Register write: 3 bytes [0x40|addr, 0x00, value] */
                s->spi_buf[0] = b;
                s->spi_len = 3;
                resp[resp_len++] = 0x00;
            } else if ((b & ADS1263_CMD_RREG) == ADS1263_CMD_RREG) {
                /* Register read: 3 bytes [0x20|addr, 0x00, 0x00] */
                s->spi_buf[0] = b;
                s->spi_len = 3;
                resp[resp_len++] = 0x00;
            } else {
                /* Single-byte command. */
                switch (b) {
                case ADS1263_CMD_RESET:
                    /* Reset all registers to power-on defaults. */
                    for (int r = 0; r < ADS1263_NUM_REGS; r++) {
                        s->regs[r] = 0x00;
                    }
                    s->regs[ADS1263_REG_ID]    = 0x01;
                    s->regs[ADS1263_REG_POWER] = 0x11;
                    s->regs[ADS1263_REG_MODE2] = 0x04; /* PGA enabled */
                    break;
                case ADS1263_CMD_START1:
                    /* Signal that a conversion has started; next
                     * read of STATUS will have DRDY=0 temporarily. */
                    break;
                case ADS1263_CMD_STOP1:
                    break;
                case ADS1263_CMD_SYOCAL1:
                case ADS1263_CMD_SYGCAL1:
                case ADS1263_CMD_SFOCAL1:
                    /* Offset/gain/system calibrations — acknowledged. */
                    break;
                default:
                    break;
                }
                resp[resp_len++] = 0x00;
                s->spi_len = 0;
            }
        } else {
            /* Continuation byte of a multi-byte command. */
            s->spi_buf[s->spi_pos] = b;
            s->spi_pos++;

            if (s->spi_len == 3) {
                /* Register read or write. */
                if ((s->spi_buf[0] & ADS1263_CMD_RREG) ==
                    ADS1263_CMD_RREG) {
                    /* Register read: bytes 2-3 are don't-care. */
                    if (s->spi_pos == 3) {
                        addr = s->spi_buf[0] & 0x1F;
                        resp[resp_len++] =
                            s->regs[addr & (ADS1263_NUM_REGS - 1)];
                    } else {
                        resp[resp_len++] = 0x00;
                    }
                } else {
                    /* Register write: store value on third byte. */
                    if (s->spi_pos == 3) {
                        addr = s->spi_buf[0] & 0x1F;
                        s->regs[addr & (ADS1263_NUM_REGS - 1)] = b;
                    }
                    resp[resp_len++] = 0x00;
                }
            }
            s->spi_len--;
        }
    }

    if (resp_len > 0) {
        qemu_chr_be_write(chr, resp, resp_len);
    }

    return len;
}

static void ads1263_chr_open(Chardev *chr,
                             ChardevBackend *backend,
                             bool *be_opened,
                             Error **errp)
{
    Ads1263Chardev *s = ADS1263_CHARDEV(chr);

    /* Power-on defaults (datasheet Table 16). */
    memset(s->regs, 0x00, sizeof(s->regs));
    s->regs[ADS1263_REG_ID]        = 0x01;
    s->regs[ADS1263_REG_POWER]     = 0x11;
    s->regs[ADS1263_REG_INTERFACE] = 0x05;
    s->regs[ADS1263_REG_MODE0]     = 0x00;
    s->regs[ADS1263_REG_MODE1]     = 0x00;
    s->regs[ADS1263_REG_MODE2]     = 0x04;
    s->regs[ADS1263_REG_INPMUX]    = 0x01;
    s->regs[ADS1263_REG_IDACMUX]   = 0x00;
    s->regs[ADS1263_REG_IDACMAG]   = 0x00;
    s->regs[ADS1263_REG_REFMUX]    = 0x00;

    s->spi_len = 0;
    s->spi_pos = 0;
    s->sample_count = 0;

    *be_opened = false;
}

static void char_ads1263_class_init(ObjectClass *oc, const void *data)
{
    ChardevClass *cc = CHARDEV_CLASS(oc);

    cc->open     = ads1263_chr_open;
    cc->chr_write = ads1263_chr_write;
}

static const TypeInfo char_ads1263_type_info = {
    .name          = TYPE_CHARDEV_ADS1263,
    .parent        = TYPE_CHARDEV,
    .instance_size = sizeof(Ads1263Chardev),
    .class_init    = char_ads1263_class_init,
};

static void register_types(void)
{
    type_register_static(&char_ads1263_type_info);
}

type_init(register_types);
