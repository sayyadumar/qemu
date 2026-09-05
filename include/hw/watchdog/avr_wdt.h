/*
 * AVR watchdog timer
 *
 * Copyright (c) 2025 QEMU Contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_WATCHDOG_AVR_WDT_H
#define HW_WATCHDOG_AVR_WDT_H

#include "hw/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_AVR_WDT "avr-wdt"
OBJECT_DECLARE_SIMPLE_TYPE(AVRWdtState, AVR_WDT)

/* WDTCSR is a single byte. */
#define AVR_WDT_REGS_SIZE   1

struct AVRWdtState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    QEMUTimer *timer;
    qemu_irq irq;               /* watchdog time-out */

    uint8_t wdtcsr;
};

#endif /* HW_WATCHDOG_AVR_WDT_H */
