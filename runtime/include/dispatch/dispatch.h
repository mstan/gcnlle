/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Dispatch driver — the seam between the runtime and Track A's recompiled C.
 *
 * DolRecomp emits the dispatch table as static-inline helpers in the generated
 * header, so a single non-static TU (src/dispatch.c) #includes that header and
 * re-exposes the driver here. This keeps every other runtime TU free of any
 * dependency on the generated code.
 */
#ifndef GCN_DISPATCH_H
#define GCN_DISPATCH_H

#include "cpu/cpu.h"

/* Run recompiled blocks starting from ctx->pc, up to max_blocks (0 = unbounded
 * — avoid; firmware busy-waits on hardware and will spin). Returns:
 *   1  the block budget was reached and execution is still live;
 *   0  execution stopped — either ctx->exception is set, or ctx->pc has no
 *      generated function / host-call (fell off the recompiled image).
 * The caller inspects ctx->pc / ctx->exception to classify the stop. Unmapped
 * MMIO touched along the way is reported by the memory layer (loud warning),
 * which is the M0 signal for which device to model next. */
int gcn_dispatch_run(CPUState* ctx, u32 max_blocks);

/* The recompiled image's entry point (DOLRECOMP_ENTRY_POINT). */
u32 gcn_dispatch_entry(void);

/* M1: one-shot snapshot of MEM1[0x81300000, +len) taken THE INSTANT execution
 * first reaches BS2's entry — i.e. the moment BS1's EXI DMA has finished and
 * nothing of BS2 has run yet. The integrity check (boot.c) must compare THIS
 * against the offline-descramble reference, never the end-of-run memory: BS2's
 * own .data/.bss live inside the DMA'd span, so any post-handoff execution
 * legitimately mutates it (a longer block budget would turn a true PASS into a
 * false FAIL). Armed by boot.c before the run when BS1 mode + a reference are
 * configured; returns NULL until the handoff happened. */
void      gcn_dispatch_arm_bs1_snapshot(u32 len);
const u8* gcn_dispatch_bs1_snapshot(u32* len_out);

#endif /* GCN_DISPATCH_H */
