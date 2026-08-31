/*
 * Renesas RX Flash Control Unit (FCU) with FACI command interface
 *
 * Datasheet: RX65N Group, RX651 Group User's Manual: Hardware
 *            (Rev.1.00 R01UH0590EJ0100), section 6 (Flash Memory)
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
 *
 * The RX65N programs and erases its on-chip code and data flash through the
 * Flash Application Command Interface (FACI). Firmware first enters P/E mode
 * for the target array via FENTRYR, then issues command sequences by writing
 * command bytes and data words to the destination address inside the flash
 * array itself, polling FSTATR.FRDY for completion.
 *
 * Both flash arrays are modelled as ROM-device memory regions: ordinary reads
 * and instruction fetches hit the backing RAM directly (fast, and firmware is
 * still loaded by the QEMU ROM loader, which bypasses these write callbacks),
 * while guest writes are routed here and interpreted as FACI commands. QEMU
 * models no real program/erase timing, so every command completes
 * synchronously and FRDY always reads ready.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "hw/misc/renesas_rx_fcu.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "system/block-backend.h"
#include "hw/block/block.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "migration/vmstate.h"
#include "qapi/error.h"

/* FACI control register offsets from the register block base (0x007FE000). */
#define R_FASTAT    0x010   /* Flash Access Status            (8-bit)  */
#define R_FAEINT    0x014   /* Flash Access Err Int Enable    (8-bit)  */
#define R_FRDYIE    0x018   /* Flash Ready Int Enable         (8-bit)  */
#define R_FSADDR    0x030   /* Flash Processing Start Address (32-bit) */
#define R_FEADDR    0x034   /* Flash Processing End Address    (32-bit) */
#define R_FSTATR    0x080   /* Flash Status                   (32-bit) */
#define R_FENTRYR   0x084   /* Flash P/E Mode Entry           (16-bit) */
#define R_FPROTR    0x088   /* Flash Protection               (16-bit) */
#define R_FSUACR    0x08C   /* Startup Area Control           (16-bit) */
#define R_FCMDR     0x0A0   /* FACI Command                   (16-bit) */
#define R_FPESTAT   0x0C0   /* P/E Error Status               (16-bit) */
#define R_FBCCNT    0x0D0   /* Blank Check Control            (16-bit) */
#define R_FBCSTAT   0x0D4   /* Blank Check Status             (8-bit)  */
#define R_FPSADDR   0x0D8   /* Programmed/Erased Start Addr   (32-bit) */
#define R_FCPSR     0x0E0   /* Clear/Processing Switch        (16-bit) */
#define R_FPCKAR    0x0E4   /* Processing Clock Notification  (16-bit) */

/* FSTATR bit definitions. */
#define FSTATR_FRDY     (1u << 6)   /* Flash ready                     */
#define FSTATR_PRGERR   (1u << 13)  /* Programming error               */
#define FSTATR_ERSERR   (1u << 14)  /* Erasure error                   */
#define FSTATR_ILGLERR  (1u << 15)  /* Illegal command error           */
#define FSTATR_OTERR    (1u << 16)  /* Other error                     */
#define FSTATR_ERRORS   (FSTATR_PRGERR | FSTATR_ERSERR | \
                         FSTATR_ILGLERR | FSTATR_OTERR)

/* FASTAT bit definitions. */
#define FASTAT_CMDLK    (1u << 4)   /* Command lock                    */

/* FENTRYR: key code in the upper byte, target bits in the lower byte. */
#define FENTRYR_KEY     0xAA00
#define FENTRYR_CODE    0x0001      /* code flash P/E mode             */
#define FENTRYR_DATA    0x0080      /* data flash P/E mode             */

/* FBCSTAT: BCST = 0 means the checked range is blank (all 0xFF). */
#define FBCSTAT_BCST    (1u << 0)

