/*
 * Renesas RX ROM cache control registers
 *
 * Copyright (c) 2024 QEMU Contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_RENESAS_RX_ROMCACHE_H
#define HW_MISC_RENESAS_RX_ROMCACHE_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_RENESAS_RX_ROMCACHE "renesas-rx-romcache"
OBJECT_DECLARE_SIMPLE_TYPE(RenesasRxRomCacheState, RENESAS_RX_ROMCACHE)

/* Covers ROMCE at +0x000 and ROMCIV at +0x004. */
#define RX_ROMCACHE_SIZE    0x10

struct RenesasRxRomCacheState {
    SysBusDevice parent_obj;

    MemoryRegion memory;

    uint16_t romce;     /* ROM cache enable */
};

#endif /* HW_MISC_RENESAS_RX_ROMCACHE_H */
