/*
 * Renesas RX Flash Control Unit (FCU) with FACI command interface
 *
 * Datasheet: RX65N Group, RX651 Group User's Manual: Hardware
 *            (Rev.1.00 R01UH0590EJ0100), section 6 (Flash Memory)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
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

#ifndef HW_MISC_RENESAS_RX_FCU_H
#define HW_MISC_RENESAS_RX_FCU_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_RENESAS_RX_FCU "renesas-rx-fcu"
typedef struct RenesasRxFcuState RenesasRxFcuState;
DECLARE_INSTANCE_CHECKER(RenesasRxFcuState, RENESAS_RX_FCU, TYPE_RENESAS_RX_FCU)

/* The FACI control register block (0x007FE000) covers the first 4 KiB. */
#define RX_FCU_REGS_SIZE    0x1000

/* sysbus MMIO regions exposed by the device. */
enum {
    RX_FCU_MMIO_REGS = 0,   /* FACI control/status registers      */
    RX_FCU_MMIO_CFLASH,     /* code flash array (rom_device)      */
    RX_FCU_MMIO_DFLASH,     /* data flash array (rom_device)      */
    RX_FCU_NR_MMIO,
};

/* sysbus IRQ lines. */
enum {
    RX_FCU_IRQ_FRDYI = 0,   /* flash ready interrupt   */
    RX_FCU_IRQ_FIFERR,      /* flash access error      */
    RX_FCU_NR_IRQ,
};

/* Identifies which flash array a FACI command targets. */
typedef enum {
    RX_FCU_CFLASH = 0,
    RX_FCU_DFLASH = 1,
} RxFcuTarget;

/*
 * Per-array context handed to the flash MemoryRegion ops so a single set of
 * callbacks can serve both the code and data flash arrays.
 */
typedef struct RxFcuFlash {
    RenesasRxFcuState *fcu;
    RxFcuTarget target;
} RxFcuFlash;

/* FACI command sequencer state. */
typedef enum {
    RX_FCU_ST_READY = 0,
    RX_FCU_ST_PROGRAM_COUNT,    /* awaiting the data-word count       */
    RX_FCU_ST_PROGRAM_DATA,     /* receiving data words, then 0xD0     */
    RX_FCU_ST_ERASE,            /* awaiting the 0xD0 confirmation      */
    RX_FCU_ST_BLANKCHECK,       /* awaiting the 0xD0 confirmation      */
} RxFcuCmdState;

struct RenesasRxFcuState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion regs_mr;
    MemoryRegion cflash_mr;
    MemoryRegion dflash_mr;

    RxFcuFlash cflash_ctx;
    RxFcuFlash dflash_ctx;

    qemu_irq frdyi;
    qemu_irq fiferr;

    /* Geometry (set via properties by the MCU). */
    uint32_t cflash_size;
    uint32_t dflash_size;
    uint32_t cflash_base;       /* absolute CPU address of code flash */
    uint32_t dflash_base;       /* absolute CPU address of data flash */

    /* Backing storage pointers (into the rom_device RAM). */
    uint8_t *cflash_ptr;
    uint8_t *dflash_ptr;

    /* FACI registers. */
    uint16_t fentryr;           /* P/E mode entry (read-back form)    */
    uint32_t fstatr;            /* flash status                       */
    uint8_t  fastat;            /* flash access status                */
    uint8_t  frdyie;            /* flash ready interrupt enable       */
    uint8_t  faeint;            /* flash access error interrupt enable */
    uint32_t fsaddr;            /* processing start address           */
    uint32_t fpsaddr;           /* programmed/erased start address    */
    uint32_t feaddr;            /* processing end address             */
    uint8_t  fbcstat;           /* blank check status                 */
    uint16_t fbccnt;            /* blank check control                */
    uint16_t fcpsr;             /* clear/processing switch            */
    uint16_t fpckar;            /* clock notification                 */
    uint16_t fprotr;            /* protection                         */
    uint16_t fsuacr;            /* startup area control               */
    uint16_t fpestat;           /* P/E error status                   */
    uint16_t fcmdr;             /* last FACI command (read-only)      */

    /* Command sequencer (RxFcuCmdState / RxFcuTarget, stored as int32 for
     * migration). */
    int32_t cmd_state;
    int32_t cmd_target;         /* array of the in-progress command   */
    uint32_t      prog_off;     /* next program offset within array   */
    uint32_t      prog_words;   /* remaining 16-bit words to program  */
};

#endif /* HW_MISC_RENESAS_RX_FCU_H */
