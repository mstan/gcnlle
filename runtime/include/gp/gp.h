/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Gather Pipe (GP) model — the 32-byte write-gather FIFO at 0xCC008000.
 *
 * The Gekko's write-gather pipe accumulates CPU stores to a 32-byte staging
 * buffer; each time 32 bytes land, the block is DMA'd to the GX command FIFO in
 * main memory at the PI FIFO write pointer, which then advances (wrapping
 * END->BASE). Transcribed from Dolphin GPFifo (Core/HW/GPFifo.h:19-22,
 * GPFifo.cpp:85-131) and the CP side (CommandProcessor.cpp:339-407):
 *
 *   - Any store of any size at any offset in the 0x20 window appends its bytes
 *     (big-endian) to the staging buffer — the recompiled IPL emits 32-bit
 *     stores at +0 and +4 (GPFifo.cpp FastWrite*:157-186 swap to big-endian).
 *   - On each accumulated 32 bytes (UpdateGatherPipe:85-112): copy the 32 bytes
 *     into guest RAM at the PI FIFO write pointer, then advance that pointer —
 *     wrapping to PI FIFO base when it equals PI FIFO end, else += 32
 *     (GPFifo.cpp:100-104; the GC FIFO_END addresses the last 32-byte slot).
 *   - Then run the CP gather-pipe-burst handler (gcn_cp_gather_pipe_bursted):
 *     the CPU-side status/interrupt evaluation always, plus — when the CP has
 *     GPLinkEnable — mirroring the write pointer into CPWritePointer and adding
 *     32 to CPReadWriteDistance (CommandProcessor.cpp:363-407, single-core).
 *
 * Every 32-byte burst is also mirrored into the always-on FIFO recorder ring
 * (debug/rings.h) — ROADMAP M2's "GX FIFO recorder", the packet inventory that
 * scopes the future FIFO interpreter.
 *
 * PI FIFO write-pointer bytes are guest-PHYSICAL (0x0xxxxxxx); the RAM copy uses
 * (wptr & 0x1FFFFFFF) into cpu->ram, bounds-checked. gcn_gp_reset() clears the
 * staging buffer (ProcessorInterface.cpp:112-119, PI_FIFO_RESET / GXAbortFrame).
 */
#ifndef GCN_GP_GP_H
#define GCN_GP_GP_H

#include "cpu/cpu.h"
#include "pi/pi.h"
#include "cp/cp.h"

#define GCN_GP_BASE  0xCC008000u
#define GCN_GP_SIZE  0x20u           /* the 32-byte gather-pipe window */

/* GATHER_PIPE_SIZE (GPFifo.h:21) re-exported under the GP name for callers. */
#define GCN_GP_GATHER_SIZE  GCN_CP_GATHER_PIPE_SIZE

typedef struct {
    u8     stage[GCN_GP_GATHER_SIZE];        /* 32-byte staging buffer */
    u32    count;                            /* bytes staged so far    */
    GcnPi* pi;                               /* PI FIFO base/end/wptr owner */
    GcnCp* cp;                               /* CP side (link/status/interrupt) */
} GcnGp;

void gcn_gp_init(GcnGp* gp, GcnPi* pi, GcnCp* cp);
u32  gcn_gp_read(void* user, CPUState* cpu, u32 addr, u8 size);
void gcn_gp_write(void* user, CPUState* cpu, u32 addr, u32 value, u8 size);

/* Reset the gather-pipe staging (PI_FIFO_RESET / GXAbortFrame). Matches the
 * `void(void)` hook type pi.h exposes; operates on the registered singleton. */
void gcn_gp_reset(void);

#endif /* GCN_GP_GP_H */
