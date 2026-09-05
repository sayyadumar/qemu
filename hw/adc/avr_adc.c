/*
 * AVR analog to digital converter
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
 * ADMUX picks a channel, ADSC starts a conversion and thirteen ADC clocks
 * later the ten bit result turns up in ADCH:ADCL.  Nothing is wired to the
 * pins, so what each one is sitting at is a property: "input0" and friends
 * hold a voltage in millivolts, which is scaled against "vref-mv" to give
 * the count.  A machine or a test can set them, including while the guest
 * is running, and by default every pin reads as ground.
 *
 * Free running is the one automatic trigger source implemented; the others
 * are all events this device cannot see.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/adc/avr_adc.h"
#include "migration/vmstate.h"

/* Register offsets */
#define R_ADCL      0x0
#define R_ADCH      0x1
#define R_ADCSRA    0x2
#define R_ADCSRB    0x3
#define R_ADMUX     0x4
#define R_DIDR2     0x5
#define R_DIDR0     0x6
#define R_DIDR1     0x7

/* ADCSRA */
#define ADCSRA_ADPS     0x07    /* prescaler                */
#define ADCSRA_ADIE     0x08    /* interrupt enable         */
#define ADCSRA_ADIF     0x10    /* interrupt flag           */
#define ADCSRA_ADATE    0x20    /* auto trigger enable      */
#define ADCSRA_ADSC     0x40    /* start conversion         */
#define ADCSRA_ADEN     0x80    /* enable                   */

/* ADCSRB */
#define ADCSRB_ADTS     0x07    /* auto trigger source      */
#define ADTS_FREE_RUNNING   0

/* ADMUX */
#define ADMUX_MUX       0x1f
#define ADMUX_ADLAR     0x20

/* The two MUX settings that are not a pin. */
#define MUX_BANDGAP     0x1e
#define MUX_GND         0x1f
#define BANDGAP_MV      1100

/* A conversion is thirteen ADC clocks, and the divider is never below two. */
#define ADC_CYCLES      13

static const uint8_t avr_adc_prescaler[8] = { 2, 2, 4, 8, 16, 32, 64, 128 };

static void avr_adc_start(AVRAdcState *s);

static uint16_t avr_adc_sample(AVRAdcState *s)
{
    uint8_t mux = s->admux & ADMUX_MUX;
    uint64_t mv;

    if (mux == MUX_GND) {
        return 0;
    } else if (mux == MUX_BANDGAP) {
        mv = BANDGAP_MV;
    } else if (mux < s->channels) {
        mv = s->input_mv[mux];
    } else {
        /*
         * Above the pin count the settings select differential pairs and
         * gain stages, which need an analog front end this does not have.
         */
        qemu_log_mask(LOG_UNIMP,
                      "avr_adc: MUX 0x%02x is not a single ended channel\n",
                      mux);
        return 0;
    }

    if (!s->vref_mv) {
        return 0;
    }
    return MIN(mv * 1024 / s->vref_mv, 1023);
}

static void avr_adc_complete(void *opaque)
{
    AVRAdcState *s = opaque;

    if (!s->result_held) {
        s->result = avr_adc_sample(s);
    }

    s->adcsra &= ~ADCSRA_ADSC;
    if (s->adcsra & ADCSRA_ADIE) {
        /*
         * The core clears ADIF as it enters the vector, and there is no way
         * to see that happen from here, so the flag is left to the firmware
         * that polls it -- which does so with the interrupt disabled.
         */
        qemu_set_irq(s->irq, 1);
    } else {
        s->adcsra |= ADCSRA_ADIF;
    }

    /* Free running starts the next conversion as soon as this one lands. */
    if ((s->adcsra & ADCSRA_ADATE) &&
        (s->adcsrb & ADCSRB_ADTS) == ADTS_FREE_RUNNING) {
        s->adcsra |= ADCSRA_ADSC;
        avr_adc_start(s);
    }
}

static void avr_adc_start(AVRAdcState *s)
{
    unsigned divider = avr_adc_prescaler[s->adcsra & ADCSRA_ADPS];
    uint64_t ns;

    if (!s->enabled || !(s->adcsra & ADCSRA_ADEN)) {
        s->adcsra &= ~ADCSRA_ADSC;
        timer_del(s->timer);
        return;
    }

    ns = (uint64_t)ADC_CYCLES * divider * NANOSECONDS_PER_SECOND /
         s->cpu_freq_hz;
    timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ns);
}

static uint64_t avr_adc_read(void *opaque, hwaddr offset, unsigned size)
{
    AVRAdcState *s = opaque;
    uint16_t result;

    if (!s->enabled) {
        return 0;
    }

    result = (s->admux & ADMUX_ADLAR) ? s->result << 6 : s->result;

    switch (offset) {
    case R_ADCL:
        /*
         * Reading ADCL holds the pair steady so that the high byte still
         * belongs to the same conversion when it is read next.
         */
        s->result_held = true;
        return result & 0xff;
    case R_ADCH:
        s->result_held = false;
        return result >> 8;
    case R_ADCSRA:
        return s->adcsra;
    case R_ADCSRB:
        return s->adcsrb;
    case R_ADMUX:
        return s->admux;
    case R_DIDR0:
        return s->didr0;
    case R_DIDR1:
        return s->didr1;
    case R_DIDR2:
        return s->didr2;
    default:
        g_assert_not_reached();
    }
}

