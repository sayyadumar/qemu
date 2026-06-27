/*
 * Renesas RX65N Evaluation Kit (RSKRX65N)
 *
 * Datasheet: RX65N Group, RX651 Group User's Manual: Hardware
 *            (Rev.1.00 R01UH0590EJ0100)
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
#include "qemu/guest-random.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "elf.h"
#include "hw/loader.h"
#include "hw/core/split-irq.h"
#include "hw/misc/led.h"
#include "hw/qdev-properties.h"
#include "hw/rx/rx65n.h"
#include "system/qtest.h"
#include "system/device_tree.h"
#include "system/reset.h"
#include "hw/boards.h"
#include "qom/object.h"

#define TYPE_RX65N_EVK_MACHINE  MACHINE_TYPE_NAME("rx65n-evk-common")

struct RX65NEvkMachineClass {
    /*< private >*/
    MachineClass parent_class;
    /*< public >*/
    const char *mcu_name;
    uint32_t    xtal_freq_hz;
    uint32_t    cflash_base;
    bool        board_leds;     /* wire RSK on-board user LEDs to GPIO */
};

/*
 * RSK RX65N-2MB on-board user LED pin assignments (port * 8 + bit).
 * Per the Renesas Starter Kit+ schematic the four user LEDs sit on PORT0;
 * adjust here if targeting a different board revision.
 */
#define RSK_NR_LEDS         4
static const int rsk_led_pins[RSK_NR_LEDS] = {
    0x00 * 8 + 3,   /* LED0 = P03 */
    0x00 * 8 + 5,   /* LED1 = P05 */
    0x02 * 8 + 7,   /* LED2 = P27 */
    0x03 * 8 + 0,   /* LED3 = P30 */
};
typedef struct RX65NEvkMachineClass RX65NEvkMachineClass;

/*
 * RSK RX65N-2MB user switches. Each switch is wired to both a port input pin
 * (readable via PIDR) and an external IRQ pin at the ICU; asserting the line
 * presses the switch. The external IRQ vector for IRQn is 64 + n.
 * Pin/IRQ choices follow the Starter Kit+ schematic (best effort).
 */
#define RSK_NR_SWITCHES     3
#define RX65N_IRQ0_VECTOR   64
static const struct {
    int pin;        /* GPIO port * 8 + bit */
    int irqn;       /* external IRQ number */
} rsk_switches[RSK_NR_SWITCHES] = {
    { 0x00 * 8 + 0,  8 },   /* SW1 = P00 / IRQ8  */
    { 0x00 * 8 + 1,  9 },   /* SW2 = P01 / IRQ9  */
    { 0x00 * 8 + 2, 10 },   /* SW3 = P02 / IRQ10 */
};

struct RX65NEvkMachineState {
    /*< private >*/
    MachineState parent_obj;
    /*< public >*/
    RX65NState mcu;

    /* Board-level switch source lines (fan out to GPIO input + ICU IRQ). */
    qemu_irq switch_in[RSK_NR_SWITCHES];
};
typedef struct RX65NEvkMachineState RX65NEvkMachineState;

DECLARE_OBJ_CHECKERS(RX65NEvkMachineState, RX65NEvkMachineClass,
                     RX65N_EVK_MACHINE, TYPE_RX65N_EVK_MACHINE)

static void rx_load_image(RXCPU *cpu, const char *filename,
                          uint32_t start, uint32_t size)
{
    static uint32_t extable[32];
    uint64_t entry;
    long kernel_size;
    int i;

    /*
     * A bare-metal RX firmware is an ELF whose segments carry their own load
     * addresses (code flash plus the fixed vector table at 0xffffff80). Load
     * those segments to their physical addresses; the CPU reset then fetches
     * the reset vector from flash and PC is set up automatically.
     */
    kernel_size = load_elf(filename, NULL, NULL, NULL, &entry,
                           NULL, NULL, NULL, ELFDATA2LSB, EM_RX, 0, 0);
    if (kernel_size > 0) {
        cpu->env.pc = entry;
        return;
    }

    /* Otherwise treat it as a raw binary image (e.g. a Linux kernel). */
    kernel_size = load_image_targphys(filename, start, size);
    if (kernel_size < 0) {
        fprintf(stderr, "qemu: could not load kernel '%s'\n", filename);
        exit(1);
    }
    cpu->env.pc = start;

    /* Set up exception trap trampoline for Linux (little-endian only) */
    for (i = 0; i < ARRAY_SIZE(extable); i++) {
        extable[i] = cpu_to_le32(0x10 + i * 4);
    }
    rom_add_blob_fixed("extable", extable, sizeof(extable), VECTOR_TABLE_BASE);
}