/* FACI command codes. */
#define FACI_CMD_PROGRAM     0xE8
#define FACI_CMD_BLOCK_ERASE 0x20
#define FACI_CMD_BLANK_CHECK 0x71
#define FACI_CMD_CLEAR_STAT  0x50
#define FACI_CMD_FORCED_STOP 0xB3
#define FACI_CMD_CONFIRM     0xD0

/* Erase block granularity (HW manual section 6). */
#define CFLASH_ERASE_BLOCK   0x8000     /* 32 KiB code flash block     */
#define DFLASH_ERASE_BLOCK   0x40       /* 64-byte data flash block    */

static bool target_in_pe_mode(RenesasRxFcuState *s, RxFcuTarget t)
{
    return t == RX_FCU_CFLASH ? (s->fentryr & FENTRYR_CODE)
                              : (s->fentryr & FENTRYR_DATA);
}

static uint8_t *target_storage(RenesasRxFcuState *s, RxFcuTarget t)
{
    return t == RX_FCU_CFLASH ? s->cflash_ptr : s->dflash_ptr;
}

static uint32_t target_size(RenesasRxFcuState *s, RxFcuTarget t)
{
    return t == RX_FCU_CFLASH ? s->cflash_size : s->dflash_size;
}

/* Translate an absolute CPU flash address to an array-relative offset. */
static bool addr_to_offset(RenesasRxFcuState *s, RxFcuTarget t,
                           uint32_t addr, uint32_t *off)
{
    uint32_t base = t == RX_FCU_CFLASH ? s->cflash_base : s->dflash_base;

    if (addr < base || addr - base >= target_size(s, t)) {
        return false;
    }
    *off = addr - base;
    return true;
}

/*
 * FCMDR pairs the command just received (CMDR, low byte) with the one before
 * it (PCMDR, high byte). A two-byte sequence such as block erase therefore
 * ends up reading 0x20d0: 0x20 shifted up by the 0xd0 confirm. The confirm
 * that terminates a programming sequence is not recorded, which is why the
 * manual lists programming as leaving CMDR at 0xe8.
 */
static void faci_set_cmdr(RenesasRxFcuState *s, uint8_t cmd)
{
    s->fcmdr = ((s->fcmdr & 0xff) << 8) | cmd;
}

/* Finish a command: report ready and pulse the ready interrupt if enabled. */
static void faci_complete(RenesasRxFcuState *s)
{
    s->cmd_state = RX_FCU_ST_READY;
    s->fstatr |= FSTATR_FRDY;
    if (s->frdyie & 1) {
        qemu_irq_pulse(s->frdyi);
    }
}

/* Flag an illegal command: lock the sequencer and raise the error interrupt. */
static void faci_illegal(RenesasRxFcuState *s)
{
    s->cmd_state = RX_FCU_ST_READY;
    s->fstatr |= FSTATR_ILGLERR | FSTATR_FRDY;
    s->fastat |= FASTAT_CMDLK;
    if (s->faeint) {
        qemu_set_irq(s->fiferr, 1);
    }
}

/*
 * Write a modified range of an array back to its block backend, if one is
 * attached. Offsets are widened to sector boundaries the way pflash does.
 */
static void fcu_flash_sync(RenesasRxFcuState *s, RxFcuTarget t,
                           uint32_t off, uint32_t len)
{
    BlockBackend *blk = t == RX_FCU_CFLASH ? s->cflash_blk : s->dflash_blk;
    bool ro = t == RX_FCU_CFLASH ? s->cflash_ro : s->dflash_ro;
    uint32_t size = target_size(s, t);
    uint64_t start, end;
    int ret;

    if (!blk || ro) {
        return;
    }
    start = QEMU_ALIGN_DOWN(off, BDRV_SECTOR_SIZE);
    end = QEMU_ALIGN_UP((uint64_t)off + len, BDRV_SECTOR_SIZE);
    if (end > size) {
        end = size;
    }
    if (start >= end) {
        return;
    }
    ret = blk_pwrite(blk, start, end - start,
                     target_storage(s, t) + start, 0);
    if (ret < 0) {
        error_report("renesas-rx-fcu: could not write flash image: %s",
                     strerror(-ret));
    }
}

