/*
 * RX65N Microcontroller
 *
 * Datasheet: RX65N Group, RX651 Group User's Manual: Hardware
 * (Rev.1.00 R01UH0590EJ0100)
 *
 * Copyright (c) 2024 QEMU Contributors
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
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "hw/rx/rx65n.h"
#include "hw/loader.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "system/reset.h"
#include "hw/qdev-properties-system.h"
#include "system/block-backend.h"
#include "system/blockdev.h"
#include "hw/misc/unimp.h"
#include "net/net.h"
#include "system/system.h"
#include "qobject/qlist.h"
#include "qom/object.h"
#include "target/rx/cpu-qom.h"

/*
 * RX65N peripheral IRQ base numbers
 * See hardware manual section 14, Table 14.2
 */
#define RX65N_TMR_IRQ   174
#define RX65N_CMT_IRQ   28
#define RX65N_SCI_IRQ   214
/* SCI4 (serial console) vectors: RXI4=82, TXI4=83, ERI4/TEI4 = GROUPBL0 (110) */
#define RX65N_SCI4_RXI  82
#define RX65N_SCI4_TXI  83
#define RX65N_GROUPBL0  110
#define RX65N_MTU3_IRQ  156   /* TGIA3; ch3: 156-160, ch4: 161-165 */
#define RX65N_S12AD_IRQ 98    /* S12ADI0=98, GBADI0=99 */
#define RX65N_RSPI0_IRQ 44    /* SPEI0=44, SPRI0=45, SPTI0=46, SPII0=47 */
#define RX65N_ETHERC_IRQ 32   /* EINT0 (level-triggered) */
#define RX65N_FCU_FIFERR 21   /* Flash access error (level-triggered) */
#define RX65N_FCU_FRDYI  23   /* Flash ready */

#define RX65N_XTAL_MIN_HZ  (8  * 1000 * 1000)
#define RX65N_XTAL_MAX_HZ  (24 * 1000 * 1000)
#define RX65N_PCLK_MAX_HZ  (60 * 1000 * 1000)

struct RX65NClass {
    /*< private >*/
    DeviceClass parent_class;
    /*< public >*/
    const char *name;
    uint64_t ram_size;
    uint64_t rom_flash_size;
    uint64_t data_flash_size;
};
typedef struct RX65NClass RX65NClass;

DECLARE_CLASS_CHECKERS(RX65NClass, RX65N_MCU, TYPE_RX65N_MCU)

/*
 * IRQ -> IPR mapping table (256 entries)
 * 0x00–0x8d: IPR register index
 * 0xff: no IPR assigned (reserved or unimplemented)
 *
 * Based on RX65N Group Hardware Manual R01UH0590EJ0100, Table 14.2.
 * The Phase 1 peripherals (CMT0/1, TMR0/1, SCI0) share the same
 * vector numbers as the RX62N, so their IPR assignments are identical.
 * Entries for unimplemented RX65N-specific peripherals (USB, CAN,
 * Ethernet, MTU3, GPT) are marked 0xff pending full implementation.
 */
/*
 * Interrupt priority register assignment, from Table 15.5 of the RX65N/RX651
 * hardware manual. For all but a handful of low-numbered sources the IPR
 * number is simply the vector number; BUSERR, GROUPIE0 and RAMERR share
 * IPR000, the two software interrupts share IPR003, and the flash and timer
 * sources below vector 32 have their own small numbers. 0xff marks a vector
 * with no IPR register, which leaves it masked.
 *
 * This is not the RX62N assignment, which is a different table entirely.
 */
static const uint8_t ipr_table[NR_IRQS] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* 8-23 */
    0x00, 0x00, 0x00, 0xff, 0xff, 0x01, 0xff, 0x02,
    0xff, 0xff, 0x03, 0x03, 0x04, 0x05, 0x06, 0x07, /* 24-39 */
    0xff, 0xff, 0x22, 0x23, 0xff, 0xff, 0x26, 0x27,
    0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0xff, 0xff, /* 40-55 */
    0xff, 0xff, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
    0xff, 0xff, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, /* 56-71 */
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
    0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f, /* 72-87 */
    0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
    0x58, 0x59, 0x5a, 0xff, 0x5c, 0x5d, 0xff, 0x5f, /* 88-103 */
    0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67,
    0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f, /* 104-119 */
    0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0xff, 0xff,
    0x78, 0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f, /* 120-135 */
    0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f, /* 136-151 */
    0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
    0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f, /* 152-167 */
    0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf, /* 168-183 */
    0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7,
    0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf, /* 184-199 */
    0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
    0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf, /* 200-215 */
    0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7,
    0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf, /* 216-231 */
    0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7,
    0xe8, 0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef, /* 232-247 */
    0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7,
    0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff, /* 248-263 */
};

