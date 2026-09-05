/*
 * AVR 8-bit timer
 *
 * Copyright (c) 2025 QEMU Contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_TIMER_AVR_TIMER8_H
#define HW_TIMER_AVR_TIMER8_H

#include "hw/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_AVR_TIMER8 "avr-timer8"
OBJECT_DECLARE_SIMPLE_TYPE(AVRTimer8State, AVR_TIMER8)

struct AVRTimer8State {
    /* <private> */
    SysBusDevice parent_obj;

    /* <public> */
    MemoryRegion iomem;
    MemoryRegion imsk_iomem;
    MemoryRegion ifr_iomem;
    QEMUTimer *timer;
    qemu_irq compa_irq;
    qemu_irq compb_irq;
    qemu_irq ovf_irq;

    bool enabled;

    /* registers */
    uint8_t ccra;
    uint8_t ccrb;
    uint8_t cnt;
    uint8_t ocra;
    uint8_t ocrb;
    uint8_t imsk;
    uint8_t ifr;

    uint8_t id;
    /*
     * TIMER2's clock select field encodes a different, denser set of
     * prescalers than the one TIMER0 shares with the 16-bit timers.
     */
    bool alt_prescaler;
    uint64_t cpu_freq_hz;
    uint64_t period_ns;         /* one counter tick; zero while stopped */
    int64_t reset_time_ns;      /* when the counter last stood at zero */
    uint64_t alarm_tick;        /* the tick the pending alarm was set for */
};

#endif /* HW_TIMER_AVR_TIMER8_H */
