/*
 * AVR analog to digital converter
 *
 * Copyright (c) 2025 QEMU Contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ADC_AVR_ADC_H
#define HW_ADC_AVR_ADC_H

#include "hw/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_AVR_ADC "avr-adc"
OBJECT_DECLARE_SIMPLE_TYPE(AVRAdcState, AVR_ADC)

/* ADCL, ADCH, ADCSRA, ADCSRB, ADMUX, DIDR2, DIDR0 and DIDR1. */
#define AVR_ADC_REGS_SIZE   8
#define AVR_ADC_MAX_CHANNELS 16

struct AVRAdcState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    QEMUTimer *timer;
    qemu_irq irq;               /* conversion complete */

    uint8_t adcsra;
    uint8_t adcsrb;
    uint8_t admux;
    uint8_t didr0;
    uint8_t didr1;
    uint8_t didr2;
    uint16_t result;            /* the last conversion, right adjusted */
    bool result_held;           /* ADCL was read and ADCH was not */

    uint32_t channels;
    uint64_t cpu_freq_hz;
    uint32_t vref_mv;           /* full scale, in millivolts */
    /* What each pin is being driven to, in millivolts. */
    uint32_t input_mv[AVR_ADC_MAX_CHANNELS];

    /* Cleared while the power reduction register gates the block off. */
    bool enabled;
};

#endif /* HW_ADC_AVR_ADC_H */