static void rx65n_evk_init(MachineState *machine)
{
    MachineClass *mc = MACHINE_GET_CLASS(machine);
    RX65NEvkMachineState *s = RX65N_EVK_MACHINE(machine);
    RX65NEvkMachineClass *rxc = RX65N_EVK_MACHINE_GET_CLASS(machine);
    MemoryRegion *sysmem = get_system_memory();
    const char *kernel_filename = machine->kernel_filename;
    const char *dtb_filename = machine->dtb;
    uint8_t rng_seed[32];

    if (machine->ram_size < mc->default_ram_size) {
        char *sz = size_to_str(mc->default_ram_size);
        error_report("Invalid RAM size, should be more than %s", sz);
        g_free(sz);
        exit(1);
    }

    /* Map external SDRAM at chip-select base */
    memory_region_add_subregion(sysmem, EXT_CS_BASE, machine->ram);

    /* Initialize MCU */
    object_initialize_child(OBJECT(machine), "mcu", &s->mcu, rxc->mcu_name);
    object_property_set_link(OBJECT(&s->mcu), "main-bus", OBJECT(sysmem),
                             &error_abort);
    object_property_set_uint(OBJECT(&s->mcu), "xtal-frequency-hz",
                             rxc->xtal_freq_hz, &error_abort);
    object_property_set_bool(OBJECT(&s->mcu), "load-kernel",
                             kernel_filename != NULL, &error_abort);

    if (!kernel_filename) {
        if (machine->firmware) {
            rom_add_file_fixed(machine->firmware, rxc->cflash_base, 0);
        }
    }

    qdev_realize(DEVICE(&s->mcu), NULL, &error_abort);

    /* Wire the RSK on-board user LEDs to the MCU GPIO output lines. */
    if (rxc->board_leds) {
        DeviceState *gpio = DEVICE(&s->mcu.gpio);
        int i;

        for (i = 0; i < RSK_NR_LEDS; i++) {
            g_autofree char *name = g_strdup_printf("rsk-led%d", i);
            DeviceState *led = DEVICE(led_create_simple(OBJECT(machine),
                                                        GPIO_POLARITY_ACTIVE_HIGH,
                                                        LED_COLOR_GREEN, name));
            qdev_connect_gpio_out_named(gpio, "gpio-out", rsk_led_pins[i],
                                        qdev_get_gpio_in(led, 0));
        }

        /*
         * Each user switch drives both a port input pin (PIDR) and an ICU
         * external IRQ line; a split-irq device fans the single board-level
         * source out to both. The split input is retained so a press can be
         * injected (e.g. from qtest) onto the switch line.
         */
        for (i = 0; i < RSK_NR_SWITCHES; i++) {
            DeviceState *split = qdev_new(TYPE_SPLIT_IRQ);

            qdev_prop_set_uint32(split, "num-lines", 2);
            qdev_realize_and_unref(split, NULL, &error_abort);
            qdev_connect_gpio_out(split, 0,
                                  qdev_get_gpio_in_named(gpio, "gpio-in",
                                                         rsk_switches[i].pin));
            qdev_connect_gpio_out(split, 1,
                                  qdev_get_gpio_in(DEVICE(&s->mcu.icu),
                                                   RX65N_IRQ0_VECTOR +
                                                   rsk_switches[i].irqn));
            s->switch_in[i] = qdev_get_gpio_in(split, 0);
        }
    }

    /* Load kernel and optional device tree */
    if (kernel_filename) {
        ram_addr_t kernel_offset;

        /* Load into upper half of SDRAM */
        kernel_offset = machine->ram_size / 2;
        rx_load_image(&s->mcu.cpu, kernel_filename,
                      EXT_CS_BASE + kernel_offset, kernel_offset);
        if (dtb_filename) {
            ram_addr_t dtb_offset;
            int dtb_size;
            g_autofree void *dtb = load_device_tree(dtb_filename, &dtb_size);

            if (dtb == NULL) {
                error_report("Couldn't open dtb file %s", dtb_filename);
                exit(1);
            }
            if (machine->kernel_cmdline &&
                qemu_fdt_setprop_string(dtb, "/chosen", "bootargs",
                                        machine->kernel_cmdline) < 0) {
                error_report("Couldn't set /chosen/bootargs");
                exit(1);
            }
            qemu_guest_getrandom_nofail(rng_seed, sizeof(rng_seed));
            qemu_fdt_setprop(dtb, "/chosen", "rng-seed",
                             rng_seed, sizeof(rng_seed));
            /* DTB at end of SDRAM */
            dtb_offset = ROUND_DOWN(machine->ram_size - dtb_size, 16);
            rom_add_blob_fixed("dtb", dtb, dtb_size,
                               EXT_CS_BASE + dtb_offset);
            qemu_register_reset_nosnapshotload(qemu_fdt_randomize_seeds,
                    rom_ptr(EXT_CS_BASE + dtb_offset, dtb_size));
            /* Pass DTB address to kernel in R1 */
            s->mcu.cpu.env.regs[1] = EXT_CS_BASE + dtb_offset;
        }
    }
}

