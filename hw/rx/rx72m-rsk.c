/*
 * Renesas Starter Kit+ for RX72M (RSK RX72M)
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
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "elf.h"
#include "hw/loader.h"
#include "hw/core/split-irq.h"
#include "hw/misc/led.h"
#include "hw/qdev-properties.h"
#include "hw/rx/rx72m.h"
#include "hw/boards.h"
#include "qom/object.h"

#define TYPE_RSK_RX72M_MACHINE  MACHINE_TYPE_NAME("rsk-rx72m")

/*
 * RSK RX72M on-board user LED / switch pin assignments (port * 8 + bit) and
 * external IRQ numbers, following the Starter Kit+ schematic (best effort).
 */
#define RSK_NR_LEDS         4
static const int rsk_led_pins[RSK_NR_LEDS] = {
    0x00 * 8 + 3,   /* LED0 = P03 */
    0x00 * 8 + 5,   /* LED1 = P05 */
    0x02 * 8 + 7,   /* LED2 = P27 */
    0x03 * 8 + 0,   /* LED3 = P30 */
};

#define RSK_NR_SWITCHES     3
#define RX72M_IRQ0_VECTOR   64
static const struct {
    int pin;
    int irqn;
} rsk_switches[RSK_NR_SWITCHES] = {
    { 0x00 * 8 + 0,  8 },   /* SW1 = P00 / IRQ8  */
    { 0x00 * 8 + 1,  9 },   /* SW2 = P01 / IRQ9  */
    { 0x00 * 8 + 2, 10 },   /* SW3 = P02 / IRQ10 */
};

struct RSKRX72MMachineState {
    /*< private >*/
    MachineState parent_obj;
    /*< public >*/
    RX72MState mcu;
    qemu_irq switch_in[RSK_NR_SWITCHES];
};
typedef struct RSKRX72MMachineState RSKRX72MMachineState;

DECLARE_INSTANCE_CHECKER(RSKRX72MMachineState, RSK_RX72M_MACHINE,
                         TYPE_RSK_RX72M_MACHINE)

static void rx72m_load_firmware(RXCPU *cpu, const char *filename,
                                uint32_t start, uint32_t size)
{
    uint64_t entry;
    long kernel_size;

    kernel_size = load_elf(filename, NULL, NULL, NULL, &entry,
                           NULL, NULL, NULL, ELFDATA2LSB, EM_RX, 0, 0);
    if (kernel_size > 0) {
        cpu->env.pc = entry;
        return;
    }

    kernel_size = load_image_targphys(filename, start, size);
    if (kernel_size < 0) {
        error_report("Could not load kernel '%s'", filename);
        exit(1);
    }
    cpu->env.pc = start;
}

static void rsk_rx72m_init(MachineState *machine)
{
    MachineClass *mc = MACHINE_GET_CLASS(machine);
    RSKRX72MMachineState *s = RSK_RX72M_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    const char *kernel_filename = machine->kernel_filename;
    DeviceState *gpio;
    int i;

    if (machine->ram_size < mc->default_ram_size) {
        char *sz = size_to_str(mc->default_ram_size);
        error_report("Invalid RAM size, should be more than %s", sz);
        g_free(sz);
        exit(1);
    }

    /* External SDRAM at the chip-select base. */
    memory_region_add_subregion(sysmem, EXT_CS_BASE, machine->ram);

    object_initialize_child(OBJECT(machine), "mcu", &s->mcu,
                            TYPE_R5F572MN_MCU);
    object_property_set_link(OBJECT(&s->mcu), "main-bus", OBJECT(sysmem),
                             &error_abort);
    object_property_set_uint(OBJECT(&s->mcu), "xtal-frequency-hz",
                             24 * 1000 * 1000, &error_abort);
    object_property_set_bool(OBJECT(&s->mcu), "load-kernel",
                             kernel_filename != NULL, &error_abort);

    if (!kernel_filename && machine->firmware) {
        rom_add_file_fixed(machine->firmware, RX72M_CFLASH_BASE_4M, 0);
    }

    qdev_realize(DEVICE(&s->mcu), NULL, &error_abort);

    /* On-board user LEDs and switches. */
    gpio = DEVICE(&s->mcu.gpio);
    for (i = 0; i < RSK_NR_LEDS; i++) {
        g_autofree char *name = g_strdup_printf("rsk-led%d", i);
        DeviceState *led = DEVICE(led_create_simple(OBJECT(machine),
                                                    GPIO_POLARITY_ACTIVE_HIGH,
                                                    LED_COLOR_GREEN, name));
        qdev_connect_gpio_out_named(gpio, "gpio-out", rsk_led_pins[i],
                                    qdev_get_gpio_in(led, 0));
    }
    for (i = 0; i < RSK_NR_SWITCHES; i++) {
        DeviceState *split = qdev_new(TYPE_SPLIT_IRQ);

        qdev_prop_set_uint32(split, "num-lines", 2);
        qdev_realize_and_unref(split, NULL, &error_abort);
        qdev_connect_gpio_out(split, 0,
                              qdev_get_gpio_in_named(gpio, "gpio-in",
                                                     rsk_switches[i].pin));
        qdev_connect_gpio_out(split, 1,
                              qdev_get_gpio_in(DEVICE(&s->mcu.icu),
                                               RX72M_IRQ0_VECTOR +
                                               rsk_switches[i].irqn));
        s->switch_in[i] = qdev_get_gpio_in(split, 0);
    }

    if (kernel_filename) {
        ram_addr_t kernel_offset = machine->ram_size / 2;
        rx72m_load_firmware(&s->mcu.cpu, kernel_filename,
                            EXT_CS_BASE + kernel_offset, kernel_offset);
    }
}

static void rsk_rx72m_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc             = "Renesas Starter Kit+ for RX72M (R5F572MN)";
    mc->init             = rsk_rx72m_init;
    mc->default_cpu_type = TYPE_RX72M_CPU;
    mc->default_ram_size = 8 * MiB;
    mc->default_ram_id   = "ext-sdram";
}

static const TypeInfo rsk_rx72m_types[] = {
    {
        .name          = TYPE_RSK_RX72M_MACHINE,
        .parent        = TYPE_MACHINE,
        .instance_size = sizeof(RSKRX72MMachineState),
        .class_init    = rsk_rx72m_class_init,
    }
};

DEFINE_TYPES(rsk_rx72m_types)
