/*
 * RX72M MCU Object
 *
 * Datasheet: RX72M Group User's Manual: Hardware (R01UH0804EJ)
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

#ifndef HW_RX_RX72M_H
#define HW_RX_RX72M_H

#include "target/rx/cpu.h"
#include "hw/intc/rx_icu.h"
#include "hw/timer/renesas_tmr.h"
#include "hw/timer/renesas_cmt.h"
#include "hw/timer/renesas_mtu3.h"
#include "hw/char/renesas_sci.h"
#include "hw/adc/renesas_s12ad.h"
#include "hw/ssi/renesas_rspi.h"
#include "hw/net/renesas_etherc.h"
#include "hw/misc/rx65n_sysclk.h"
#include "hw/misc/renesas_rx_fcu.h"
#include "hw/gpio/renesas_rx_gpio.h"
#include "hw/dma/renesas_rx_dmac.h"
#include "hw/dma/renesas_rx_dtc.h"
#include "hw/watchdog/renesas_rx_wdt.h"
#include "hw/rtc/renesas_rx_rtc.h"
#include "qom/object.h"

#define TYPE_RX72M_MCU      "rx72m-mcu"
typedef struct RX72MState RX72MState;
DECLARE_INSTANCE_CHECKER(RX72MState, RX72M_MCU, TYPE_RX72M_MCU)

/* Concrete MCU variant: RSK RX72M part (4 MB flash, 512 KB RAM). */
#define TYPE_R5F572MN_MCU   "r5f572mn-mcu"

/* External chip-select base (for off-chip SDRAM/SRAM on board) */
#define EXT_CS_BASE         0x01000000
/* Exception vector table resides at the top of internal flash */
#define VECTOR_TABLE_BASE   0xffffff80

/* On-chip memory base addresses (HW manual section 5) */
#define RX72M_IRAM_BASE     0x00000000
/*
 * Primary on-chip SRAM starts at 0x00000000; the peripheral I/O register space
 * begins at 0x00080000. SRAM beyond 512 KB lives in a separate expansion
 * region at 0x00800000.
 */
#define RX72M_SRAM_MAX      (512 * 1024)
#define RX72M_EXRAM_BASE    0x00800000
#define RX72M_DFLASH_BASE   0x00100000
/*
 * Code flash always ends at 0xFFFFFFFF, so base = 0x100000000 - flash_size.
 *   4 MB variant: 0xFFC00000
 */
#define RX72M_CFLASH_BASE_4M    0xFFC00000

/* Peripheral base addresses (shared RX600-series ICUb peripheral map) */
#define RX72M_SYSTEM_BASE   0x00080000
#define RX72M_ICU_BASE      0x00087000
#define RX72M_TMR_BASE      0x00088200
#define RX72M_CMT_BASE      0x00088000
#define RX72M_SCI_BASE      0x0008A000
#define RX72M_SCI_SPACING   0x20
#define RX72M_SCI4_BASE     (RX72M_SCI_BASE + 4 * RX72M_SCI_SPACING)
/*
 * RSK RX72M routes its serial console to SCI channel 7. On the RX72M the SCIg
 * register block for channel 7 is located at 0x000D00E0 (verified against the
 * FIT driver's channel-7 ROM config in the ads1263 reference firmware), not in
 * the 0x0008A000 SCI window used by the lower channels.
 */
#define RX72M_SCI1_BASE     0x0008A020
#define RX72M_SCI7_BASE     0x000D00E0
#define RX72M_MTU3_BASE     0x000C1200
#define RX72M_S12AD_BASE    0x00089000
#define RX72M_RSPI0_BASE    0x000D0100
#define RX72M_ETHERC_BASE   0x000C0000
#define RX72M_FCU_BASE      0x007FE000
#define RX72M_GPIO_BASE     0x0008C000
#define RX72M_MPC_BASE      0x0008C100
#define RX72M_DMAC_BASE     0x00082000
#define RX72M_DTC_BASE      0x00082400
#define RX72M_WDT_BASE      0x00088020
#define RX72M_RTC_BASE      0x0008C400

#define RX72M_NR_TMR    2
#define RX72M_NR_CMT    2
#define RX72M_NR_SCI    2

struct RX72MState {
    /*< private >*/
    DeviceState parent_obj;
    /*< public >*/

    RXCPU cpu;
    RXICUState icu;
    RTMRState tmr[RX72M_NR_TMR];
    RCMTState cmt[RX72M_NR_CMT];
    RSCIState sci[RX72M_NR_SCI];
    RXMTU3State mtu3;
    RX65NS12ADState s12ad;
    RX65NRSPIState rspi;
    RX65NEthercState etherc;
    RX65NSysClkState sysclk;
    RenesasRxFcuState fcu;
    RenesasRxGpioState gpio;
    RenesasRxDmacState dmac;
    RenesasRxDtcState dtc;
    RenesasRxWdtState wdt;
    RenesasRxRtcState rtc;

    MemoryRegion *sysmem;
    bool kernel;

    MemoryRegion iram;
    MemoryRegion exram;

    /* Populated during realize; board uses this for firmware loading */
    uint32_t cflash_base;

    uint32_t xtal_freq_hz;
    uint32_t pclk_freq_hz;
};

#endif /* HW_RX_RX72M_H */