/*
 * Level-triggered interrupt list.
 * All other interrupts are edge-triggered.
 * See hardware manual section 14.3.
 */
static const uint8_t levelirq[] = {
     16,  21,  32,  44,  47,  48,  51,  64,  65,  66,
     67,  68,  69,  70,  71,  72,  73,  74,  75,  76,
     77,  78,  79,  90,  91, 170, 171, 172, 173, 214,
    217, 218, 221, 222, 225, 226, 229, 234, 237, 238,
    241, 246, 249, 250, 253,
};

static void register_icu(RX65NState *s)
{
    int i;
    SysBusDevice *icu;
    QList *ipr_map, *trigger_level;

    object_initialize_child(OBJECT(s), "icu", &s->icu, TYPE_RX_ICU);
    icu = SYS_BUS_DEVICE(&s->icu);

    ipr_map = qlist_new();
    for (i = 0; i < NR_IRQS; i++) {
        qlist_append_int(ipr_map, ipr_table[i]);
    }
    qdev_prop_set_array(DEVICE(icu), "ipr-map", ipr_map);

    trigger_level = qlist_new();
    for (i = 0; i < ARRAY_SIZE(levelirq); i++) {
        qlist_append_int(trigger_level, levelirq[i]);
    }
    qdev_prop_set_array(DEVICE(icu), "trigger-level", trigger_level);
    sysbus_realize(icu, &error_abort);

    sysbus_connect_irq(icu, 0, qdev_get_gpio_in(DEVICE(&s->cpu), RX_CPU_IRQ));
    sysbus_connect_irq(icu, 1, qdev_get_gpio_in(DEVICE(&s->cpu), RX_CPU_FIR));
    sysbus_connect_irq(icu, 2, qdev_get_gpio_in(DEVICE(&s->icu), SWI));
    sysbus_mmio_map(icu, 0, RX65N_ICU_BASE);
}

static void register_tmr(RX65NState *s, int unit)
{
    SysBusDevice *tmr;
    int i, irqbase;

    object_initialize_child(OBJECT(s), "tmr[*]",
                            &s->tmr[unit], TYPE_RENESAS_TMR);
    tmr = SYS_BUS_DEVICE(&s->tmr[unit]);
    qdev_prop_set_uint64(DEVICE(tmr), "input-freq", s->pclk_freq_hz);
    sysbus_realize(tmr, &error_abort);

    irqbase = RX65N_TMR_IRQ + TMR_NR_IRQ * unit;
    for (i = 0; i < TMR_NR_IRQ; i++) {
        sysbus_connect_irq(tmr, i,
                           qdev_get_gpio_in(DEVICE(&s->icu), irqbase + i));
    }
    sysbus_mmio_map(tmr, 0, RX65N_TMR_BASE + unit * 0x10);
}

static void register_cmt(RX65NState *s, int unit)
{
    SysBusDevice *cmt;
    int i, irqbase;

    object_initialize_child(OBJECT(s), "cmt[*]",
                            &s->cmt[unit], TYPE_RENESAS_CMT);
    cmt = SYS_BUS_DEVICE(&s->cmt[unit]);
    qdev_prop_set_uint64(DEVICE(cmt), "input-freq", s->pclk_freq_hz);
    sysbus_realize(cmt, &error_abort);

    irqbase = RX65N_CMT_IRQ + CMT_NR_IRQ * unit;
    for (i = 0; i < CMT_NR_IRQ; i++) {
        sysbus_connect_irq(cmt, i,
                           qdev_get_gpio_in(DEVICE(&s->icu), irqbase + i));
    }
    sysbus_mmio_map(cmt, 0, RX65N_CMT_BASE + unit * 0x10);
}

