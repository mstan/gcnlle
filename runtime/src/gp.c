/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Gather Pipe (GP) model (impl). See include/gp/gp.h for scope and the exact
 * Dolphin GPFifo/CommandProcessor functions/lines every step is transcribed
 * from. Reads are meaningless (the gather pipe is write-only); writes accumulate
 * and burst.
 */
#include "gp/gp.h"
#include "gx/gx.h"         /* gcn_gx_pipeline_drain — fifo-reset join (G3) */
#include "debug/rings.h"

#include <stdio.h>
#include <string.h>

/* One GP like the one VI/DI; registered at init so the PI_FIFO_RESET hook (a
 * void(void) function pointer set from boot.c) can reach it without pi.c taking
 * a dependency on gp.h. */
static GcnGp* s_gp = NULL;

void gcn_gp_init(GcnGp* gp, GcnPi* pi, GcnCp* cp) {
    memset(gp, 0, sizeof *gp);
    gp->pi = pi;
    gp->cp = cp;
    s_gp = gp;
}

void gcn_gp_reset(void) {
    /* ProcessorInterface.cpp:118-119 / GPFifo::ResetGatherPipe:80-83 — drop any
     * partial staged bytes. (The CP-register ResetFifo Dolphin also runs on
     * PI_FIFO_RESET is deferred: the IPL boot path does not depend on it, and
     * coupling it here would diverge loudly rather than silently — GX_PLAN.)
     *
     * G3: a fifo reset with pipelined-but-undecoded bytes queued would let
     * pre-reset commands execute after the reset the synchronous design
     * decoded them before — join first (no-op with the pipeline off). */
    gcn_gx_pipeline_drain();
    if (s_gp)
        s_gp->count = 0;
}

/* Flush one full 32-byte block: copy to guest RAM at the PI write pointer,
 * record it, advance the PI write pointer (wrap END->BASE), then run the CP
 * gather-pipe-burst handler. */
static void gp_flush_burst(GcnGp* gp, CPUState* cpu) {
    GcnPi* pi = gp->pi;
    u32 wptr = pi->reg[GCN_PI_FIFO_WPTR >> 2];
    u32 base = pi->reg[GCN_PI_FIFO_BASE >> 2];
    u32 end  = pi->reg[GCN_PI_FIFO_END  >> 2];

    /* Copy 32 bytes into guest RAM at the physical write pointer (GPFifo.cpp:96
     * memory.CopyToEmu). Bounds-checked; the pointer is guest-physical. */
    u32 phys = wptr & 0x1FFFFFFFu;
    if (cpu && cpu->ram && (u64)phys + GCN_GP_GATHER_SIZE <= (u64)cpu->ram_size) {
        memcpy(cpu->ram + phys, gp->stage, GCN_GP_GATHER_SIZE);
    } else {
        static int warned = 0;
        if (!warned) {
            fprintf(stderr, "gcn gp: FIFO write pointer 0x%08X out of MEM1 range — "
                            "burst not copied (FIFO not set up?)\n", wptr);
            warned = 1;
        }
    }

    /* Always-on FIFO recorder ring (ROADMAP M2 packet inventory). */
    gcn_ring_fifo(cpu ? cpu->pc : 0u, wptr, gp->stage);

    /* Advance the PI write pointer: wrap to base when it equals end, else += 32
     * (GPFifo.cpp:101-104). */
    if (wptr == end)
        wptr = base;
    else
        wptr += GCN_GP_GATHER_SIZE;
    pi->reg[GCN_PI_FIFO_WPTR >> 2] = wptr;

    /* CP side: status/interrupt eval + (when linked) mirror wptr + distance. */
    gcn_cp_gather_pipe_bursted(gp->cp, wptr);
}

u32 gcn_gp_read(void* user, CPUState* cpu, u32 addr, u8 size) {
    /* The gather pipe is write-only; a read has no defined value. */
    (void)user; (void)cpu; (void)addr; (void)size;
    return 0;
}

void gcn_gp_write(void* user, CPUState* cpu, u32 addr, u32 value, u8 size) {
    GcnGp* gp = (GcnGp*)user;
    (void)addr;   /* any offset in the window is "append to the gather pipe" */

    /* Append `size` bytes big-endian (GPFifo FastWrite* store swapped values).
     * NOTE: the MMIO layer delivers `value` as u32, so a 64-bit store's high 32
     * bits are already truncated upstream — clamp to 4 bytes accordingly (a
     * u32 shifted by >=32 would be UB). The recompiled IPL uses 32-bit
     * gather-pipe stores (offsets +0/+4), for which this is exact; a future
     * 64-bit store would land as its low word and diverge loudly in the diff. */
    if (size > 4) size = 4;
    for (u8 i = 0; i < size; i++) {
        u8 b = (u8)(value >> (8u * (size - 1u - i)));
        gp->stage[gp->count++] = b;
        if (gp->count == GCN_GP_GATHER_SIZE) {
            gp_flush_burst(gp, cpu);
            gp->count = 0;
        }
    }
}