static void avr_adc_write(void *opaque, hwaddr offset, uint64_t value,
                          unsigned size)
{
    AVRAdcState *s = opaque;
    uint8_t val8 = value;

    if (!s->enabled) {
        return;
    }

    switch (offset) {
    case R_ADCL:
    case R_ADCH:
        qemu_log_mask(LOG_GUEST_ERROR, "avr_adc: the result is read only\n");
        break;
    case R_ADCSRA:
        /* ADIF is cleared by writing a one to it. */
        if (val8 & ADCSRA_ADIF) {
            s->adcsra &= ~ADCSRA_ADIF;
        }
        s->adcsra = (s->adcsra & ADCSRA_ADIF) |
                    (val8 & ~ADCSRA_ADIF);

        if (!(s->adcsra & ADCSRA_ADEN)) {
            s->adcsra &= ~ADCSRA_ADSC;
            timer_del(s->timer);
        } else if (s->adcsra & ADCSRA_ADSC) {
            avr_adc_start(s);
        }
        break;
    case R_ADCSRB:
        s->adcsrb = val8;
        if ((s->adcsra & ADCSRA_ADATE) &&
            (val8 & ADCSRB_ADTS) != ADTS_FREE_RUNNING) {
            qemu_log_mask(LOG_UNIMP,
                          "avr_adc: auto trigger source %u is not modelled\n",
                          val8 & ADCSRB_ADTS);
        }
        break;
    case R_ADMUX:
        s->admux = val8;
        break;
    case R_DIDR0:
        s->didr0 = val8;
        break;
    case R_DIDR1:
        s->didr1 = val8;
        break;
    case R_DIDR2:
        s->didr2 = val8;
        break;
    default:
        g_assert_not_reached();
    }
}

static const MemoryRegionOps avr_adc_ops = {
    .read = avr_adc_read,
    .write = avr_adc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 1 },
    .valid = { .min_access_size = 1, .max_access_size = 1 },
};

static void avr_adc_reset(DeviceState *dev)
{
    AVRAdcState *s = AVR_ADC(dev);

    s->adcsra = 0;
    s->adcsrb = 0;
    s->admux = 0;
    s->didr0 = 0;
    s->didr1 = 0;
    s->didr2 = 0;
    s->result = 0;
    s->result_held = false;
    if (s->timer) {
        timer_del(s->timer);
    }
    qemu_set_irq(s->irq, 0);
}

static void avr_adc_pr(void *opaque, int irq, int level)
{
    AVRAdcState *s = AVR_ADC(opaque);

    s->enabled = !level;
    if (!s->enabled) {
        avr_adc_reset(DEVICE(s));
    }
}

static void avr_adc_init(Object *obj)
{
    AVRAdcState *s = AVR_ADC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    unsigned i;

    memory_region_init_io(&s->mmio, obj, &avr_adc_ops, s,
                          TYPE_AVR_ADC, AVR_ADC_REGS_SIZE);
    sysbus_init_mmio(sbd, &s->mmio);
    sysbus_init_irq(sbd, &s->irq);
    qdev_init_gpio_in(DEVICE(obj), avr_adc_pr, 1);

    /*
     * What each pin is being driven to, so a machine or a test can work the
     * converter without an analog model behind it.  They are here rather
     * than in realize so that -global can set them.
     */
    for (i = 0; i < AVR_ADC_MAX_CHANNELS; i++) {
        g_autofree char *name = g_strdup_printf("input%u", i);

        object_property_add_uint32_ptr(obj, name, &s->input_mv[i],
                                       OBJ_PROP_FLAG_READWRITE);
    }
}

static void avr_adc_realize(DeviceState *dev, Error **errp)
{
    AVRAdcState *s = AVR_ADC(dev);

    if (s->cpu_freq_hz == 0) {
        error_setg(errp, "AVR ADC: cpu-frequency-hz property must be set");
        return;
    }
    if (s->channels == 0 || s->channels > AVR_ADC_MAX_CHANNELS) {
        error_setg(errp, "AVR ADC: channels must be between 1 and %d",
                   AVR_ADC_MAX_CHANNELS);
        return;
    }

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, avr_adc_complete, s);
    s->enabled = true;
}

static const Property avr_adc_properties[] = {
    DEFINE_PROP_UINT32("channels", AVRAdcState, channels, 8),
    DEFINE_PROP_UINT32("vref-mv", AVRAdcState, vref_mv, 5000),
    DEFINE_PROP_UINT64("cpu-frequency-hz", AVRAdcState, cpu_freq_hz, 0),
};

static const VMStateDescription vmstate_avr_adc = {
    .name = "avr-adc",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(adcsra, AVRAdcState),
        VMSTATE_UINT8(adcsrb, AVRAdcState),
        VMSTATE_UINT8(admux, AVRAdcState),
        VMSTATE_UINT8(didr0, AVRAdcState),
        VMSTATE_UINT8(didr1, AVRAdcState),
        VMSTATE_UINT8(didr2, AVRAdcState),
        VMSTATE_UINT16(result, AVRAdcState),
        VMSTATE_BOOL(result_held, AVRAdcState),
        VMSTATE_BOOL(enabled, AVRAdcState),
        VMSTATE_UINT32_ARRAY(input_mv, AVRAdcState, AVR_ADC_MAX_CHANNELS),
        VMSTATE_TIMER_PTR(timer, AVRAdcState),
        VMSTATE_END_OF_LIST()
    }
};

static void avr_adc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = avr_adc_realize;
    dc->vmsd = &vmstate_avr_adc;
    device_class_set_legacy_reset(dc, avr_adc_reset);
    device_class_set_props(dc, avr_adc_properties);
}

static const TypeInfo avr_adc_info = {
    .name          = TYPE_AVR_ADC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AVRAdcState),
    .instance_init = avr_adc_init,
    .class_init    = avr_adc_class_init,
};

static void avr_adc_register_types(void)
{
    type_register_static(&avr_adc_info);
}

type_init(avr_adc_register_types)
