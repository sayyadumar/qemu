/*
 * Renesas RX Data Transfer Controller (DTC)
 *
 * Datasheet: RX65N Group, RX651 Group User's Manual: Hardware
 *            (Rev.1.00 R01UH0590EJ0100), section 18 (DTC)
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

#ifndef HW_DMA_RENESAS_RX_DTC_H
#define HW_DMA_RENESAS_RX_DTC_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_RENESAS_RX_DTC "renesas-rx-dtc"
typedef struct RenesasRxDtcState RenesasRxDtcState;
DECLARE_INSTANCE_CHECKER(RenesasRxDtcState, RENESAS_RX_DTC, TYPE_RENESAS_RX_DTC)

#define RX_DTC_REGS_SIZE    0x20

struct RenesasRxDtcState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion mr;

    uint8_t  dtccr;     /* control                    */
    uint32_t dtcvbr;    /* vector base address        */
    uint8_t  dtcadmod;  /* address mode               */
    uint8_t  dtcst;     /* module start               */
    uint16_t dtcsts;    /* status                     */
    uint32_t dtcibr;    /* index table base           */
    uint8_t  dtcor;     /* operation                  */
    uint16_t dtcexbr;   /* extended repeat-area base  */
};

#endif /* HW_DMA_RENESAS_RX_DTC_H */