static void register_sci(RX65NState *s, int unit)
{
    SysBusDevice *sci;

    object_initialize_child(OBJECT(s), "sci[*]",
                            &s->sci[unit], TYPE_RENESAS_SCI);
    sci = SYS_BUS_DEVICE(&s->sci[unit]);
    /*
     * The console lives on SCI4 and takes the first -serial chardev. Other
     * channels take the chardev of the same index, so further -serial
     * options can back them; index 0 is skipped because the console already
     * holds it, and a channel with no chardev still presents its registers.
     */
    qdev_prop_set_chr(DEVICE(sci), "chardev",
                      unit == RX65N_SCI_CONSOLE ? serial_hd(0)
                      : unit == 0               ? NULL
                                                : serial_hd(unit));
    qdev_prop_set_uint64(DEVICE(sci), "input-freq", s->pclk_freq_hz);
    sysbus_realize(sci, &error_abort);

    /*
     * Only SCI4's interrupt vectors are wired: RXI4 and TXI4 have individual
     * vectors and ERI4/TEI4 come through the GROUPBL0 group interrupt. The
     * other channels are mapped so their registers respond, which is enough
     * for a polled driver, but their vectors are not connected.
     */
    if (unit == RX65N_SCI_CONSOLE) {
        sysbus_connect_irq(sci, ERI,
                           qdev_get_gpio_in(DEVICE(&s->icu), RX65N_GROUPBL0));
        sysbus_connect_irq(sci, RXI,
                           qdev_get_gpio_in(DEVICE(&s->icu), RX65N_SCI4_RXI));
        sysbus_connect_irq(sci, TXI,
                           qdev_get_gpio_in(DEVICE(&s->icu), RX65N_SCI4_TXI));
        sysbus_connect_irq(sci, TEI,
                           qdev_get_gpio_in(DEVICE(&s->icu), RX65N_GROUPBL0));
    }
    sysbus_mmio_map(sci, 0, RX65N_SCI_BASE + unit * RX65N_SCI_SPACING);
}

static void register_mtu3(RX65NState *s)
{
    SysBusDevice *mtu3;
    int i;

    object_initialize_child(OBJECT(s), "mtu3", &s->mtu3, TYPE_RENESAS_MTU3);
    mtu3 = SYS_BUS_DEVICE(&s->mtu3);
    qdev_prop_set_uint64(DEVICE(mtu3), "input-freq", s->pclk_freq_hz);
    sysbus_realize(mtu3, &error_abort);

    for (i = 0; i < MTU3_NR_IRQ; i++) {
        sysbus_connect_irq(mtu3, i,
                           qdev_get_gpio_in(DEVICE(&s->icu),
                                            RX65N_MTU3_IRQ + i));
    }
    sysbus_mmio_map(mtu3, 0, RX65N_MTU3_BASE);
}

static void register_s12ad(RX65NState *s)
{
    SysBusDevice *s12ad;
    int i;

    object_initialize_child(OBJECT(s), "s12ad", &s->s12ad, TYPE_RENESAS_S12AD);
    s12ad = SYS_BUS_DEVICE(&s->s12ad);
    qdev_prop_set_uint64(DEVICE(s12ad), "input-freq", s->pclk_freq_hz);
    sysbus_realize(s12ad, &error_abort);

    for (i = 0; i < S12AD_NR_IRQ; i++) {
        sysbus_connect_irq(s12ad, i,
                           qdev_get_gpio_in(DEVICE(&s->icu),
                                            RX65N_S12AD_IRQ + i));
    }
    sysbus_mmio_map(s12ad, 0, RX65N_S12AD_BASE);
}

static void register_rspi(RX65NState *s)
{
    SysBusDevice *rspi;
    int i;

    object_initialize_child(OBJECT(s), "rspi", &s->rspi, TYPE_RENESAS_RSPI);
    rspi = SYS_BUS_DEVICE(&s->rspi);
    qdev_prop_set_uint64(DEVICE(rspi), "input-freq", s->pclk_freq_hz);
    sysbus_realize(rspi, &error_abort);

    for (i = 0; i < RSPI_NR_IRQ; i++) {
        sysbus_connect_irq(rspi, i,
                           qdev_get_gpio_in(DEVICE(&s->icu),
                                            RX65N_RSPI0_IRQ + i));
    }
    sysbus_mmio_map(rspi, 0, RX65N_RSPI0_BASE);
}

static void register_etherc(RX65NState *s)
{
    SysBusDevice *etherc;

    object_initialize_child(OBJECT(s), "etherc", &s->etherc, TYPE_RENESAS_ETHERC);
    etherc = SYS_BUS_DEVICE(&s->etherc);
    /* Bind to a -nic/-netdev backend (default network) if one is present. */
    qemu_configure_nic_device(DEVICE(&s->etherc), true, NULL);
    sysbus_realize(etherc, &error_abort);

    sysbus_connect_irq(etherc, 0,
                       qdev_get_gpio_in(DEVICE(&s->icu), RX65N_ETHERC_IRQ));
    sysbus_mmio_map(etherc, 0, RX65N_ETHERC_BASE);
}