static void rx65n_evk_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->init             = rx65n_evk_init;
    mc->default_cpu_type = TYPE_RX65N_CPU;
    mc->default_ram_size = 8 * MiB;
    mc->default_ram_id   = "ext-sdram";
}

static void rx65ne_evk_class_init(ObjectClass *oc, const void *data)
{
    RX65NEvkMachineClass *rxc = RX65N_EVK_MACHINE_CLASS(oc);
    MachineClass *mc = MACHINE_CLASS(oc);

    rxc->mcu_name     = TYPE_R5F565NE_MCU;
    rxc->xtal_freq_hz = 12 * 1000 * 1000;
    rxc->cflash_base  = RX65N_CFLASH_BASE_512K;
    mc->desc          = "Renesas RX65N EVK (R5F565NE, 512 KB flash, 256 KB RAM)";
}

static void rx65nh_evk_class_init(ObjectClass *oc, const void *data)
{
    RX65NEvkMachineClass *rxc = RX65N_EVK_MACHINE_CLASS(oc);
    MachineClass *mc = MACHINE_CLASS(oc);

    rxc->mcu_name     = TYPE_R5F565NH_MCU;
    rxc->xtal_freq_hz = 12 * 1000 * 1000;
    rxc->cflash_base  = RX65N_CFLASH_BASE_2M;
    mc->desc          = "Renesas RX65N EVK (R5F565NH, 2 MB flash, 640 KB RAM)";
}

static void rsk_rx65n_2mb_class_init(ObjectClass *oc, const void *data)
{
    RX65NEvkMachineClass *rxc = RX65N_EVK_MACHINE_CLASS(oc);
    MachineClass *mc = MACHINE_CLASS(oc);

    /* Renesas Starter Kit+ for RX65N-2MB: R5F565NEH, 24 MHz crystal. */
    rxc->mcu_name     = TYPE_R5F565NH_MCU;
    rxc->xtal_freq_hz = 24 * 1000 * 1000;
    rxc->cflash_base  = RX65N_CFLASH_BASE_2M;
    rxc->board_leds   = true;
    mc->desc          = "Renesas Starter Kit+ for RX65N-2MB (R5F565NEH)";
}

static const TypeInfo rx65n_evk_types[] = {
    {
        .name       = MACHINE_TYPE_NAME("rx65n-r5f565ne-evk"),
        .parent     = TYPE_RX65N_EVK_MACHINE,
        .class_init = rx65ne_evk_class_init,
    }, {
        .name       = MACHINE_TYPE_NAME("rx65n-r5f565nh-evk"),
        .parent     = TYPE_RX65N_EVK_MACHINE,
        .class_init = rx65nh_evk_class_init,
    }, {
        .name       = MACHINE_TYPE_NAME("rsk-rx65n-2mb"),
        .parent     = TYPE_RX65N_EVK_MACHINE,
        .class_init = rsk_rx65n_2mb_class_init,
    }, {
        .name          = TYPE_RX65N_EVK_MACHINE,
        .parent        = TYPE_MACHINE,
        .instance_size = sizeof(RX65NEvkMachineState),
        .class_size    = sizeof(RX65NEvkMachineClass),
        .class_init    = rx65n_evk_class_init,
        .abstract      = true,
    }
};

DEFINE_TYPES(rx65n_evk_types)
