/*
 * AVR 8-bit timer
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
 * TIMER0 and TIMER2 on the ATmega parts.  The counter is not stepped; it is
 * derived from the virtual clock, and an alarm is scheduled for whichever
 * event in the current waveform period comes next.
 *
 * The waveform generation mode decides both where the counter turns around
 * and how it gets there.  Normal, CTC and fast PWM are single slope: the
 * counter runs 0..TOP and starts over, so a period is TOP + 1 ticks.  The
 * phase correct modes are dual slope: the counter runs up to TOP and back
 * down to zero, so a period is 2 * TOP ticks and a compare value is passed
 * twice.  Everything below works in terms of a position within a period,
 * which makes both shapes the same problem.
 *
 * The compare output pins are not modelled -- there is nothing on the other
 * end of them -- so a mode is only as good as its interrupts here.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/timer/avr_timer8.h"
#include "migration/vmstate.h"

/* Register offsets */
#define T8_CCRA     0x0
#define T8_CCRB     0x1
#define T8_CNT      0x2
#define T8_OCRA     0x3
#define T8_OCRB     0x4
#define T8_REGS_SIZE 0x5

/* TCCRnA */
#define T8_CCRA_WGM01   0x03
#define T8_CCRA_COMB    0x30
#define T8_CCRA_COMA    0xc0
#define T8_CCRA_OC_CONF (T8_CCRA_COMA | T8_CCRA_COMB)

/* TCCRnB */
#define T8_CCRB_CS      0x07
#define T8_CCRB_WGM2    0x08
#define T8_CCRB_FOCB    0x40
#define T8_CCRB_FOCA    0x80

/* Both TIMSKn and TIFRn */
#define T8_INT_TOV      0x01    /* counter overflow  */
#define T8_INT_OCA      0x02    /* output compare A  */
#define T8_INT_OCB      0x04    /* output compare B  */
#define T8_INT_MASK     (T8_INT_TOV | T8_INT_OCA | T8_INT_OCB)

/* Waveform generation modes, WGM2:0 */
#define T8_MODE_NORMAL          0
#define T8_MODE_PWM_PC          1   /* phase correct, TOP = 0xff */
#define T8_MODE_CTC             2   /* TOP = OCRA                */
#define T8_MODE_PWM_FAST        3   /* TOP = 0xff                */
#define T8_MODE_PWM_PC_OCRA     5   /* phase correct, TOP = OCRA */
#define T8_MODE_PWM_FAST_OCRA   7   /* TOP = OCRA                */

#define CLKSRC(t8)  ((t8)->ccrb & T8_CCRB_CS)
#define MODE(t8)    ((((t8)->ccrb & T8_CCRB_WGM2) >> 1) | \
                     ((t8)->ccra & T8_CCRA_WGM01))

/*
 * Clock select encodings.  Zero means the counter does not advance: either
 * the source is stopped, or it is the external T pin, which nothing drives.
 */
static const uint16_t avr_timer8_prescaler[8] = {
    0, 1, 8, 64, 256, 1024, 0, 0
};

static const uint16_t avr_timer8_prescaler_alt[8] = {
    0, 1, 8, 32, 64, 128, 256, 1024
};

typedef struct {
    uint32_t pos;
    uint8_t flag;
} AVRTimer8Event;

#define T8_MAX_EVENTS   5

static void avr_timer8_set_alarm(AVRTimer8State *t8);

static bool avr_timer8_dual_slope(AVRTimer8State *t8)
{
    return MODE(t8) == T8_MODE_PWM_PC || MODE(t8) == T8_MODE_PWM_PC_OCRA;
}

static uint32_t avr_timer8_top(AVRTimer8State *t8)
{
    switch (MODE(t8)) {
    case T8_MODE_CTC:
    case T8_MODE_PWM_PC_OCRA:
    case T8_MODE_PWM_FAST_OCRA:
        return t8->ocra;
    default:
        return 0xff;
    }
}

static uint32_t avr_timer8_period_ticks(AVRTimer8State *t8)
{
    uint32_t top = avr_timer8_top(t8);

    if (avr_timer8_dual_slope(t8)) {
        /* With a TOP of zero there is nowhere to count to. */
        return top ? 2 * top : 1;
    }
    return top + 1;
}

/* Where the counter stands at a given position within a period. */
static uint8_t avr_timer8_cnt_at(AVRTimer8State *t8, uint32_t pos)
{
    uint32_t top = avr_timer8_top(t8);

    if (avr_timer8_dual_slope(t8) && pos > top) {
        return 2 * top - pos;
    }
    return pos;
}

