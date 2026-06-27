/*
 * Renesas RX DMA Controller (DMAC) and Data Transfer Controller (DTC)
 *
 * Datasheet: RX65N Group, RX651 Group User's Manual: Hardware
 *            (Rev.1.00 R01UH0590EJ0100), sections 17 (DMAC) and 18 (DTC)
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

#ifndef HW_DMA_RENESAS_RX_DMAC_H
#define HW_DMA_RENESAS_RX_DMAC_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_RENESAS_RX_DMAC "renesas-rx-dmac"
typedef struct RenesasRxDmacState RenesasRxDmacState;
DECLARE_INSTANCE_CHECKER(RenesasRxDmacState, RENESAS_RX_DMAC, TYPE_RENESAS_RX_DMAC)

#define RX_DMAC_NR_CH       8
#define RX_DMAC_CH_SIZE     0x40        /* per-channel register spacing  */
#define RX_DMAC_REGS_SIZE   0x300       /* channels + common registers   */

typedef struct RxDmacChannel {
    uint32_t dmsar;     /* source address                       */
    uint32_t dmdar;     /* destination address                  */
    uint32_t dmcra;     /* transfer count (A: lower, B: upper)  */
    uint16_t dmcrb;     /* block transfer count                 */
    uint16_t dmtmd;     /* transfer mode                        */
    uint8_t  dmint;     /* interrupt setting                    */
    uint16_t dmamd;     /* address mode                         */
    uint32_t dmofr;     /* offset                               */
    uint8_t  dmcnt;     /* transfer enable                      */
    uint8_t  dmreq;     /* software/refresh request             */
    uint8_t  dmsts;     /* status                               */
} RxDmacChannel;

struct RenesasRxDmacState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion mr;
    AddressSpace *as;
    MemoryRegion *dma_mr;

    RxDmacChannel ch[RX_DMAC_NR_CH];
    uint8_t dmast;      /* module activation                    */

    qemu_irq irq[RX_DMAC_NR_CH];
};

#endif /* HW_DMA_RENESAS_RX_DMAC_H */
