/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Command Processor (CP) model (impl). See include/cp/cp.h for scope and the
 * exact Dolphin CommandProcessor functions/lines every register, mask and
 * formula is transcribed from. 16-bit registers; 32-bit access is split into
 * halves like vi.c.
 */
#include "cp/cp.h"
#include "debug/rings.h"

#include <stdio.h>
#include <string.h>

void gcn_cp_set_irq(GcnCp* cp, GcnCpIrqFn fn, void* user) {
    cp->irq = fn;
    cp->irq_user = user;
}

void gcn_cp_init(GcnCp* cp) {
    /* CommandProcessorManager::Init:107-126 — status has CommandIdle+ReadIdle
     * (computed on read here), ctrl=0, fifo Init()=all zero. Metrics MMIOs are
     * fixed constants: all 0 except CLKS_PER_VTX_OUT = 4 (.cpp:165-191). */
    memset(cp, 0, sizeof *cp);
    cp->reg[GCN_CP_CLKS_PER_VTX_OUT >> 1] = 4u;
}

/* ---- 32-bit view of a FIFO register (LO half at lo_off, HI half at lo_off+2 —
 *      MMIO::Utils::LowPart/HighPart, CommandProcessor.cpp:142-156). ---- */
static u32 cp_u32(const GcnCp* cp, u32 lo_off) {
    return ((u32)cp->reg[(lo_off + 2u) >> 1] << 16) | cp->reg[lo_off >> 1];
}

static void cp_set_u32(GcnCp* cp, u32 lo_off, u32 v) {
    cp->reg[lo_off >> 1]        = (u16)(v & 0xFFFFu);
    cp->reg[(lo_off + 2u) >> 1] = (u16)(v >> 16);
}

/* ---- CPU-side status + interrupt evaluation (SetCPStatusFromCPU:514-550 +
 *      UpdateInterrupts:409-426, single-core path). Recompute the hi/lo
 *      watermark breach from the live ReadWriteDistance vs the watermark
 *      registers, gate INT_CAUSE_CP exactly as Dolphin does, and drive the
 *      PI line (level-pushed; transition-logged to the event ring). ---- */
static void cp_update_interrupts(GcnCp* cp) {
    u32 rwd    = cp_u32(cp, GCN_CP_FIFO_RW_DIST_LO);
    u32 hi_wm  = cp_u32(cp, GCN_CP_FIFO_HI_WM_LO);
    u32 lo_wm  = cp_u32(cp, GCN_CP_FIFO_LO_WM_LO);
    cp->hi_wm = (rwd > hi_wm) ? 1 : 0;
    cp->lo_wm = (rwd < lo_wm) ? 1 : 0;

    int bp_int   = cp->breakpoint && cp->bp_int;
    int ovf_int  = cp->hi_wm && cp->hi_wm_int;
    int undf_int = cp->lo_wm && cp->lo_wm_int;
    int level = (bp_int || ovf_int || undf_int) && cp->gp_read_enable;

    if (cp->irq)
        cp->irq(cp->irq_user, level);
    if (level != cp->irq_level) {
        gcn_ring_event(level ? GCN_EV_IRQ_RAISE : GCN_EV_IRQ_CLEAR,
                       /*source*/ 11u /* PI cause bit of INT_CAUSE_CP=0x800 */, 0u, 0u);
        cp->irq_level = level;
    }
}

/* SetCpStatusRegister:552-564 — STATUS computed purely from live pointer state.
 * AtBreakpoint (Fifo.cpp:404-411) = bFF_BPEnable && (ReadPointer==Breakpoint). */
static u16 cp_status(const GcnCp* cp) {
    u32 rwd  = cp_u32(cp, GCN_CP_FIFO_RW_DIST_LO);
    u32 rptr = cp_u32(cp, GCN_CP_FIFO_RPTR_LO);
    u32 wptr = cp_u32(cp, GCN_CP_FIFO_WPTR_LO);
    u32 bp   = cp_u32(cp, GCN_CP_FIFO_BP_LO);
    int at_bp     = cp->bp_enable && (rptr == bp);
    int read_idle = (rwd == 0) || (rptr == wptr);
    int cmd_idle  = (rwd == 0) || at_bp || !cp->gp_read_enable;

    u16 hex = 0;
    if (cp->hi_wm)     hex |= GCN_CP_STATUS_OVF_HI_WM;
    if (cp->lo_wm)     hex |= GCN_CP_STATUS_UNF_LO_WM;
    if (read_idle)     hex |= GCN_CP_STATUS_READ_IDLE;
    if (cmd_idle)      hex |= GCN_CP_STATUS_CMD_IDLE;
    if (cp->breakpoint) hex |= GCN_CP_STATUS_BREAKPOINT;
    return hex;
}

