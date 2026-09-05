/*
 * AVR watchdog timer
 *
 * Copyright (c) 2025 QEMU Contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * One register, WDTCSR, and one instruction, WDR, which restarts the count.
 * The watchdog runs off its own 128 kHz oscillator rather than the system
 * clock, so its timeouts do not move when the part is clocked differently.
 *
 * WDE and WDIE choose what a timeout does.  On its own WDIE raises an
 * interrupt and clears itself, so an unattended second timeout falls through
 * to whatever WDE says; WDE on its own resets the part.  Together they give
 * the usual arrangement of one warning followed by a reset.
 *
 * Changing WDE or the prescaler needs the timed sequence -- WDCE and WDE
 * written together, then the new value within four cycles -- which is what
 * stops a program that has already lost its way from turning the watchdog
 * off.  Four cycles of a clock QEMU does not step is not a window that can
 * be timed here, so WDCE stays armed until the next write to WDTCSR
 * consumes it.  Code that follows the sequence cannot tell the difference.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/watchdog/avr_wdt.h"
#include "migration/vmstate.h"
#include "system/watchdog.h"

/* WDTCSR */
#define WDTCSR_WDP03    0x07    /* prescaler, low three bits */
#define WDTCSR_WDE      0x08    /* system reset enable       */
#define WDTCSR_WDCE     0x10    /* change enable             */
#define WDTCSR_WDP3     0x20    /* prescaler, top bit        */
#define WDTCSR_WDIE     0x40    /* interrupt enable          */
#define WDTCSR_WDIF     0x80    /* interrupt flag            */

/* The oscillator the watchdog counts, in Hz. */
#define WDT_OSC_HZ      128000

/* The shortest timeout is 2048 oscillator cycles; each step doubles it. */
#define WDT_BASE_CYCLES 2048
#define WDT_MAX_WDP     9

static unsigned avr_wdt_prescaler(AVRWdtState *s)
{
    unsigned wdp = (s->wdtcsr & WDTCSR_WDP03) |
                   ((s->wdtcsr & WDTCSR_WDP3) ? 8 : 0);

    if (wdp > WDT_MAX_WDP) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "avr_wdt: reserved prescaler %u selected\n", wdp);
        return WDT_MAX_WDP;
    }
    return wdp;
}

static bool avr_wdt_running(AVRWdtState *s)
{
    return s->wdtcsr & (WDTCSR_WDE | WDTCSR_WDIE);
}

/* Start the count over, which is what WDR does and what a timeout does. */
static void avr_wdt_restart(AVRWdtState *s)
{
    uint64_t cycles;

    if (!avr_wdt_running(s)) {
        timer_del(s->timer);
        return;
    }

    cycles = (uint64_t)WDT_BASE_CYCLES << avr_wdt_prescaler(s);
    timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                        cycles * NANOSECONDS_PER_SECOND / WDT_OSC_HZ);
}

static void avr_wdt_timeout(void *opaque)
{
    AVRWdtState *s = opaque;

    if (s->wdtcsr & WDTCSR_WDIE) {
        /*
         * Hardware clears WDIE as it takes the interrupt, so a handler that
         * does not restart the watchdog leaves the next timeout to WDE.
         */
        s->wdtcsr |= WDTCSR_WDIF;
        s->wdtcsr &= ~WDTCSR_WDIE;
        qemu_set_irq(s->irq, 1);
        avr_wdt_restart(s);
        return;
    }

    if (s->wdtcsr & WDTCSR_WDE) {
        watchdog_perform_action();
    }
}

/* WDR, from the CPU. */
static void avr_wdt_reset_in(void *opaque, int irq, int level)
{
    AVRWdtState *s = AVR_WDT(opaque);

    if (level) {
        avr_wdt_restart(s);
    }
}

static uint64_t avr_wdt_read(void *opaque, hwaddr offset, unsigned size)
{
    AVRWdtState *s = opaque;

    return s->wdtcsr;
}

static void avr_wdt_write(void *opaque, hwaddr offset, uint64_t value,
                          unsigned size)
{
    AVRWdtState *s = opaque;
    uint8_t val8 = value;
    bool armed = s->wdtcsr & WDTCSR_WDCE;
    uint8_t next;

    if ((val8 & WDTCSR_WDCE) && (val8 & WDTCSR_WDE)) {
        /*
         * The first half of the timed sequence.  Only WDCE and WDE take
         * effect; the rest of the value is there to be written again in the
         * second half.
         */
        s->wdtcsr |= WDTCSR_WDCE | WDTCSR_WDE;
        avr_wdt_restart(s);
        return;
    }

    /* WDIE and WDIF are always writable; WDIF is cleared by writing a one. */
    next = s->wdtcsr & ~(WDTCSR_WDCE | WDTCSR_WDIE);
    next |= val8 & WDTCSR_WDIE;
    if (val8 & WDTCSR_WDIF) {
        next &= ~WDTCSR_WDIF;
        qemu_set_irq(s->irq, 0);
    }

    if (armed) {
        next &= ~(WDTCSR_WDE | WDTCSR_WDP3 | WDTCSR_WDP03);
        next |= val8 & (WDTCSR_WDE | WDTCSR_WDP3 | WDTCSR_WDP03);
    } else if ((val8 ^ s->wdtcsr) & (WDTCSR_WDE | WDTCSR_WDP3 | WDTCSR_WDP03)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "avr_wdt: WDE and the prescaler need the timed "
                      "sequence; the write was ignored\n");
    }

    s->wdtcsr = next;
    avr_wdt_restart(s);
}

static const MemoryRegionOps avr_wdt_ops = {
    .read = avr_wdt_read,
    .write = avr_wdt_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 1 },
    .valid = { .min_access_size = 1, .max_access_size = 1 },
};

static void avr_wdt_reset(DeviceState *dev)
{
    AVRWdtState *s = AVR_WDT(dev);

    s->wdtcsr = 0;
    if (s->timer) {
        timer_del(s->timer);
    }
    qemu_set_irq(s->irq, 0);
}

static void avr_wdt_init(Object *obj)
{
    AVRWdtState *s = AVR_WDT(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->mmio, obj, &avr_wdt_ops, s,
                          TYPE_AVR_WDT, AVR_WDT_REGS_SIZE);
    sysbus_init_mmio(sbd, &s->mmio);
    sysbus_init_irq(sbd, &s->irq);
    qdev_init_gpio_in(DEVICE(obj), avr_wdt_reset_in, 1);
}

static void avr_wdt_realize(DeviceState *dev, Error **errp)
{
    AVRWdtState *s = AVR_WDT(dev);

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, avr_wdt_timeout, s);
}

static const VMStateDescription vmstate_avr_wdt = {
    .name = "avr-wdt",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(wdtcsr, AVRWdtState),
        VMSTATE_TIMER_PTR(timer, AVRWdtState),
        VMSTATE_END_OF_LIST()
    }
};

static void avr_wdt_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = avr_wdt_realize;
    dc->vmsd = &vmstate_avr_wdt;
    device_class_set_legacy_reset(dc, avr_wdt_reset);
}

static const TypeInfo avr_wdt_info = {
    .name          = TYPE_AVR_WDT,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AVRWdtState),
    .instance_init = avr_wdt_init,
    .class_init    = avr_wdt_class_init,
};

static void avr_wdt_register_types(void)
{
    type_register_static(&avr_wdt_info);
}

type_init(avr_wdt_register_types)