static void register_sysclk(RX65NState *s)
{
    SysBusDevice *sysclk;

    object_initialize_child(OBJECT(s), "sysclk", &s->sysclk,
                            TYPE_RX65N_SYSCLK);
    sysclk = SYS_BUS_DEVICE(&s->sysclk);
    sysbus_realize(sysclk, &error_abort);
    sysbus_mmio_map(sysclk, 0, RX65N_SYSTEM_BASE);
}

static void register_fcu(RX65NState *s, RX65NClass *rxc)
{
    SysBusDevice *fcu;

    object_initialize_child(OBJECT(s), "fcu", &s->fcu, TYPE_RENESAS_RX_FCU);
    fcu = SYS_BUS_DEVICE(&s->fcu);
    qdev_prop_set_uint32(DEVICE(fcu), "code-flash-size", rxc->rom_flash_size);
    qdev_prop_set_uint32(DEVICE(fcu), "data-flash-size", rxc->data_flash_size);
    qdev_prop_set_uint32(DEVICE(fcu), "code-flash-base", s->cflash_base);
    qdev_prop_set_uint32(DEVICE(fcu), "data-flash-base", RX65N_DFLASH_BASE);
    /*
     * Optional persistent backing: -drive if=pflash,unit=0 for the code
     * flash and unit=1 for the data flash. Without them the arrays are
     * volatile and start erased.
     */
    qdev_prop_set_drive(DEVICE(fcu), "code-flash-drive",
                        drive_get(IF_PFLASH, 0, 0) ?
                        blk_by_legacy_dinfo(drive_get(IF_PFLASH, 0, 0)) : NULL);
    qdev_prop_set_drive(DEVICE(fcu), "data-flash-drive",
                        drive_get(IF_PFLASH, 0, 1) ?
                        blk_by_legacy_dinfo(drive_get(IF_PFLASH, 0, 1)) : NULL);
    qdev_prop_set_drive(DEVICE(fcu), "ofsm-drive",
                        drive_get(IF_PFLASH, 0, 2) ?
                        blk_by_legacy_dinfo(drive_get(IF_PFLASH, 0, 2)) : NULL);
    sysbus_realize(fcu, &error_abort);

    /* Region 0: FACI registers; region 1: code flash; region 2: data flash. */
    sysbus_mmio_map(fcu, RX_FCU_MMIO_REGS, RX65N_FCU_BASE);
    sysbus_mmio_map(fcu, RX_FCU_MMIO_CFLASH, s->cflash_base);
    sysbus_mmio_map(fcu, RX_FCU_MMIO_DFLASH, RX65N_DFLASH_BASE);
    sysbus_mmio_map(fcu, RX_FCU_MMIO_OFSM, RX65N_OFSM_BASE);
    sysbus_mmio_map(fcu, RX_FCU_MMIO_FACI, RX_FCU_FACI_ISSUE_BASE);

    sysbus_connect_irq(fcu, RX_FCU_IRQ_FRDYI,
                       qdev_get_gpio_in(DEVICE(&s->icu), RX65N_FCU_FRDYI));
    sysbus_connect_irq(fcu, RX_FCU_IRQ_FIFERR,
                       qdev_get_gpio_in(DEVICE(&s->icu), RX65N_FCU_FIFERR));
}

static void register_gpio(RX65NState *s)
{
    SysBusDevice *gpio;

    object_initialize_child(OBJECT(s), "gpio", &s->gpio, TYPE_RENESAS_RX_GPIO);
    gpio = SYS_BUS_DEVICE(&s->gpio);
    sysbus_realize(gpio, &error_abort);
    sysbus_mmio_map(gpio, RX_GPIO_MMIO_PORT, RX65N_GPIO_BASE);
    sysbus_mmio_map(gpio, RX_GPIO_MMIO_MPC, RX65N_MPC_BASE);
}

static void register_dmac(RX65NState *s)
{
    SysBusDevice *dmac;
    int i;

    object_initialize_child(OBJECT(s), "dmac", &s->dmac, TYPE_RENESAS_RX_DMAC);
    dmac = SYS_BUS_DEVICE(&s->dmac);
    object_property_set_link(OBJECT(&s->dmac), "dma-memory",
                             OBJECT(s->sysmem), &error_abort);
    sysbus_realize(dmac, &error_abort);
    sysbus_mmio_map(dmac, 0, RX65N_DMAC_BASE);

    /* Channels 0-3 have dedicated vectors; 4-7 are routed via group IRQs. */
    for (i = 0; i < 4; i++) {
        sysbus_connect_irq(dmac, i,
                           qdev_get_gpio_in(DEVICE(&s->icu),
                                            RX65N_DMAC_IRQ + i));
    }
}

