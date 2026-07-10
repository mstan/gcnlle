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

#endif /* GCN_DISPATCH_H */