/* How many ticks the counter has seen since it last stood at zero. */
static uint64_t avr_timer8_elapsed(AVRTimer8State *t8)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (!t8->period_ns || now <= t8->reset_time_ns) {
        return 0;
    }
    return (now - t8->reset_time_ns) / t8->period_ns;
}

/* Re-anchor the counter so that it reads `cnt` as of now. */
static void avr_timer8_set_cnt(AVRTimer8State *t8, uint8_t cnt)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    t8->reset_time_ns = now - (int64_t)cnt * (int64_t)t8->period_ns;
}

/*
 * The events one waveform period can produce, as positions within it.  A
 * dual slope period passes each compare value twice, once on the way up and
 * once on the way back down.
 */
static int avr_timer8_events(AVRTimer8State *t8, AVRTimer8Event *ev)
{
    uint32_t top = avr_timer8_top(t8);
    bool dual = avr_timer8_dual_slope(t8);
    uint8_t ocr[2] = { t8->ocra, t8->ocrb };
    uint8_t flag[2] = { T8_INT_OCA, T8_INT_OCB };
    int n = 0;
    int i;

    /*
     * A single slope period sets TOV as the counter rolls off TOP and a dual
     * slope one sets it at BOTTOM; either way that is position zero of the
     * next period.  CTC is the exception: it clears the counter at OCRA, so
     * it only reaches MAX -- where TOV is defined to be set -- when OCRA is
     * itself MAX.
     */
    if (MODE(t8) != T8_MODE_CTC || top == 0xff) {
        ev[n].pos = 0;
        ev[n++].flag = T8_INT_TOV;
    }

    for (i = 0; i < 2; i++) {
        if (ocr[i] > top) {
            continue;           /* the counter turns around before it */
        }
        ev[n].pos = ocr[i];
        ev[n++].flag = flag[i];
        if (dual && ocr[i] != 0 && ocr[i] != top) {
            ev[n].pos = 2 * top - ocr[i];
            ev[n++].flag = flag[i];
        }
    }

    return n;
}

/*
 * The flag is what firmware polls, and the core clears it as it enters the
 * vector.  QEMU's AVR core gives the device no way to see that happen, so
 * clear it here as the request goes out: an interrupt that is masked in is
 * going to be taken, while firmware that polls instead does so with the
 * interrupt disabled, and there the flag is left standing until a one is
 * written to it.
 */
static void avr_timer8_raise(AVRTimer8State *t8, uint8_t flag)
{
    qemu_irq irq;

    switch (flag) {
    case T8_INT_TOV:
        irq = t8->ovf_irq;
        break;
    case T8_INT_OCA:
        irq = t8->compa_irq;
        break;
    case T8_INT_OCB:
        irq = t8->compb_irq;
        break;
    default:
        g_assert_not_reached();
    }

    if (t8->imsk & flag) {
        qemu_set_irq(irq, 1);
    } else {
        t8->ifr |= flag;
    }
}

static void avr_timer8_alarm(void *opaque)
{
    AVRTimer8State *t8 = opaque;
    AVRTimer8Event ev[T8_MAX_EVENTS];
    uint32_t pos;
    int n, i;

    if (!t8->enabled || !t8->period_ns) {
        return;
    }

    /*
     * Take the position from the tick the alarm was set for rather than from
     * the clock, which may have run on past the deadline.
     */
    pos = t8->alarm_tick % avr_timer8_period_ticks(t8);

    n = avr_timer8_events(t8, ev);
    for (i = 0; i < n; i++) {
        if (ev[i].pos == pos) {
            avr_timer8_raise(t8, ev[i].flag);
        }
    }

    avr_timer8_set_alarm(t8);
}

static void avr_timer8_set_alarm(AVRTimer8State *t8)
{
    AVRTimer8Event ev[T8_MAX_EVENTS];
    uint32_t period, cur, delta = UINT32_MAX;
    uint64_t elapsed;
    int n, i;

    if (!t8->enabled || !t8->period_ns) {
        timer_del(t8->timer);
        return;
    }

    period = avr_timer8_period_ticks(t8);
    elapsed = avr_timer8_elapsed(t8);
    cur = elapsed % period;

    n = avr_timer8_events(t8, ev);
    for (i = 0; i < n; i++) {
        /* Distance to the next occurrence, in 1..period ticks. */
        uint32_t d = (ev[i].pos + period - cur - 1) % period + 1;

        if (d < delta) {
            delta = d;
        }
    }
    if (delta == UINT32_MAX) {
        timer_del(t8->timer);
        return;
    }

    t8->alarm_tick = elapsed + delta;
    timer_mod(t8->timer,
              t8->reset_time_ns + (int64_t)t8->alarm_tick * t8->period_ns);
}

