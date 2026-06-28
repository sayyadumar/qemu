/*
 * RX72M Microcontroller
 *
 * Datasheet: RX72M Group User's Manual: Hardware (R01UH0804EJ)
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
 *
 * The RX72M reuses the RX65N peripheral set and the shared RX600-series ICUb
 * interrupt map; only the memory sizes, code-flash base and CPU core (RXv3)
 * differ. RX72M-specific blocks (EtherCAT, CAN-FD, USBA, ...) are left as
 * unimplemented stubs so accesses log instead of faulting.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "hw/rx/rx72m.h"
#include "hw/loader.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "hw/misc/unimp.h"
#include "net/net.h"
#include "system/system.h"
#include "qobject/qlist.h"
#include "qom/object.h"
#include "target/rx/cpu-qom.h"
#include "chardev/char.h"
#include "hw/adc/renesas_ads1263.h"

/* RX72M peripheral IRQ base numbers (shared RX600-series ICUb map). */
#define RX72M_TMR_IRQ   174
#define RX72M_CMT_IRQ   28
/*
 * The RSK RX72M firmware console is on SCI channel 7 (pins on PORT9).
 * RXI7=98, TXI7=99 (verified from the firmware's relocatable vector table);
 * ERI7/TEI7 are sourced via the GROUPBL0 group interrupt.
 */
#define RX72M_SCI1_RXI  36
#define RX72M_SCI1_TXI  37
#define RX72M_SCI1_ERI  38
#define RX72M_SCI1_TEI  39
#define RX72M_SCI7_RXI  98
#define RX72M_SCI7_TXI  99
#define RX72M_GROUPBL0  110
#define RX72M_MTU3_IRQ  156
#define RX72M_S12AD_IRQ 98
#define RX72M_RSPI0_IRQ 44
#define RX72M_ETHERC_IRQ 32
#define RX72M_FCU_FIFERR 21
#define RX72M_FCU_FRDYI  23
#define RX72M_DMAC_IRQ   120

#define RX72M_IRQ0_VECTOR 64

#define RX72M_XTAL_MIN_HZ  (8  * 1000 * 1000)
#define RX72M_XTAL_MAX_HZ  (24 * 1000 * 1000)
#define RX72M_PCLK_MAX_HZ  (60 * 1000 * 1000)

struct RX72MClass {
    /*< private >*/
    DeviceClass parent_class;
    /*< public >*/
    const char *name;
    uint64_t ram_size;
    uint64_t rom_flash_size;
    uint64_t data_flash_size;
};
typedef struct RX72MClass RX72MClass;

DECLARE_CLASS_CHECKERS(RX72MClass, RX72M_MCU, TYPE_RX72M_MCU)

/*
 * IRQ -> IPR mapping table (256 entries), shared with the RX65N for the
 * common peripherals. See RX72M Group HW Manual, interrupt vector table.
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
    0x40, 0xff, 0x62, 0x63, 0xff, 0xff, 0x48, 0xff, /* SCI7: RXI7=98/IPR62,
                                                       TXI7=99/IPR63 */
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

static const uint8_t levelirq[] = {
     16,  21,  32,  44,  47,  48,  51,  64,  65,  66,
     67,  68,  69,  70,  71,  72,  73,  74,  75,  76,
     77,  78,  79,  90,  91, 170, 171, 172, 173, 214,
    217, 218, 221, 222, 225, 226, 229, 234, 237, 238,
    241, 246, 249, 250, 253,
    /*
     * SCI1/SCI7 TXI are level requests (asserted while TDRE & TIE): the FIT
     * driver restarts transmission by re-enabling the interrupt at the ICU
     * with the data register already empty, which only re-triggers if TXI is
     * level.
     */
    37, 99,
};

static void register_icu(RX72MState *s)
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
    sysbus_mmio_map(icu, 0, RX72M_ICU_BASE);
}

