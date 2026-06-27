/*
 * Renesas RX Watchdog Timers (WDT and IWDT)
 *
 * Datasheet: RX65N Group, RX651 Group User's Manual: Hardware
 *            (Rev.1.00 R01UH0590EJ0100), sections 28 (WDT) and 29 (IWDT)
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

#ifndef HW_WATCHDOG_RENESAS_RX_WDT_H
#define HW_WATCHDOG_RENESAS_RX_WDT_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_RENESAS_RX_WDT "renesas-rx-wdt"
typedef struct RenesasRxWdtState RenesasRxWdtState;
DECLARE_INSTANCE_CHECKER(RenesasRxWdtState, RENESAS_RX_WDT, TYPE_RENESAS_RX_WDT)

/* One block covering both WDT (0x88020) and IWDT (0x88030). */
#define RX_WDT_REGS_SIZE    0x20

struct RenesasRxWdtState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion mr;

    /* WDT registers. */
    uint16_t wdtcr;
    uint16_t wdtsr;
    uint8_t  wdtrcr;
    uint8_t  wdt_rr_phase;  /* tracks the 0x00 -> 0xFF refresh sequence */

    /* IWDT registers. */
    uint16_t iwdtcr;
    uint16_t iwdtsr;
    uint8_t  iwdtrcr;
    uint8_t  iwdtcstpr;
    uint8_t  iwdt_rr_phase;
};

#endif /* HW_WATCHDOG_RENESAS_RX_WDT_H */