static void faci_block_erase(RenesasRxFcuState *s, RxFcuTarget t, uint32_t off)
{
    uint32_t block = t == RX_FCU_CFLASH ? CFLASH_ERASE_BLOCK
                                        : DFLASH_ERASE_BLOCK;
    uint32_t start = off & ~(block - 1);
    uint32_t size = target_size(s, t);

    if (start >= size) {
        faci_illegal(s);
        return;
    }
    if (start + block > size) {
        block = size - start;
    }
    memset(target_storage(s, t) + start, 0xff, block);
    fcu_flash_sync(s, t, start, block);
    s->fpsaddr = (t == RX_FCU_CFLASH ? s->cflash_base : s->dflash_base) + start;
    faci_complete(s);
}

static void faci_blank_check(RenesasRxFcuState *s, RxFcuTarget t)
{
    uint32_t start, end;
    const uint8_t *p;

    if (!addr_to_offset(s, t, s->fsaddr, &start) ||
        !addr_to_offset(s, t, s->feaddr, &end) || end < start) {
        faci_illegal(s);
        return;
    }

    p = target_storage(s, t);
    s->fbcstat = 0;     /* assume blank */
    for (uint32_t i = start; i <= end; i++) {
        if (p[i] != 0xff) {
            s->fbcstat = FBCSTAT_BCST;  /* not blank */
            break;
        }
    }
    faci_complete(s);
}

/*
 * Interpret a guest write to a flash array address while that array is in P/E
 * mode. Implements the FACI program / block-erase / blank-check sequences.
 */
static void faci_command(RenesasRxFcuState *s, RxFcuTarget t,
                         uint32_t off, uint64_t value, unsigned size)
{
    uint8_t cmd = value & 0xff;

    switch (s->cmd_state) {
    case RX_FCU_ST_READY:
        faci_set_cmdr(s, cmd);
        switch (cmd) {
        case FACI_CMD_PROGRAM:
            s->cmd_target = t;
            s->prog_off = off;
            s->fsaddr = (t == RX_FCU_CFLASH ? s->cflash_base : s->dflash_base)
                        + off;
            s->cmd_state = RX_FCU_ST_PROGRAM_COUNT;
            break;
        case FACI_CMD_BLOCK_ERASE:
            s->cmd_target = t;
            s->prog_off = off;
            s->cmd_state = RX_FCU_ST_ERASE;
            break;
        case FACI_CMD_BLANK_CHECK:
            s->cmd_target = t;
            s->cmd_state = RX_FCU_ST_BLANKCHECK;
            break;
        case FACI_CMD_CLEAR_STAT:
            s->fstatr = (s->fstatr & ~FSTATR_ERRORS) | FSTATR_FRDY;
            s->fastat &= ~FASTAT_CMDLK;
            qemu_set_irq(s->fiferr, 0);
            break;
        case FACI_CMD_FORCED_STOP:
            s->fstatr = (s->fstatr & ~FSTATR_ERRORS) | FSTATR_FRDY;
            s->fastat &= ~FASTAT_CMDLK;
            s->cmd_state = RX_FCU_ST_READY;
            qemu_set_irq(s->fiferr, 0);
            break;
        default:
            faci_illegal(s);
            break;
        }
        break;

    case RX_FCU_ST_PROGRAM_COUNT:
        /* Number of 16-bit data words that follow. */
        s->prog_words = value & 0xff;
        s->cmd_state = RX_FCU_ST_PROGRAM_DATA;
        break;

    case RX_FCU_ST_PROGRAM_DATA:
        if (s->prog_words > 0) {
            uint8_t *p = target_storage(s, s->cmd_target);
            uint32_t sz = target_size(s, s->cmd_target);
            if (s->prog_off + 2 <= sz) {
                /* Flash programming can only clear bits; emulate write as AND. */
                uint16_t cur = lduw_le_p(p + s->prog_off);
                stw_le_p(p + s->prog_off, cur & (uint16_t)value);
                fcu_flash_sync(s, s->cmd_target, s->prog_off, 2);
            }
            s->prog_off += 2;
            s->prog_words--;
        } else if (cmd == FACI_CMD_CONFIRM) {
            faci_complete(s);
        } else {
            faci_illegal(s);
        }
        break;

    case RX_FCU_ST_ERASE:
        if (cmd == FACI_CMD_CONFIRM) {
            faci_set_cmdr(s, cmd);
            faci_block_erase(s, s->cmd_target, s->prog_off);
        } else {
            faci_illegal(s);
        }
        break;

    case RX_FCU_ST_BLANKCHECK:
        if (cmd == FACI_CMD_CONFIRM) {
            faci_set_cmdr(s, cmd);
            faci_blank_check(s, s->cmd_target);
        } else {
            faci_illegal(s);
        }
        break;
    }
}