static void register_tmr(RX72MState *s, int unit)
{
    SysBusDevice *tmr;
    int i, irqbase;

    object_initialize_child(OBJECT(s), "tmr[*]", &s->tmr[unit],
                            TYPE_RENESAS_TMR);
    tmr = SYS_BUS_DEVICE(&s->tmr[unit]);
    qdev_prop_set_uint64(DEVICE(tmr), "input-freq", s->pclk_freq_hz);
    sysbus_realize(tmr, &error_abort);

    irqbase = RX72M_TMR_IRQ + TMR_NR_IRQ * unit;
    for (i = 0; i < TMR_NR_IRQ; i++) {
        sysbus_connect_irq(tmr, i,
                           qdev_get_gpio_in(DEVICE(&s->icu), irqbase + i));
    }
    sysbus_mmio_map(tmr, 0, RX72M_TMR_BASE + unit * 0x10);
}

static void register_cmt(RX72MState *s, int unit)
{
    SysBusDevice *cmt;
    int i, irqbase;

    object_initialize_child(OBJECT(s), "cmt[*]", &s->cmt[unit],
                            TYPE_RENESAS_CMT);
    cmt = SYS_BUS_DEVICE(&s->cmt[unit]);
    qdev_prop_set_uint64(DEVICE(cmt), "input-freq", s->pclk_freq_hz);
    sysbus_realize(cmt, &error_abort);

    irqbase = RX72M_CMT_IRQ + CMT_NR_IRQ * unit;
    for (i = 0; i < CMT_NR_IRQ; i++) {
        sysbus_connect_irq(cmt, i,
                           qdev_get_gpio_in(DEVICE(&s->icu), irqbase + i));
    }
    sysbus_mmio_map(cmt, 0, RX72M_CMT_BASE + unit * 0x10);
}

static void register_sci(RX72MState *s, int unit)
{
    SysBusDevice *sci;
    hwaddr base;
    int rxi_irq, txi_irq, eri_irq, tei_irq;
    Chardev *chr;

    if (unit == 0) {
        /* SCI7 — RSK RX72M console UART */
        base     = RX72M_SCI7_BASE;
        rxi_irq  = RX72M_SCI7_RXI;
        txi_irq  = RX72M_SCI7_TXI;
        eri_irq  = RX72M_GROUPBL0;
        tei_irq  = RX72M_GROUPBL0;
        chr      = serial_hd(0);
    } else if (unit == 1) {
        /* SCI1 — SPI master for ADS1263 ADC */
        base     = RX72M_SCI1_BASE;
        rxi_irq  = RX72M_SCI1_RXI;
        txi_irq  = RX72M_SCI1_TXI;
        eri_irq  = RX72M_SCI1_ERI;
        tei_irq  = RX72M_SCI1_TEI;
        chr      = qemu_chardev_new("ads1263", TYPE_CHARDEV_ADS1263,
                                    NULL, NULL, &error_abort);
    } else {
        g_assert_not_reached();
    }

    object_initialize_child(OBJECT(s), "sci[*]", &s->sci[unit],
                            TYPE_RENESAS_SCI);
    sci = SYS_BUS_DEVICE(&s->sci[unit]);
    qdev_prop_set_chr(DEVICE(sci), "chardev", chr);
    qdev_prop_set_uint64(DEVICE(sci), "input-freq", s->pclk_freq_hz);
    sysbus_realize(sci, &error_abort);

    sysbus_connect_irq(sci, ERI,
                       qdev_get_gpio_in(DEVICE(&s->icu), eri_irq));
    sysbus_connect_irq(sci, RXI,
                       qdev_get_gpio_in(DEVICE(&s->icu), rxi_irq));
    sysbus_connect_irq(sci, TXI,
                       qdev_get_gpio_in(DEVICE(&s->icu), txi_irq));
    sysbus_connect_irq(sci, TEI,
                       qdev_get_gpio_in(DEVICE(&s->icu), tei_irq));
    sysbus_mmio_map(sci, 0, base);
}

static void register_mtu3(RX72MState *s)
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
                                            RX72M_MTU3_IRQ + i));
    }
    sysbus_mmio_map(mtu3, 0, RX72M_MTU3_BASE);
}

static void register_s12ad(RX72MState *s)
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
                                            RX72M_S12AD_IRQ + i));
    }
    sysbus_mmio_map(s12ad, 0, RX72M_S12AD_BASE);
}

static void register_rspi(RX72MState *s)
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
                                            RX72M_RSPI0_IRQ + i));
    }
    sysbus_mmio_map(rspi, 0, RX72M_RSPI0_BASE);
}