static void register_dtc(RX65NState *s)
{
    SysBusDevice *dtc;

    object_initialize_child(OBJECT(s), "dtc", &s->dtc, TYPE_RENESAS_RX_DTC);
    dtc = SYS_BUS_DEVICE(&s->dtc);
    sysbus_realize(dtc, &error_abort);
    sysbus_mmio_map(dtc, 0, RX65N_DTC_BASE);
}

static void register_wdt(RX65NState *s)
{
    SysBusDevice *wdt;

    object_initialize_child(OBJECT(s), "wdt", &s->wdt, TYPE_RENESAS_RX_WDT);
    wdt = SYS_BUS_DEVICE(&s->wdt);
    sysbus_realize(wdt, &error_abort);
    sysbus_mmio_map(wdt, 0, RX65N_WDT_BASE);
}

static void register_rtc(RX65NState *s)
{
    SysBusDevice *rtc;

    object_initialize_child(OBJECT(s), "rtc", &s->rtc, TYPE_RENESAS_RX_RTC);
    rtc = SYS_BUS_DEVICE(&s->rtc);
    sysbus_realize(rtc, &error_abort);
    sysbus_mmio_map(rtc, 0, RX65N_RTC_BASE);
}

/*
 * The CPU latches its reset vector during CPU reset, which the SoC has to
 * run before the peripherals exist because the interrupt controller needs a
 * realized CPU. That is fine for a firmware image loaded with -bios, which
 * the ROM loader can answer for at any time, but not for a code flash backed
 * by a drive: nothing is mapped at the vector address yet. Re-read it here,
 * from a handler that runs at machine reset once every region is in place.
 */
static void rx65n_reset_vector(void *opaque)
{
    RX65NState *s = opaque;
    uint32_t vec;

    /*
     * Re-latch the code flash bank layout first: in dual mode the vector
     * lives in whichever bank is mapped high, so reading it before the swap
     * has been applied would fetch it from the wrong bank.
     */
    rx_fcu_update_bank_map(&s->fcu);

    if (rom_ptr(0xfffffffc, 4)) {
        return;     /* a ROM blob covers the vector; the CPU already has it */
    }
    vec = ldl_le_phys(CPU(&s->cpu)->as, 0xfffffffc);
    if (vec != 0 && vec != 0xffffffff) {
        s->cpu.env.pc = vec;
    }
}