static uint64_t faci_flash_read(void *opaque, hwaddr offset, unsigned size)
{
    RxFcuFlash *f = opaque;
    const uint8_t *p = target_storage(f->fcu, f->target);

    /*
     * This callback is only reached while the array is in P/E mode (romd is
     * disabled); outside P/E mode reads hit the backing RAM directly. Return
     * the stored contents so reads remain coherent in either mode.
     */
    switch (size) {
    case 1:
        return p[offset];
    case 2:
        return lduw_le_p(p + offset);
    default:
        return ldl_le_p(p + offset);
    }
}

static void faci_flash_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    RxFcuFlash *f = opaque;
    RenesasRxFcuState *s = f->fcu;

    if (!target_in_pe_mode(s, f->target)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "renesas-rx-fcu: write to %s flash @0x%" HWADDR_PRIx
                      " while not in P/E mode\n",
                      f->target == RX_FCU_CFLASH ? "code" : "data", offset);
        return;
    }
    faci_command(s, f->target, offset, value, size);
}

static const MemoryRegionOps faci_flash_ops = {
    .read = faci_flash_read,
    .write = faci_flash_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* Update P/E mode entry and toggle the rom_device read path accordingly. */
static void fcu_set_fentryr(RenesasRxFcuState *s, uint16_t value)
{
    bool was_code = s->fentryr & FENTRYR_CODE;
    bool was_data = s->fentryr & FENTRYR_DATA;
    bool code, data;

    if ((value & 0xff00) != FENTRYR_KEY) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "renesas-rx-fcu: FENTRYR write without key (0x%04x)\n",
                      value);
        return;
    }

    code = value & FENTRYR_CODE;
    data = value & FENTRYR_DATA;
    s->fentryr = (code ? FENTRYR_CODE : 0) | (data ? FENTRYR_DATA : 0);

    if (code != was_code) {
        memory_region_rom_device_set_romd(&s->cflash_mr, !code);
    }
    if (data != was_data) {
        memory_region_rom_device_set_romd(&s->dflash_mr, !data);
    }
    if (!code && !data) {
        /* Leaving P/E mode resets the command sequencer. */
        s->cmd_state = RX_FCU_ST_READY;
    }
}