static void register_etherc(RX72MState *s)
{
    SysBusDevice *etherc;

    object_initialize_child(OBJECT(s), "etherc", &s->etherc,
                            TYPE_RENESAS_ETHERC);
    etherc = SYS_BUS_DEVICE(&s->etherc);
    qemu_configure_nic_device(DEVICE(&s->etherc), true, NULL);
    sysbus_realize(etherc, &error_abort);

    sysbus_connect_irq(etherc, 0,
                       qdev_get_gpio_in(DEVICE(&s->icu), RX72M_ETHERC_IRQ));
    sysbus_mmio_map(etherc, 0, RX72M_ETHERC_BASE);
}

static void register_sysclk(RX72MState *s)
{
    SysBusDevice *sysclk;

    object_initialize_child(OBJECT(s), "sysclk", &s->sysclk,
                            TYPE_RX65N_SYSCLK);
    sysclk = SYS_BUS_DEVICE(&s->sysclk);
    sysbus_realize(sysclk, &error_abort);
    sysbus_mmio_map(sysclk, 0, RX72M_SYSTEM_BASE);
}

static void register_fcu(RX72MState *s, RX72MClass *rxc)
{
    SysBusDevice *fcu;

    object_initialize_child(OBJECT(s), "fcu", &s->fcu, TYPE_RENESAS_RX_FCU);
    fcu = SYS_BUS_DEVICE(&s->fcu);
    qdev_prop_set_uint32(DEVICE(fcu), "code-flash-size", rxc->rom_flash_size);
    qdev_prop_set_uint32(DEVICE(fcu), "data-flash-size", rxc->data_flash_size);
    qdev_prop_set_uint32(DEVICE(fcu), "code-flash-base", s->cflash_base);
    qdev_prop_set_uint32(DEVICE(fcu), "data-flash-base", RX72M_DFLASH_BASE);
    sysbus_realize(fcu, &error_abort);

    sysbus_mmio_map(fcu, RX_FCU_MMIO_REGS, RX72M_FCU_BASE);
    sysbus_mmio_map(fcu, RX_FCU_MMIO_CFLASH, s->cflash_base);
    sysbus_mmio_map(fcu, RX_FCU_MMIO_DFLASH, RX72M_DFLASH_BASE);

    sysbus_connect_irq(fcu, RX_FCU_IRQ_FRDYI,
                       qdev_get_gpio_in(DEVICE(&s->icu), RX72M_FCU_FRDYI));
    sysbus_connect_irq(fcu, RX_FCU_IRQ_FIFERR,
                       qdev_get_gpio_in(DEVICE(&s->icu), RX72M_FCU_FIFERR));
}

static void register_gpio(RX72MState *s)
{
    SysBusDevice *gpio;

    object_initialize_child(OBJECT(s), "gpio", &s->gpio, TYPE_RENESAS_RX_GPIO);
    gpio = SYS_BUS_DEVICE(&s->gpio);
    sysbus_realize(gpio, &error_abort);
    sysbus_mmio_map(gpio, RX_GPIO_MMIO_PORT, RX72M_GPIO_BASE);
    sysbus_mmio_map(gpio, RX_GPIO_MMIO_MPC, RX72M_MPC_BASE);
}

static void register_dmac(RX72MState *s)
{
    SysBusDevice *dmac;
    int i;

    object_initialize_child(OBJECT(s), "dmac", &s->dmac, TYPE_RENESAS_RX_DMAC);
    dmac = SYS_BUS_DEVICE(&s->dmac);
    object_property_set_link(OBJECT(&s->dmac), "dma-memory",
                             OBJECT(s->sysmem), &error_abort);
    sysbus_realize(dmac, &error_abort);
    sysbus_mmio_map(dmac, 0, RX72M_DMAC_BASE);

    for (i = 0; i < 4; i++) {
        sysbus_connect_irq(dmac, i,
                           qdev_get_gpio_in(DEVICE(&s->icu),
                                            RX72M_DMAC_IRQ + i));
    }
}

static void register_dtc(RX72MState *s)
{
    SysBusDevice *dtc;

    object_initialize_child(OBJECT(s), "dtc", &s->dtc, TYPE_RENESAS_RX_DTC);
    dtc = SYS_BUS_DEVICE(&s->dtc);
    sysbus_realize(dtc, &error_abort);
    sysbus_mmio_map(dtc, 0, RX72M_DTC_BASE);
}

