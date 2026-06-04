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
#include "hw/misc/unimp.h"
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
#define RX65N_MTU3_IRQ  156   /* TGIA3; ch3: 156-160, ch4: 161-165 */
#define RX65N_S12AD_IRQ 98    /* S12ADI0=98, GBADI0=99 */
#define RX65N_RSPI0_IRQ 44    /* SPEI0=44, SPRI0=45, SPTI0=46, SPII0=47 */
#define RX65N_ETHERC_IRQ 32   /* EINT0 (level-triggered) */

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
static const uint8_t ipr_table[NR_IRQS] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* 0-15 */
    0x00, 0xff, 0xff, 0xff, 0xff, 0x01, 0xff, 0x02,
    0xff, 0xff, 0xff, 0x03, 0x04, 0x05, 0x06, 0x07, /* 16-31 */
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x14, 0x14, 0x14, /* 32-47 */
    0x15, 0x15, 0x15, 0x15, 0xff, 0xff, 0xff, 0xff,
    0x18, 0x18, 0x18, 0x18, 0x18, 0x1d, 0x1e, 0x1f, /* 48-63 */
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
    0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, /* 64-79 */
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0x3a, 0x3b, 0x3c, 0xff, 0xff, 0xff, /* 80-95 */
    0x40, 0xff, 0x44, 0x45, 0xff, 0xff, 0x48, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* 96-111 */
    0xff, 0xff, 0x51, 0x51, 0x51, 0x51, 0x52, 0x52,
    0x52, 0x53, 0x53, 0x54, 0x54, 0x55, 0x55, 0x56, /* 112-127 */
    0x56, 0x57, 0x57, 0x57, 0x57, 0x58, 0x59, 0x59,
    0x59, 0x59, 0x5a, 0x5b, 0x5b, 0x5b, 0x5c, 0x5c, /* 128-143 */
    0x5c, 0x5c, 0x5d, 0x5d, 0x5d, 0x5e, 0x5e, 0x5f,
    0x5f, 0x60, 0x60, 0x61, 0x61, 0x62, 0x62, 0x62, /* 144-159 */
    0x62, 0x63, 0x64, 0x64, 0x64, 0x64, 0x65, 0x66,
    0x66, 0x66, 0x67, 0x67, 0x67, 0x67, 0x68, 0x68, /* 160-175 */
    0x68, 0x69, 0x69, 0x69, 0x6a, 0x6a, 0x6a, 0x6b,
    0x6b, 0x6b, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* 176-191 */
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x70, 0x71,
    0x72, 0x73, 0x74, 0x75, 0xff, 0xff, 0xff, 0xff, /* 192-207 */
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x80, 0x80,
    0x80, 0x80, 0x81, 0x81, 0x81, 0x81, 0x82, 0x82, /* 208-223 */
    0x82, 0x82, 0x83, 0x83, 0x83, 0x83, 0xff, 0xff,
    0xff, 0xff, 0x85, 0x85, 0x85, 0x85, 0x86, 0x86, /* 224-239 */
    0x86, 0x86, 0xff, 0xff, 0xff, 0xff, 0x88, 0x89,
    0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f, 0x90, 0x91, /* 240-255 */
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
    int i, irqbase;

    object_initialize_child(OBJECT(s), "sci[*]",
                            &s->sci[unit], TYPE_RENESAS_SCI);
    sci = SYS_BUS_DEVICE(&s->sci[unit]);
    qdev_prop_set_chr(DEVICE(sci), "chardev", serial_hd(unit));
    qdev_prop_set_uint64(DEVICE(sci), "input-freq", s->pclk_freq_hz);
    sysbus_realize(sci, &error_abort);

    irqbase = RX65N_SCI_IRQ + SCI_NR_IRQ * unit;
    for (i = 0; i < SCI_NR_IRQ; i++) {
        sysbus_connect_irq(sci, i,
                           qdev_get_gpio_in(DEVICE(&s->icu), irqbase + i));
    }
    sysbus_mmio_map(sci, 0, RX65N_SCI_BASE + unit * 0x08);
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
    sysbus_realize(etherc, &error_abort);

    sysbus_connect_irq(etherc, 0,
                       qdev_get_gpio_in(DEVICE(&s->icu), RX65N_ETHERC_IRQ));
    sysbus_mmio_map(etherc, 0, RX65N_ETHERC_BASE);
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
     * Simplified clock model: use a fixed 4x multiplier.
     * Real hardware uses a PLL with configurable ratios up to 120 MHz ICLK.
     */
    s->pclk_freq_hz = 4 * s->xtal_freq_hz;
    assert(s->pclk_freq_hz <= RX65N_PCLK_MAX_HZ);

    /*
     * Code flash base depends on flash size: it always ends at 0xFFFFFFFF,
     * so base = 0x100000000 - flash_size.
     */
    s->cflash_base = (uint32_t)(0x100000000ULL - rxc->rom_flash_size);

    memory_region_init_ram(&s->iram, OBJECT(dev), "iram",
                           rxc->ram_size, &error_abort);
    memory_region_add_subregion(s->sysmem, RX65N_IRAM_BASE, &s->iram);

    memory_region_init_rom(&s->d_flash, OBJECT(dev), "flash-data",
                           rxc->data_flash_size, &error_abort);
    memory_region_add_subregion(s->sysmem, RX65N_DFLASH_BASE, &s->d_flash);

    memory_region_init_rom(&s->c_flash, OBJECT(dev), "flash-code",
                           rxc->rom_flash_size, &error_abort);
    memory_region_add_subregion(s->sysmem, s->cflash_base, &s->c_flash);

    /* Stub out unimplemented peripheral regions so accesses log warnings */
    create_unimplemented_device("rx65n.usb",    0x000A0000, 0x10000);
    create_unimplemented_device("rx65n.rscan",  0x000A8000, 0x10000);
    create_unimplemented_device("rx65n.gpt",    0x000C2000, 0x01000);

    /* Initialize CPU */
    object_initialize_child(OBJECT(s), "cpu", &s->cpu, TYPE_RX65N_CPU);
    qdev_realize(DEVICE(&s->cpu), NULL, &error_abort);

    register_icu(s);
    s->cpu.env.ack = qdev_get_gpio_in_named(DEVICE(&s->icu), "ack", 0);
    register_tmr(s, 0);
    register_tmr(s, 1);
    register_cmt(s, 0);
    register_cmt(s, 1);
    register_sci(s, 0);
    register_mtu3(s);
    register_s12ad(s);
    register_rspi(s);
    register_etherc(s);
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
        .name          = TYPE_RX65N_MCU,
        .parent        = TYPE_DEVICE,
        .instance_size = sizeof(RX65NState),
        .class_size    = sizeof(RX65NClass),
        .class_init    = rx65n_class_init,
        .abstract      = true,
    }
};

DEFINE_TYPES(rx65n_types)