/* ---- 16-bit register access ---- */

static int cp_is_fifo_reg(u32 off) {
    return off >= GCN_CP_FIFO_REG_FIRST && off <= GCN_CP_FIFO_REG_LAST;
}

static u16 cp_read16(GcnCp* cp, u32 off) {
    switch (off) {
    case GCN_CP_STATUS:
        return cp_status(cp);           /* computed, never stored */
    case GCN_CP_CLEAR:
        return 0;                        /* MMIO::Constant<u16>(0) (.cpp:210) */
    default:
        /* CTRL read-back, PERF/UNK, FIFO regs (single-core direct read of the
         * stored halves — RW_DISTANCE/READ_POINTER complex readers reduce to
         * this without a GPU thread), and metrics constants. */
        return cp->reg[off >> 1];
    }
}

static void cp_write16(GcnCp* cp, u32 off, u16 value) {
    if (cp_is_fifo_reg(off)) {
        /* FIFO regs: LO halves mask 0xffe0, HI halves mask 0x03ff (GCN). Then
         * recompute status — a pointer/distance/watermark change can breach a
         * watermark (GX_PLAN: "after every ... pointer-register write"). */
        u16 mask = (off & 2u) ? GCN_CP_WMASK_HI : GCN_CP_WMASK_LO;
        cp->reg[off >> 1] = value & mask;
        cp_update_interrupts(cp);
        return;
    }
    switch (off) {
    case GCN_CP_CTRL: {
        /* CommandProcessor.cpp:201-208 store val&0x3F, then SetCpControlRegister
         * :574-595 decodes the enable flags. Recompute the interrupt after
         * (enabling GPReadEnable can newly gate the CP line on/off). */
        u16 v = value & GCN_CP_CTRL_MASK;
        cp->reg[GCN_CP_CTRL >> 1] = v;
        cp->gp_read_enable = (v & GCN_CP_CTRL_GPREAD_ENABLE)  ? 1 : 0;
        cp->bp_enable      = (v & GCN_CP_CTRL_BP_ENABLE)      ? 1 : 0;
        cp->hi_wm_int      = (v & GCN_CP_CTRL_OVF_INT_ENABLE) ? 1 : 0;
        cp->lo_wm_int      = (v & GCN_CP_CTRL_UNF_INT_ENABLE) ? 1 : 0;
        cp->gp_link_enable = (v & GCN_CP_CTRL_GPLINK_ENABLE)  ? 1 : 0;
        cp->bp_int         = (v & GCN_CP_CTRL_BP_INT)         ? 1 : 0;
        cp_update_interrupts(cp);
        return;
    }
    case GCN_CP_CLEAR:
        /* SetCpClearRegister:597-601 — Dolphin intentionally does nothing (GP
         * timing not emulated). No-op. */
        return;
    case GCN_CP_PERF_SELECT:
        cp->reg[off >> 1] = value & 0x0007u;   /* DirectWrite mask (.cpp:221) */
        return;
    case GCN_CP_UNK_0A:
        cp->reg[off >> 1] = value & 0x00FFu;   /* DirectWrite mask (.cpp:225) */
        return;
    default:
        /* STATUS (RO) and metrics constants (InvalidWrite) — ignore. */
        return;
    }
}

u32 gcn_cp_read(void* user, CPUState* cpu, u32 addr, u8 size) {
    GcnCp* cp = (GcnCp*)user;
    u32 off = addr - GCN_CP_BASE;
    (void)cpu;
    switch (size) {
    case 1: {
        u16 v = cp_read16(cp, off & ~1u);
        return (addr & 1u) ? (v & 0xFFu) : (v >> 8);
    }
    case 2:
        return cp_read16(cp, off);
    default: /* 4: HI halfword at off, LO at off+2 */
        return (cp_read16(cp, off) << 16) | cp_read16(cp, off + 2u);
    }
}