static void rx65n_realize(DeviceState *dev, Error **errp)
{
    RX65NState *s = RX65N_MCU(dev);
    RX65NClass *rxc = RX65N_MCU_GET_CLASS(dev);

    if (s->xtal_freq_hz == 0) {
        error_setg(errp, "\"xtal-frequency-hz\" property must be provided.");
        return;
    }
    if (s->xtal_freq_hz < RX65N_XTAL_MIN_HZ
            || s->xtal_freq_hz > RX65N_XTAL_MAX_HZ) {
        error_setg(errp, "\"xtal-frequency-hz\" property out of range "
                   "(8–24 MHz).");
        return;
    }
    /*
     * Simplified clock model: a fixed 4x multiplier, capped at the maximum
     * peripheral clock. Real hardware runs the PLL up to 240 MHz and divides
     * down to a <=60 MHz PCLKB; the cap yields the correct 60 MHz PCLKB for
     * faster crystals (e.g. the RSK's 24 MHz part) instead of overshooting.
     */
    s->pclk_freq_hz = MIN(4 * s->xtal_freq_hz, RX65N_PCLK_MAX_HZ);

    /*
     * Code flash base depends on flash size: it always ends at 0xFFFFFFFF,
     * so base = 0x100000000 - flash_size.
     */
    s->cflash_base = (uint32_t)(0x100000000ULL - rxc->rom_flash_size);

    memory_region_init_ram(&s->iram, OBJECT(dev), "iram",
                           MIN(rxc->ram_size, RX65N_SRAM_MAX), &error_abort);
    memory_region_add_subregion(s->sysmem, RX65N_IRAM_BASE, &s->iram);

    /*
     * Primary SRAM ends at 0x00080000 where the peripheral I/O space begins.
     * Any SRAM beyond 512 KB lives in the separate expansion region so it does
     * not shadow the on-chip peripheral registers.
     */
    if (rxc->ram_size > RX65N_SRAM_MAX) {
        memory_region_init_ram(&s->exram, OBJECT(dev), "exram",
                               rxc->ram_size - RX65N_SRAM_MAX, &error_abort);
        memory_region_add_subregion(s->sysmem, RX65N_EXRAM_BASE, &s->exram);
    }

    /*
     * Catch-all over the peripheral register space, mapped at a lower
     * priority than every real device. Without it an access to a peripheral
     * this model does not implement reads back zero in silence, which during
     * bring-up looks identical to a peripheral that is present but idle.
     */
    create_unimplemented_device("rx65n.peripheral", 0x00080000, 0x00080000);

    /* Stub out unimplemented peripheral regions so accesses log warnings */
    create_unimplemented_device("rx65n.usb",    0x000A0000, 0x10000);
    create_unimplemented_device("rx65n.rscan",  0x000A8000, 0x10000);
    create_unimplemented_device("rx65n.gpt",    0x000C2000, 0x01000);

    /* Initialize CPU */
    object_initialize_child(OBJECT(s), "cpu", &s->cpu, TYPE_RX65N_CPU);
    qdev_realize(DEVICE(&s->cpu), NULL, &error_abort);

    register_icu(s);
    s->cpu.env.ack = qdev_get_gpio_in_named(DEVICE(&s->icu), "ack", 0);
    register_sysclk(s);
    register_fcu(s, rxc);
    register_gpio(s);
    register_dmac(s);
    register_dtc(s);
    register_wdt(s);
    register_rtc(s);
    register_tmr(s, 0);
    register_tmr(s, 1);
    register_cmt(s, 0);
    register_cmt(s, 1);
    for (int i = 0; i < RX65N_NR_SCI; i++) {
        register_sci(s, i);
    }
    register_mtu3(s);
    register_s12ad(s);
    register_rspi(s);
    register_etherc(s);
    qemu_register_reset(rx65n_reset_vector, s);

}

static const Property rx65n_properties[] = {
    DEFINE_PROP_LINK("main-bus", RX65NState, sysmem, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_BOOL("load-kernel", RX65NState, kernel, false),
    DEFINE_PROP_UINT32("xtal-frequency-hz", RX65NState, xtal_freq_hz, 0),
};

static void rx65n_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rx65n_realize;
    device_class_set_props(dc, rx65n_properties);
}

static void r5f565ne_class_init(ObjectClass *oc, const void *data)
{
    RX65NClass *rxc = RX65N_MCU_CLASS(oc);

    rxc->ram_size        = 256 * KiB;
    rxc->rom_flash_size  = 512 * KiB;
    rxc->data_flash_size = 32  * KiB;
}

static void r5f565nh_class_init(ObjectClass *oc, const void *data)
{
    RX65NClass *rxc = RX65N_MCU_CLASS(oc);

    rxc->ram_size        = 640 * KiB;
    rxc->rom_flash_size  = 2   * MiB;
    rxc->data_flash_size = 32  * KiB;
}

/*
 * RX651 part with 1.5 MB of code flash. Same die family as the RX65N parts,
 * with the Ethernet controller depopulated; the model leaves ETHERC in place,
 * which firmware for this part simply never touches.
 */
static void r5f5651c_class_init(ObjectClass *oc, const void *data)
{
    RX65NClass *rxc = RX65N_MCU_CLASS(oc);

    rxc->ram_size        = 640 * KiB;
    rxc->rom_flash_size  = 1536 * KiB;
    rxc->data_flash_size = 32  * KiB;
}

static const TypeInfo rx65n_types[] = {
    {
        .name       = TYPE_R5F565NE_MCU,
        .parent     = TYPE_RX65N_MCU,
        .class_init = r5f565ne_class_init,
    }, {
        .name       = TYPE_R5F565NH_MCU,
        .parent     = TYPE_RX65N_MCU,
        .class_init = r5f565nh_class_init,
    }, {
        .name       = TYPE_R5F5651C_MCU,
        .parent     = TYPE_RX65N_MCU,
        .class_init = r5f5651c_class_init,
    }, {
        .name          = TYPE_RX65N_MCU,
        .parent        = TYPE_DEVICE,
        .instance_size = sizeof(RX65NState),
        .class_size    = sizeof(RX65NClass),
        .class_init    = rx65n_class_init,
        .abstract      = true,
    }
};

DEFINE_TYPES(rx65n_types)