static uint64_t fcu_regs_read(void *opaque, hwaddr offset, unsigned size)
{
    RenesasRxFcuState *s = opaque;
    uint64_t val;

    switch (offset) {
    case R_FASTAT:
        val = s->fastat;
        break;
    case R_FAEINT:
        val = s->faeint;
        break;
    case R_FRDYIE:
        val = s->frdyie;
        break;
    case R_FSADDR:
        val = s->fsaddr;
        break;
    case R_FEADDR:
        val = s->feaddr;
        break;
    case R_FSTATR:
        val = s->fstatr;
        break;
    case R_FENTRYR:
        val = s->fentryr;
        break;
    case R_FPROTR:
        val = s->fprotr;
        break;
    case R_FSUACR:
        val = s->fsuacr;
        break;
    case R_FCMDR:
        val = s->fcmdr;
        break;
    case R_FPESTAT:
        val = s->fpestat;
        break;
    case R_FBCCNT:
        val = s->fbccnt;
        break;
    case R_FBCSTAT:
        val = s->fbcstat;
        break;
    case R_FPSADDR:
        val = s->fpsaddr;
        break;
    case R_FCPSR:
        val = s->fcpsr;
        break;
    case R_FPCKAR:
        val = s->fpckar;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "renesas-rx-fcu: read from unimplemented reg 0x%"
                      HWADDR_PRIx "\n", offset);
        return 0;
    }

    if (size < 4) {
        val &= (1ull << (8 * size)) - 1;
    }
    return val;
}

static void fcu_regs_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    RenesasRxFcuState *s = opaque;

    switch (offset) {
    case R_FASTAT:
        s->fastat = value;
        break;
    case R_FAEINT:
        s->faeint = value;
        break;
    case R_FRDYIE:
        s->frdyie = value;
        break;
    case R_FSADDR:
        s->fsaddr = value;
        break;
    case R_FEADDR:
        s->feaddr = value;
        break;
    case R_FENTRYR:
        fcu_set_fentryr(s, value);
        break;
    case R_FPROTR:
        s->fprotr = value;
        break;
    case R_FSUACR:
        s->fsuacr = value;
        break;
    case R_FBCCNT:
        s->fbccnt = value;
        break;
    case R_FCPSR:
        s->fcpsr = value;
        break;
    case R_FPCKAR:
        s->fpckar = value;
        break;
    case R_FSTATR:
    case R_FBCSTAT:
    case R_FCMDR:
    case R_FPESTAT:
    case R_FPSADDR:
        /* Read-only status registers; ignore writes. */
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "renesas-rx-fcu: write to unimplemented reg 0x%"
                      HWADDR_PRIx " = 0x%" PRIx64 "\n", offset, value);
        break;
    }
}

