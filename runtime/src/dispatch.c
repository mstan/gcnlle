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
#include "dsp/dsp.h"          /* advance the real DSP core alongside the CPU */

/* Nominal PPC-cycle budget handed to the DSP per executed block (the DSP runs
 * ~1/6th of it). The DSP only acts on CPU stimuli, so a generous rate keeps its
 * IROM/ucode responsive without racing; exact timing is absorbed by the
 * poll-aware oracle diff. */
#define GCN_DSP_CYCLES_PER_BLOCK 600u

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
        /* A block that raises an exception returns with ctx->pc set to the vector
         * (e.g. 0xC00 for `sc`) and ctx->exception still set — the generated
         * per-instruction guards use that flag to abort the FAULTING block. But
         * the exception has now been delivered: ctx->pc IS the handler. Clear the
         * flag before running the handler block, or its own first guard
         * (`if (ctx->exception) return;`) misfires and it loops on the vector
         * without ever executing. If the vector has no recompiled handler,
         * dolrecomp_call returns 0; restore the flag so the stop diagnostic still
         * reports the exception. The handler ends in `rfi` (pc=srr0). */
        u32 pending = ctx->exception;
        ctx->exception = 0;
        if (!dolrecomp_call(ctx, ctx->pc)) {   /* off-image PC / no handler */
            ctx->exception = pending;
            return 0;
        }
        ctx->timebase += GCN_TB_TICKS_PER_BLOCK;
        gcn_dsp_tick(GCN_DSP_CYCLES_PER_BLOCK);  /* run the DSP core in step */
        blocks++;
    }
    return 1;
}

u32 gcn_dispatch_entry(void) {
    return DOLRECOMP_ENTRY_POINT;
}