static void register_wdt(RX72MState *s)
{
    SysBusDevice *wdt;

    object_initialize_child(OBJECT(s), "wdt", &s->wdt, TYPE_RENESAS_RX_WDT);
    wdt = SYS_BUS_DEVICE(&s->wdt);
    sysbus_realize(wdt, &error_abort);
    sysbus_mmio_map(wdt, 0, RX72M_WDT_BASE);
}

static void register_rtc(RX72MState *s)
{
    SysBusDevice *rtc;

    object_initialize_child(OBJECT(s), "rtc", &s->rtc, TYPE_RENESAS_RX_RTC);
    rtc = SYS_BUS_DEVICE(&s->rtc);
    sysbus_realize(rtc, &error_abort);
    sysbus_mmio_map(rtc, 0, RX72M_RTC_BASE);
}

static void rx72m_realize(DeviceState *dev, Error **errp)
{
    RX72MState *s = RX72M_MCU(dev);
    RX72MClass *rxc = RX72M_MCU_GET_CLASS(dev);

    if (s->xtal_freq_hz == 0) {
        error_setg(errp, "\"xtal-frequency-hz\" property must be provided.");
        return;
    }
    if (s->xtal_freq_hz < RX72M_XTAL_MIN_HZ
            || s->xtal_freq_hz > RX72M_XTAL_MAX_HZ) {
        error_setg(errp, "\"xtal-frequency-hz\" property out of range "
                   "(8–24 MHz).");
        return;
    }
    /* Simplified clock model capped at the maximum peripheral clock. */
    s->pclk_freq_hz = MIN(4 * s->xtal_freq_hz, RX72M_PCLK_MAX_HZ);

    s->cflash_base = (uint32_t)(0x100000000ULL - rxc->rom_flash_size);

    memory_region_init_ram(&s->iram, OBJECT(dev), "iram",
                           MIN(rxc->ram_size, RX72M_SRAM_MAX), &error_abort);
    memory_region_add_subregion(s->sysmem, RX72M_IRAM_BASE, &s->iram);

    if (rxc->ram_size > RX72M_SRAM_MAX) {
        memory_region_init_ram(&s->exram, OBJECT(dev), "exram",
                               rxc->ram_size - RX72M_SRAM_MAX, &error_abort);
        memory_region_add_subregion(s->sysmem, RX72M_EXRAM_BASE, &s->exram);
    }

    /* RX72M-specific peripheral regions are stubbed (no models added). */
    create_unimplemented_device("rx72m.usba",     0x000A0000, 0x10000);
    create_unimplemented_device("rx72m.canfd",    0x000A8000, 0x10000);
    create_unimplemented_device("rx72m.ethercat", 0x000E0000, 0x10000);

    object_initialize_child(OBJECT(s), "cpu", &s->cpu, TYPE_RX72M_CPU);
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
    register_sci(s, 0);
    register_sci(s, 1);
    register_mtu3(s);
    register_s12ad(s);
    register_rspi(s);
    register_etherc(s);
}

static const Property rx72m_properties[] = {
    DEFINE_PROP_LINK("main-bus", RX72MState, sysmem, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_BOOL("load-kernel", RX72MState, kernel, false),
    DEFINE_PROP_UINT32("xtal-frequency-hz", RX72MState, xtal_freq_hz, 0),
};

static void rx72m_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rx72m_realize;
    device_class_set_props(dc, rx72m_properties);
}

static void r5f572mn_class_init(ObjectClass *oc, const void *data)
{
    RX72MClass *rxc = RX72M_MCU_CLASS(oc);

    rxc->ram_size        = 512 * KiB;
    rxc->rom_flash_size  = 4   * MiB;
    rxc->data_flash_size = 32  * KiB;
}

static const TypeInfo rx72m_types[] = {
    {
        .name       = TYPE_R5F572MN_MCU,
        .parent     = TYPE_RX72M_MCU,
        .class_init = r5f572mn_class_init,
    }, {
        .name          = TYPE_RX72M_MCU,
        .parent        = TYPE_DEVICE,
        .instance_size = sizeof(RX72MState),
        .class_size    = sizeof(RX72MClass),
        .class_init    = rx72m_class_init,
        .abstract      = true,
    }
};

DEFINE_TYPES(rx72m_types)
