/*
 * RX65N MCU Object
 *
 * Datasheet: RX65N Group, RX651 Group User's Manual: Hardware
 *            (Rev.1.00 R01UH0590EJ0100)
 *
 * Copyright (c) 2024 QEMU Contributors
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

#ifndef HW_RX_RX65N_H
#define HW_RX_RX65N_H

#include "target/rx/cpu.h"
#include "hw/intc/rx_icu.h"
#include "hw/timer/renesas_tmr.h"
#include "hw/timer/renesas_cmt.h"
#include "hw/timer/renesas_mtu3.h"
#include "hw/char/renesas_sci.h"
#include "hw/adc/renesas_s12ad.h"
#include "hw/ssi/renesas_rspi.h"
#include "hw/net/renesas_etherc.h"
#include "qom/object.h"

#define TYPE_RX65N_MCU      "rx65n-mcu"
typedef struct RX65NState RX65NState;
DECLARE_INSTANCE_CHECKER(RX65NState, RX65N_MCU, TYPE_RX65N_MCU)

/* Concrete MCU variants */
#define TYPE_R5F565NE_MCU   "r5f565ne-mcu"   /* 512 KB flash, 256 KB RAM */
#define TYPE_R5F565NH_MCU   "r5f565nh-mcu"   /* 2 MB flash,   640 KB RAM */

/* External chip-select base (for off-chip SDRAM/SRAM on board) */
#define EXT_CS_BASE         0x01000000
/* Exception vector table resides at the top of internal flash */
#define VECTOR_TABLE_BASE   0xffffff80

/* On-chip memory base addresses (HW manual section 5) */
#define RX65N_IRAM_BASE     0x00000000
#define RX65N_DFLASH_BASE   0x00100000
/*
 * Code flash base depends on variant flash size; it always ends at
 * 0xFFFFFFFF, so base = 0x100000000 - flash_size.
 *   512 KB variant: 0xFFF80000
 *   2 MB variant:   0xFFE00000
 */
#define RX65N_CFLASH_BASE_512K  0xFFF80000
#define RX65N_CFLASH_BASE_2M    0xFFE00000

/* Peripheral base addresses (HW manual section 5) */
#define RX65N_ICU_BASE      0x00087000
#define RX65N_TMR_BASE      0x00088200
#define RX65N_CMT_BASE      0x00088000
#define RX65N_SCI_BASE      0x00088240
#define RX65N_MTU3_BASE     0x000C1200
#define RX65N_S12AD_BASE    0x00089000
#define RX65N_RSPI0_BASE    0x000D0100
#define RX65N_ETHERC_BASE   0x000C0000

/* Phase 1: minimal peripheral counts (extend in later phases) */
#define RX65N_NR_TMR    2
#define RX65N_NR_CMT    2
#define RX65N_NR_SCI    1

struct RX65NState {
    /*< private >*/
    DeviceState parent_obj;
    /*< public >*/

    RXCPU cpu;
    RXICUState icu;
    RTMRState tmr[RX65N_NR_TMR];
    RCMTState cmt[RX65N_NR_CMT];
    RSCIState sci[RX65N_NR_SCI];
    RXMTU3State mtu3;
    RX65NS12ADState s12ad;
    RX65NRSPIState rspi;
    RX65NEthercState etherc;

    MemoryRegion *sysmem;
    bool kernel;

    MemoryRegion iram;
    MemoryRegion d_flash;
    MemoryRegion c_flash;

    /* Populated during realize; board uses this for firmware loading */
    uint32_t cflash_base;

    /* Input clock (XTAL) frequency */
    uint32_t xtal_freq_hz;
    /* Peripheral module clock frequency (derived from XTAL + PLL) */
    uint32_t pclk_freq_hz;
};

#endif /* HW_RX_RX65N_H */