static const MemoryRegionOps fcu_regs_ops = {
    .read = fcu_regs_read,
    .write = fcu_regs_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/*
 * Latch the code flash bank layout from the option-setting memory. The
 * hardware samples MDE.BANKMD and BANKSEL.BANKSWP at reset, so a mode or
 * bank switch only takes effect on the next reset, which is what makes the
 * "program the other bank then reboot into it" update flow work.
 *
 * BANKSWP = 111b maps bank 0, the one holding the vectors, into the upper
 * half, which is the layout a linearly programmed image already has, so it
 * needs no aliasing. 000b swaps the halves.
 */
void rx_fcu_update_bank_map(RenesasRxFcuState *s)
{
    uint32_t mde, banksel;
    bool dual, swapped;

    mde = ldl_le_p(s->ofsm_ptr + RX_OFSM_MDE);
    banksel = ldl_le_p(s->ofsm_ptr + RX_OFSM_BANKSEL);

    dual = (mde & RX_BANKMD_MASK) == RX_BANKMD_DUAL;
    swapped = (banksel & RX_BANKSWP_MASK) == RX_BANKSWP_SWAPPED;

    /*
     * Dual mode needs a code flash of at least 1.5 MB. Smaller parts do not
     * offer it, and the 1.5 MB bank bases are not simply the halves of the
     * linear range, so only the 2 MB layout is handled here.
     */
    if (dual && s->cflash_size < 2 * MiB) {
        qemu_log_mask(LOG_UNIMP,
                      "renesas-rx-fcu: dual mode requested on a %u KiB code "
                      "flash is not modelled; staying linear\n",
                      s->cflash_size / 1024);
        dual = false;
    }

    s->dual_mode = dual;
    s->bank_swapped = dual && swapped;

    memory_region_transaction_begin();
    memory_region_set_enabled(&s->cflash_linear, !s->bank_swapped);
    memory_region_set_enabled(&s->cflash_bank[0], s->bank_swapped);
    memory_region_set_enabled(&s->cflash_bank[1], s->bank_swapped);
    if (s->bank_swapped) {
        /* Lower half of the map shows bank 0, upper half shows bank 1. */
        memory_region_set_alias_offset(&s->cflash_bank[0], s->cflash_size / 2);
        memory_region_set_alias_offset(&s->cflash_bank[1], 0);
    }
    memory_region_transaction_commit();
}

static void rx_fcu_reset(DeviceState *dev)
{
    RenesasRxFcuState *s = RENESAS_RX_FCU(dev);

    /* The bank layout is sampled from the OFSM at every reset. */
    rx_fcu_update_bank_map(s);

    /* Exit P/E mode and restore direct (romd) flash reads. */
    memory_region_rom_device_set_romd(&s->cflash_mr, true);
    memory_region_rom_device_set_romd(&s->dflash_mr, true);

    s->fentryr = 0;
    s->fstatr = FSTATR_FRDY;
    s->fastat = 0;
    s->frdyie = 0;
    s->faeint = 0;
    s->fsaddr = 0;
    s->feaddr = 0;
    s->fpsaddr = 0;
    s->fbcstat = 0;
    s->fbccnt = 0;
    s->fcpsr = 0;
    s->fpckar = 0;
    s->fprotr = 0;
    s->fsuacr = 0;
    s->fpestat = 0;
    s->fcmdr = 0;
    s->cmd_state = RX_FCU_ST_READY;
    s->prog_off = 0;
    s->prog_words = 0;

    qemu_set_irq(s->frdyi, 0);
    qemu_set_irq(s->fiferr, 0);
}

/*
 * Attach one array to its block backend: take the write permission if the
 * image allows it, then load the image over the erased contents. A backend
 * whose size does not match the array is rejected rather than silently
 * truncated.
 */
static bool fcu_attach_blk(RenesasRxFcuState *s, DeviceState *dev,
                           RxFcuTarget t, Error **errp)
{
    BlockBackend *blk = t == RX_FCU_CFLASH ? s->cflash_blk : s->dflash_blk;
    MemoryRegion *mr = t == RX_FCU_CFLASH ? &s->cflash_mr : &s->dflash_mr;
    uint32_t size = target_size(s, t);
    uint64_t perm;
    bool ro;

    if (!blk) {
        return true;
    }

    ro = !blk_supports_write_perm(blk);
    if (t == RX_FCU_CFLASH) {
        s->cflash_ro = ro;
    } else {
        s->dflash_ro = ro;
    }

    perm = BLK_PERM_CONSISTENT_READ | (ro ? 0 : BLK_PERM_WRITE);
    if (blk_set_perm(blk, perm, BLK_PERM_ALL, errp) < 0) {
        return false;
    }

    if (!blk_check_size_and_read_all(blk, dev, target_storage(s, t),
                                     size, errp)) {
        vmstate_unregister_ram(mr, dev);
        return false;
    }
    return true;
}

static void rx_fcu_realize(DeviceState *dev, Error **errp)
{
    RenesasRxFcuState *s = RENESAS_RX_FCU(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    if (s->cflash_size == 0 || s->dflash_size == 0) {
        error_setg(errp, "code-flash-size and data-flash-size must be set");
        return;
    }

    s->cflash_ctx.fcu = s;
    s->cflash_ctx.target = RX_FCU_CFLASH;
    s->dflash_ctx.fcu = s;
    s->dflash_ctx.target = RX_FCU_DFLASH;

    memory_region_init_io(&s->regs_mr, OBJECT(s), &fcu_regs_ops, s,
                          "renesas-rx-fcu.regs", RX_FCU_REGS_SIZE);

    if (!memory_region_init_rom_device(&s->cflash_mr, OBJECT(s),
                                       &faci_flash_ops, &s->cflash_ctx,
                                       "renesas-rx-fcu.cflash",
                                       s->cflash_size, errp)) {
        return;
    }
    if (!memory_region_init_rom_device(&s->dflash_mr, OBJECT(s),
                                       &faci_flash_ops, &s->dflash_ctx,
                                       "renesas-rx-fcu.dflash",
                                       s->dflash_size, errp)) {
        return;
    }
    /*
     * The option-setting memory is read-only to ordinary accesses; it is
     * only changed by the FACI configuration-set command, which is not
     * modelled, so a plain ROM is the right shape for it.
     */
    if (!memory_region_init_rom(&s->ofsm_mr, OBJECT(s),
                                "renesas-rx-fcu.ofsm",
                                RX_FCU_OFSM_SIZE, errp)) {
        return;
    }

    /*
     * The code flash reaches the bus through a container holding either one
     * alias over the whole array (linear mode) or one per bank (dual mode).
     */
    memory_region_init(&s->cflash_container, OBJECT(s),
                       "renesas-rx-fcu.cflash-map", s->cflash_size);
    memory_region_init_alias(&s->cflash_linear, OBJECT(s),
                             "renesas-rx-fcu.cflash-linear",
                             &s->cflash_mr, 0, s->cflash_size);
    memory_region_add_subregion(&s->cflash_container, 0, &s->cflash_linear);

    for (int i = 0; i < 2; i++) {
        g_autofree char *name =
            g_strdup_printf("renesas-rx-fcu.cflash-bank%d", i);
        memory_region_init_alias(&s->cflash_bank[i], OBJECT(s), name,
                                 &s->cflash_mr, 0, s->cflash_size / 2);
        memory_region_add_subregion(&s->cflash_container,
                                    (hwaddr)i * (s->cflash_size / 2),
                                    &s->cflash_bank[i]);
        memory_region_set_enabled(&s->cflash_bank[i], false);
    }

    s->cflash_ptr = memory_region_get_ram_ptr(&s->cflash_mr);
    s->dflash_ptr = memory_region_get_ram_ptr(&s->dflash_mr);
    s->ofsm_ptr = memory_region_get_ram_ptr(&s->ofsm_mr);

    /*
     * Erased flash reads as all ones, but the backing memory starts zeroed.
     * Fill both arrays here rather than at reset: realize runs before the
     * ROM loader writes the firmware image into the code flash, and flash
     * is non-volatile, so anything the guest programmed must survive a
     * later system reset.
     */
    memset(s->cflash_ptr, 0xff, s->cflash_size);
    memset(s->dflash_ptr, 0xff, s->dflash_size);
    memset(s->ofsm_ptr, 0xff, RX_FCU_OFSM_SIZE);

    if (!fcu_attach_blk(s, dev, RX_FCU_CFLASH, errp) ||
        !fcu_attach_blk(s, dev, RX_FCU_DFLASH, errp)) {
        return;
    }
    if (s->ofsm_blk) {
        uint64_t perm;
        int64_t len;

        s->ofsm_ro = !blk_supports_write_perm(s->ofsm_blk);
        perm = BLK_PERM_CONSISTENT_READ | (s->ofsm_ro ? 0 : BLK_PERM_WRITE);
        if (blk_set_perm(s->ofsm_blk, perm, BLK_PERM_ALL, errp) < 0) {
            return;
        }
        /*
         * The OFSM is smaller than a block sector, so an exact size match is
         * not something an image file can offer. Require enough bytes and
         * read only the ones the region holds.
         */
        len = blk_getlength(s->ofsm_blk);
        if (len < RX_FCU_OFSM_SIZE) {
            error_setg(errp, "ofsm-drive is %" PRId64 " bytes, need at least %d",
                       len, RX_FCU_OFSM_SIZE);
            return;
        }
        if (blk_pread(s->ofsm_blk, 0, RX_FCU_OFSM_SIZE, s->ofsm_ptr, 0) < 0) {
            error_setg(errp, "could not read ofsm-drive");
            return;
        }
    }

    sysbus_init_mmio(sbd, &s->regs_mr);
    sysbus_init_mmio(sbd, &s->cflash_container);
    sysbus_init_mmio(sbd, &s->dflash_mr);
    sysbus_init_mmio(sbd, &s->ofsm_mr);
    sysbus_init_irq(sbd, &s->frdyi);
    sysbus_init_irq(sbd, &s->fiferr);
}

static const Property rx_fcu_properties[] = {
    DEFINE_PROP_DRIVE("code-flash-drive", RenesasRxFcuState, cflash_blk),
    DEFINE_PROP_DRIVE("data-flash-drive", RenesasRxFcuState, dflash_blk),
    DEFINE_PROP_DRIVE("ofsm-drive", RenesasRxFcuState, ofsm_blk),
    DEFINE_PROP_UINT32("code-flash-size", RenesasRxFcuState, cflash_size, 0),
    DEFINE_PROP_UINT32("data-flash-size", RenesasRxFcuState, dflash_size, 0),
    DEFINE_PROP_UINT32("code-flash-base", RenesasRxFcuState, cflash_base, 0),
    DEFINE_PROP_UINT32("data-flash-base", RenesasRxFcuState, dflash_base, 0),
};

static const VMStateDescription vmstate_rx_fcu = {
    .name = "renesas-rx-fcu",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(fentryr, RenesasRxFcuState),
        VMSTATE_UINT32(fstatr, RenesasRxFcuState),
        VMSTATE_UINT8(fastat, RenesasRxFcuState),
        VMSTATE_UINT8(frdyie, RenesasRxFcuState),
        VMSTATE_UINT8(faeint, RenesasRxFcuState),
        VMSTATE_UINT32(fsaddr, RenesasRxFcuState),
        VMSTATE_UINT32(fpsaddr, RenesasRxFcuState),
        VMSTATE_UINT32(feaddr, RenesasRxFcuState),
        VMSTATE_UINT8(fbcstat, RenesasRxFcuState),
        VMSTATE_UINT16(fbccnt, RenesasRxFcuState),
        VMSTATE_UINT16(fcpsr, RenesasRxFcuState),
        VMSTATE_UINT16(fpckar, RenesasRxFcuState),
        VMSTATE_UINT16(fprotr, RenesasRxFcuState),
        VMSTATE_UINT16(fsuacr, RenesasRxFcuState),
        VMSTATE_UINT16(fpestat, RenesasRxFcuState),
        VMSTATE_UINT16(fcmdr, RenesasRxFcuState),
        VMSTATE_INT32(cmd_state, RenesasRxFcuState),
        VMSTATE_INT32(cmd_target, RenesasRxFcuState),
        VMSTATE_UINT32(prog_off, RenesasRxFcuState),
        VMSTATE_UINT32(prog_words, RenesasRxFcuState),
        VMSTATE_END_OF_LIST()
    }
};

static void rx_fcu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rx_fcu_realize;
    dc->vmsd = &vmstate_rx_fcu;
    device_class_set_props(dc, rx_fcu_properties);
    device_class_set_legacy_reset(dc, rx_fcu_reset);
}

static const TypeInfo rx_fcu_info = {
    .name = TYPE_RENESAS_RX_FCU,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RenesasRxFcuState),
    .class_init = rx_fcu_class_init,
};

static void rx_fcu_register_types(void)
{
    type_register_static(&rx_fcu_info);
}

type_init(rx_fcu_register_types)