static void avr_timer8_clksrc_update(AVRTimer8State *t8)
{
    const uint16_t *table = t8->alt_prescaler ? avr_timer8_prescaler_alt
                                                : avr_timer8_prescaler;
    uint16_t divider = table[CLKSRC(t8)];
    uint8_t cnt;

    if (!divider && CLKSRC(t8) > 5 && !t8->alt_prescaler) {
        qemu_log_mask(LOG_UNIMP, "%s: external clock source unsupported\n",
                      __func__);
    }

    /* Keep the counter where it is across a change of rate. */
    cnt = t8->period_ns ?
          avr_timer8_cnt_at(t8, avr_timer8_elapsed(t8)
                                % avr_timer8_period_ticks(t8)) : t8->cnt;

    t8->period_ns = divider ?
                    NANOSECONDS_PER_SECOND * divider / t8->cpu_freq_hz : 0;
    if (t8->period_ns) {
        avr_timer8_set_cnt(t8, cnt);
    } else {
        t8->cnt = cnt;          /* frozen; reads come from here */
    }
}

static uint8_t avr_timer8_read_cnt(AVRTimer8State *t8)
{
    if (!t8->period_ns) {
        return t8->cnt;
    }
    return avr_timer8_cnt_at(t8, avr_timer8_elapsed(t8)
                                 % avr_timer8_period_ticks(t8));
}

static uint64_t avr_timer8_read(void *opaque, hwaddr offset, unsigned size)
{
    AVRTimer8State *t8 = opaque;

    switch (offset) {
    case T8_CCRA:
        return t8->ccra;
    case T8_CCRB:
        return t8->ccrb;
    case T8_CNT:
        return avr_timer8_read_cnt(t8);
    case T8_OCRA:
        return t8->ocra;
    case T8_OCRB:
        return t8->ocrb;
    default:
        g_assert_not_reached();
    }
}

static void avr_timer8_write(void *opaque, hwaddr offset, uint64_t val64,
                             unsigned size)
{
    AVRTimer8State *t8 = opaque;
    uint8_t val8 = val64;
    uint8_t prev_clksrc = CLKSRC(t8);

    switch (offset) {
    case T8_CCRA:
        t8->ccra = val8;
        if (val8 & T8_CCRA_OC_CONF) {
            qemu_log_mask(LOG_UNIMP, "%s: output compare pins unsupported\n",
                          __func__);
        }
        break;
    case T8_CCRB:
        t8->ccrb = val8;
        if (val8 & (T8_CCRB_FOCA | T8_CCRB_FOCB)) {
            qemu_log_mask(LOG_UNIMP, "%s: forced output compare unsupported\n",
                          __func__);
        }
        if (CLKSRC(t8) != prev_clksrc) {
            avr_timer8_clksrc_update(t8);
        }
        break;
    case T8_CNT:
        t8->cnt = val8;
        avr_timer8_set_cnt(t8, val8);
        break;
    case T8_OCRA:
        t8->ocra = val8;
        break;
    case T8_OCRB:
        t8->ocrb = val8;
        break;
    default:
        g_assert_not_reached();
    }

    avr_timer8_set_alarm(t8);
}

static uint64_t avr_timer8_imsk_read(void *opaque, hwaddr offset, unsigned size)
{
    AVRTimer8State *t8 = opaque;

    return t8->imsk;
}

static void avr_timer8_imsk_write(void *opaque, hwaddr offset, uint64_t val64,
                                  unsigned size)
{
    AVRTimer8State *t8 = opaque;

    t8->imsk = val64 & T8_INT_MASK;
}

static uint64_t avr_timer8_ifr_read(void *opaque, hwaddr offset, unsigned size)
{
    AVRTimer8State *t8 = opaque;

    return t8->ifr;
}

static void avr_timer8_ifr_write(void *opaque, hwaddr offset, uint64_t val64,
                                 unsigned size)
{
    AVRTimer8State *t8 = opaque;

    /* Writing a one clears the flag. */
    t8->ifr &= ~(val64 & T8_INT_MASK);
}

static const MemoryRegionOps avr_timer8_ops = {
    .read = avr_timer8_read,
    .write = avr_timer8_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 1 },
    .valid = { .min_access_size = 1, .max_access_size = 1 },
};

static const MemoryRegionOps avr_timer8_imsk_ops = {
    .read = avr_timer8_imsk_read,
    .write = avr_timer8_imsk_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 1 },
    .valid = { .min_access_size = 1, .max_access_size = 1 },
};