void gcn_cp_write(void* user, CPUState* cpu, u32 addr, u32 value, u8 size) {
    GcnCp* cp = (GcnCp*)user;
    u32 off = addr - GCN_CP_BASE;
    (void)cpu;
    switch (size) {
    case 1: {
        u16 v = cp_read16(cp, off & ~1u);
        if (addr & 1u) v = (u16)((v & 0xFF00u) | (value & 0xFFu));
        else           v = (u16)((v & 0x00FFu) | ((value & 0xFFu) << 8));
        cp_write16(cp, off & ~1u, v);
        return;
    }
    case 2:
        cp_write16(cp, off, (u16)value);
        return;
    default: /* 4 */
        cp_write16(cp, off, (u16)(value >> 16));
        cp_write16(cp, off + 2u, (u16)value);
        return;
    }
}

/* ---- gather-pipe burst (GatherPipeBursted:339-407, single-core) ----
 * gp.c has already copied the 32 bytes to guest RAM and advanced the PI write
 * pointer with the END->BASE wrap. Here we mirror Dolphin's CP side: run the
 * CPU-side status evaluation FIRST (with the pre-burst distance, exactly as
 * Dolphin calls SetCPStatusFromCPU at the top), and — iff GPLinkEnable — mirror
 * the (already-advanced) PI write pointer into CPWritePointer and add 32 to
 * CPReadWriteDistance. There is no FIFO consumer, so the read pointer is left
 * alone and the distance climbs (GX_PLAN deferral #2). */
void gcn_cp_gather_pipe_bursted(GcnCp* cp, u32 pi_write_pointer) {
    cp_update_interrupts(cp);
    if (!cp->gp_link_enable)
        return;
    cp_set_u32(cp, GCN_CP_FIFO_WPTR_LO, pi_write_pointer);
    cp_set_u32(cp, GCN_CP_FIFO_RW_DIST_LO,
               cp_u32(cp, GCN_CP_FIFO_RW_DIST_LO) + GCN_CP_GATHER_PIPE_SIZE);
}

/* ---- GPU-side FIFO consumer support (gx.c) ---------------------------------
 * The read side of the FIFO, transcribed from Fifo.cpp RunGpuLoop:326-354. The
 * consumer reads the 32 bytes at the current read pointer, then calls
 * gcn_cp_gpu_consume_chunk to advance it. */

u32 gcn_cp_fifo_read_pointer(const GcnCp* cp) {
    return cp_u32(cp, GCN_CP_FIFO_RPTR_LO);
}

u32 gcn_cp_fifo_rw_distance(const GcnCp* cp) {
    return cp_u32(cp, GCN_CP_FIFO_RW_DIST_LO);
}

/* AtBreakpoint (Fifo.cpp:404-411): bFF_BPEnable && (ReadPointer == Breakpoint). */
int gcn_cp_at_breakpoint(const GcnCp* cp) {
    return cp->bp_enable &&
           (cp_u32(cp, GCN_CP_FIFO_RPTR_LO) == cp_u32(cp, GCN_CP_FIFO_BP_LO));
}

void gcn_cp_gpu_consume_chunk(GcnCp* cp) {
    /* Advance the read pointer, wrapping CPEnd->CPBase (Fifo.cpp:329-332 — the
     * same wrap the write side does in gp.c). */
    u32 rptr = cp_u32(cp, GCN_CP_FIFO_RPTR_LO);
    u32 base = cp_u32(cp, GCN_CP_FIFO_BASE_LO);
    u32 end  = cp_u32(cp, GCN_CP_FIFO_END_LO);
    if (rptr == end)
        rptr = base;
    else
        rptr += GCN_CP_GATHER_PIPE_SIZE;
    cp_set_u32(cp, GCN_CP_FIFO_RPTR_LO, rptr);

    /* Subtract 32 from the distance (Fifo.cpp:347). Dolphin ASSERTs on a
     * negative distance; we saturate at 0 and note it once rather than corrupt
     * the value — a negative distance would only arise from a consumer/producer
     * accounting bug, which would then diverge loudly in the oracle diff. */
    u32 rwd = cp_u32(cp, GCN_CP_FIFO_RW_DIST_LO);
    if (rwd < GCN_CP_GATHER_PIPE_SIZE) {
        static int warned = 0;
        if (!warned) {
            fprintf(stderr, "gcn cp: FIFO read-write distance underflow "
                            "(rwd=%u < 32) — saturating at 0\n", rwd);
            warned = 1;
        }
        rwd = 0;
    } else {
        rwd -= GCN_CP_GATHER_PIPE_SIZE;
    }
    cp_set_u32(cp, GCN_CP_FIFO_RW_DIST_LO, rwd);

    /* SetCPStatusFromGPU (Fifo.cpp:354): re-run the watermark/interrupt eval now
     * that the distance changed. STATUS itself is computed on read. */
    cp_update_interrupts(cp);
}
