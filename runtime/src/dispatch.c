/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The one TU that touches Track A's generated code. It #includes the generated
 * header (which brings in the static-inline DolRecomp dispatch table and the
 * func_XXXXXXXX prototypes) and re-exposes a non-static driver. Built only when
 * GCN_WITH_GENERATED is set and runtime/generated/ has been populated by
 * generate.sh; the include path adds runtime/generated for "generated.h".
 */
#include "generated.h"        /* DolRecomp dispatch inlines + func_ decls */
#include "dispatch/dispatch.h"

/* Guest time-base ticks advanced per executed block. The real Gekko TB runs at
 * bus/4; we don't have per-block instruction counts here, so M0 uses a fixed
 * monotonic tick. This only needs to advance so that mftb-based delay loops
 * terminate — the oracle diff is MMIO value+order, insensitive to the exact TB
 * rate. Refine to a real cycle model once the recompiler emits per-block counts. */
#define GCN_TB_TICKS_PER_BLOCK 8u

/* Own the run loop (rather than the generated static-inline dolrecomp_run_blocks)
 * so we can advance the time base between blocks — otherwise mftb reads a frozen
 * TB and firmware delay loops spin forever. */
int gcn_dispatch_run(CPUState* ctx, u32 max_blocks) {
    u32 blocks = 0;
    while (max_blocks == 0u || blocks < max_blocks) {
        if (!dolrecomp_call(ctx, ctx->pc)) return 0;  /* off-image PC / no func */
        if (ctx->exception) return 0;
        ctx->timebase += GCN_TB_TICKS_PER_BLOCK;
        blocks++;
    }
    return 1;
}

u32 gcn_dispatch_entry(void) {
    return DOLRECOMP_ENTRY_POINT;
}