static const MemoryRegionOps avr_timer8_ifr_ops = {
    .read = avr_timer8_ifr_read,
    .write = avr_timer8_ifr_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 1 },
    .valid = { .min_access_size = 1, .max_access_size = 1 },
};

static void avr_timer8_reset(DeviceState *dev)
{
    AVRTimer8State *t8 = AVR_TIMER8(dev);

    t8->ccra = 0;
    t8->ccrb = 0;
    t8->cnt = 0;
    t8->ocra = 0;
    t8->ocrb = 0;
    t8->imsk = 0;
    t8->ifr = 0;
    t8->period_ns = 0;
    t8->reset_time_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    t8->alarm_tick = 0;

    if (t8->timer) {
        timer_del(t8->timer);
    }
    qemu_set_irq(t8->compa_irq, 0);
    qemu_set_irq(t8->compb_irq, 0);
    qemu_set_irq(t8->ovf_irq, 0);
}

static void avr_timer8_pr(void *opaque, int irq, int level)
{
    AVRTimer8State *t8 = AVR_TIMER8(opaque);

    t8->enabled = !level;
    if (!t8->enabled) {
        avr_timer8_reset(DEVICE(t8));
    }
}

static void avr_timer8_init(Object *obj)
{
    AVRTimer8State *t8 = AVR_TIMER8(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    sysbus_init_irq(sbd, &t8->compa_irq);
    sysbus_init_irq(sbd, &t8->compb_irq);
    sysbus_init_irq(sbd, &t8->ovf_irq);

    memory_region_init_io(&t8->iomem, obj, &avr_timer8_ops, t8,
                          TYPE_AVR_TIMER8, T8_REGS_SIZE);
    memory_region_init_io(&t8->imsk_iomem, obj, &avr_timer8_imsk_ops, t8,
                          TYPE_AVR_TIMER8 "-intmask", 1);
    memory_region_init_io(&t8->ifr_iomem, obj, &avr_timer8_ifr_ops, t8,
                          TYPE_AVR_TIMER8 "-intflag", 1);
    sysbus_init_mmio(sbd, &t8->iomem);
    sysbus_init_mmio(sbd, &t8->imsk_iomem);
    sysbus_init_mmio(sbd, &t8->ifr_iomem);

    qdev_init_gpio_in(DEVICE(obj), avr_timer8_pr, 1);
}

static void avr_timer8_realize(DeviceState *dev, Error **errp)
{
    AVRTimer8State *t8 = AVR_TIMER8(dev);

    if (t8->cpu_freq_hz == 0) {
        error_setg(errp, "AVR timer8: cpu-frequency-hz property must be set");
        return;
    }

    t8->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, avr_timer8_alarm, t8);
    t8->enabled = true;
}

static const Property avr_timer8_properties[] = {
    DEFINE_PROP_UINT8("id", AVRTimer8State, id, 0),
    DEFINE_PROP_BOOL("alt-prescaler", AVRTimer8State, alt_prescaler, false),
    DEFINE_PROP_UINT64("cpu-frequency-hz", AVRTimer8State, cpu_freq_hz, 0),
};

static const VMStateDescription vmstate_avr_timer8 = {
    .name = "avr-timer8",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(ccra, AVRTimer8State),
        VMSTATE_UINT8(ccrb, AVRTimer8State),
        VMSTATE_UINT8(cnt, AVRTimer8State),
        VMSTATE_UINT8(ocra, AVRTimer8State),
        VMSTATE_UINT8(ocrb, AVRTimer8State),
        VMSTATE_UINT8(imsk, AVRTimer8State),
        VMSTATE_UINT8(ifr, AVRTimer8State),
        VMSTATE_BOOL(enabled, AVRTimer8State),
        VMSTATE_UINT64(period_ns, AVRTimer8State),
        VMSTATE_INT64(reset_time_ns, AVRTimer8State),
        VMSTATE_UINT64(alarm_tick, AVRTimer8State),
        VMSTATE_TIMER_PTR(timer, AVRTimer8State),
        VMSTATE_END_OF_LIST()
    }
};

static void avr_timer8_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = avr_timer8_realize;
    dc->vmsd = &vmstate_avr_timer8;
    device_class_set_legacy_reset(dc, avr_timer8_reset);
    device_class_set_props(dc, avr_timer8_properties);
}

static const TypeInfo avr_timer8_info = {
    .name          = TYPE_AVR_TIMER8,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AVRTimer8State),
    .instance_init = avr_timer8_init,
    .class_init    = avr_timer8_class_init,
};

static void avr_timer8_register_types(void)
{
    type_register_static(&avr_timer8_info);
}

type_init(avr_timer8_register_types)
