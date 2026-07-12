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
#include "ai/ai.h"            /* advance the AI sample counter/AIINT per block */
#include "vi/vi.h"            /* advance the VI beam counter per block       */
#include "di/di.h"            /* complete deferred DI drive commands per block */
#include "gx/gx.h"            /* drain the GX FIFO + execute commands per block */
#include "pi/pi.h"            /* deliver pending external interrupts         */
#include "debug/rings.h"      /* always-on block/PC ring */
#include "debug/debug_server.h" /* pumped once per block (non-blocking) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <x86intrin.h>   /* __rdtsc — GCN_DISPATCH_STATS attribution only */

/* Nominal PPC-cycle budget handed to the DSP per executed block (the DSP core
 * advances ~1/6th of it). A recompiled block is only ~5-10 PPC cycles, so this
 * should be ~5-12 to track the true DSP:CPU clock ratio. The old value (600) ran
 * the DSP LLE core ~60-120x too fast per block, which — because the DSP core is
 * the single dominant runtime cost — made the whole emulation a slideshow
 * (~1 fps). 12 is the measured sweet spot: functional (boot animation/intro
 * sequencer unfreeze intact), oracle-clean (matched=19355, one benign PI
 * reorder), and ~20x faster. Override with GCN_DSP_CYCLES (dispatch.c). */
#define GCN_DSP_CYCLES_PER_BLOCK 12u

/* Guest time-base ticks advanced per executed block. The real Gekko TB runs at
 * bus/4; we don't have per-block instruction counts here, so M0 uses a fixed
 * monotonic tick. This only needs to advance so that mftb-based delay loops
 * terminate — the oracle diff is MMIO value+order, insensitive to the exact TB
 * rate. Refine to a real cycle model once the recompiler emits per-block counts. */
#define GCN_TB_TICKS_PER_BLOCK 8u

/* Monotonic device-clock cycles (Gekko core cycles, TB*12) advanced per block.
 * This is deliberately NOT ctx->timebase: the guest can WRITE the TB (mttbl/
 * mttbu — the IPL zeroes it during OS init), but hardware device clocks (the
 * VI 27 MHz pixel clock) keep running through a TB write. Deriving device time
 * from the writable TB made the beam jump backward when the IPL rebased it. */
#define GCN_CORE_CYCLES_PER_BLOCK (GCN_TB_TICKS_PER_BLOCK * 12u)

/* M1 handoff snapshot (see dispatch.h): armed by boot.c, captured at the first
 * pc==0x81300000 block below. */
static u8* s_bs1_snap = NULL;
static u32 s_bs1_snap_len = 0;
static u32 s_bs1_snap_want = 0;

void gcn_dispatch_arm_bs1_snapshot(u32 len) {
    s_bs1_snap_want = len;
}

const u8* gcn_dispatch_bs1_snapshot(u32* len_out) {
    *len_out = s_bs1_snap_len;
    return s_bs1_snap;
}

/* Own the run loop (rather than the generated static-inline dolrecomp_run_blocks)
 * so we can advance the time base between blocks — otherwise mftb reads a frozen
 * TB and firmware delay loops spin forever. */
int gcn_dispatch_run(CPUState* ctx, u32 max_blocks) {
    u32 blocks = 0;
    static u64 device_cycles = 0;   /* monotonic across nested/repeated runs */

    /* DSP cycles advanced per executed block. The DSP-LLE core is the dominant
     * runtime cost (running the DSP ucode/spin ~100 cycles per CPU block), and
     * the fixed 600 PPC-cycle value was heavily over-provisioned: a recompiled
     * block is only ~5-10 PPC cycles, so 600 ran the DSP ~60-100x faster than the
     * true ~1/6 DSP:CPU ratio warrants. GCN_DSP_CYCLES overrides it (0 keeps the
     * DSP ticking but does no work). Read ONCE here, not per block. */
    u32 dsp_cycles_per_block = GCN_DSP_CYCLES_PER_BLOCK;
    { const char* e = getenv("GCN_DSP_CYCLES");
      if (e && *e) dsp_cycles_per_block = (u32)strtoul(e, NULL, 0); }

    /* GCN_DISPATCH_STATS=1: per-component wall attribution (see the stats
     * branch below). Read once; the tsc accumulators live across nested runs. */
    static int s_dstats = -1;
    static u64 s_tsc[6];
    if (s_dstats < 0) s_dstats = getenv("GCN_DISPATCH_STATS") ? 1 : 0;

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
        /* Block boundaries are the safe points for asynchronous exceptions:
         * vector to 0x500 here if the PI has (cause & mask) and MSR[EE]. The
         * handler entry then flows through the same pending/clear dance as any
         * synchronous exception below. */
        gcn_pi_deliver_external(ctx);
        gcn_ring_block(ctx->pc);   /* always-on retired-block/PC timeline */

        /* M1 (opt-in, GCN_LOG_BS1_HANDOFF=1): one-shot dump of the FULL CPU
         * state the instant execution reaches BS2's entry (0x81300000),
         * whichever path got here — the "fixture cross-check" docs/M1_PLAN.md
         * §7 step 7 proposed: BS1 (real, GCN_BOOT_BS1=1) must produce the same
         * HID0/BAT/MSR values Dolphin's HLE hardcodes at its 0x81200150 landing
         * point (Boot.cpp:456-461, cited in the plan), even though Dolphin
         * cannot be diffed live here (it never executes real BS1). Default
         * off: byte-identical to before this existed. */
        if (ctx->pc == 0x81300000u) {
            /* M1 integrity snapshot (dispatch.h): capture the DMA'd BS2 image
             * NOW, before a single BS2 instruction mutates its own data
             * sections. One-shot; malloc'd (~1.5 MB) only when armed. */
            if (s_bs1_snap_want && !s_bs1_snap && ctx->ram &&
                0x01300000u + (u64)s_bs1_snap_want <= ctx->ram_size) {
                s_bs1_snap = (u8*)malloc(s_bs1_snap_want);
                if (s_bs1_snap) {
                    memcpy(s_bs1_snap, ctx->ram + 0x01300000u, s_bs1_snap_want);
                    s_bs1_snap_len = s_bs1_snap_want;
                }
            }
            static int logged = 0;
            const char* want_log = getenv("GCN_LOG_BS1_HANDOFF");
            if (!logged && want_log && *want_log && *want_log != '0') {
                logged = 1;
                fprintf(stdout,
                    "\n--- BS1->BS2 HANDOFF CPU STATE (pc=0x81300000) ---\n"
                    "  msr   = 0x%08X   hid0 = 0x%08X   hid2 = 0x%08X\n"
                    "  lr    = 0x%08X   cr = 0x%08X   xer = 0x%08X\n"
                    "  sr[0..3]   = 0x%08X 0x%08X 0x%08X 0x%08X\n"
                    "  dbat0 u/l  = 0x%08X 0x%08X   ibat0 u/l = 0x%08X 0x%08X\n"
                    "  dbat1 u/l  = 0x%08X 0x%08X   ibat1 u/l = 0x%08X 0x%08X\n"
                    "  dbat2 u/l  = 0x%08X 0x%08X   ibat2 u/l = 0x%08X 0x%08X\n"
                    "  dbat3 u/l  = 0x%08X 0x%08X   ibat3 u/l = 0x%08X 0x%08X\n"
                    "  gpr[1](sp) = 0x%08X   gpr[3] = 0x%08X   gpr[4] = 0x%08X\n",
                    /* SPR numbering (PowerPC ISA): HID0=1008; IBAT0..3 U/L =
                     * 528..535, DBAT0..3 U/L = 536..543 (matches cpu_glue.c's
                     * ppc_spr_access[528..543/1008] = SPR_RW range). */
                    ctx->msr, ctx->spr[1008], ctx->hid2, ctx->lr, ctx->cr, ctx->xer,
                    ctx->sr[0], ctx->sr[1], ctx->sr[2], ctx->sr[3],
                    ctx->spr[536], ctx->spr[537], ctx->spr[528], ctx->spr[529],
                    ctx->spr[538], ctx->spr[539], ctx->spr[530], ctx->spr[531],
                    ctx->spr[540], ctx->spr[541], ctx->spr[532], ctx->spr[533],
                    ctx->spr[542], ctx->spr[543], ctx->spr[534], ctx->spr[535],
                    ctx->gpr[1], ctx->gpr[3], ctx->gpr[4]);
                fflush(stdout);
            }
        }

        u32 pending = ctx->exception;
        ctx->exception = 0;
        /* GCN_DISPATCH_STATS=1: rdtsc-based wall attribution of the per-block
         * loop across block-exec and each device tick, printed every 2^20
         * blocks. Shares (not absolute ns) are the product — the tsc frequency
         * never needs calibrating. Off by default; ~zero cost when off. */
        if (s_dstats) {
            u64 t0 = __rdtsc();
            int ok = dolrecomp_call(ctx, ctx->pc);
            u64 t1 = __rdtsc(); s_tsc[0] += t1 - t0;
            if (!ok) { ctx->exception = pending; return 0; }
            ctx->timebase += GCN_TB_TICKS_PER_BLOCK;
            device_cycles += GCN_CORE_CYCLES_PER_BLOCK;
            gcn_dsp_tick(dsp_cycles_per_block);
            u64 t2 = __rdtsc(); s_tsc[1] += t2 - t1;
            gcn_ai_tick();
            u64 t3 = __rdtsc(); s_tsc[2] += t3 - t2;
            gcn_vi_tick(device_cycles);
            u64 t4 = __rdtsc(); s_tsc[3] += t4 - t3;
            gcn_di_tick();
            u64 t5 = __rdtsc(); s_tsc[4] += t5 - t4;
            gcn_gx_tick(GCN_CORE_CYCLES_PER_BLOCK);
            u64 t6 = __rdtsc(); s_tsc[5] += t6 - t5;
            if ((blocks & 0xFFFFFu) == 0xFFFFFu) {
                u64 tot = s_tsc[0]+s_tsc[1]+s_tsc[2]+s_tsc[3]+s_tsc[4]+s_tsc[5];
                fprintf(stderr, "[dispatch-stats] blocks=%u  block-exec=%.1f%% "
                        "dsp=%.1f%% ai=%.1f%% vi=%.1f%% di=%.1f%% gx=%.1f%%\n",
                        blocks + 1u,
                        100.0*(double)s_tsc[0]/(double)tot, 100.0*(double)s_tsc[1]/(double)tot,
                        100.0*(double)s_tsc[2]/(double)tot, 100.0*(double)s_tsc[3]/(double)tot,
                        100.0*(double)s_tsc[4]/(double)tot, 100.0*(double)s_tsc[5]/(double)tot);
                fflush(stderr);
            }
        } else {
        if (!dolrecomp_call(ctx, ctx->pc)) {   /* off-image PC / no handler */
            ctx->exception = pending;
            return 0;
        }
        ctx->timebase += GCN_TB_TICKS_PER_BLOCK;
        device_cycles += GCN_CORE_CYCLES_PER_BLOCK;
        /* Accrues DSP-cycle debt and flushes it at CPU observation points or
         * the K-cycle cap (default 4096 PPC cycles, GCN_DSP_BATCH_PPC) —
         * no longer runs the DSP core every block; see gcn_dsp_flush (dsp.c). */
        gcn_dsp_tick(dsp_cycles_per_block);
        gcn_ai_tick();                            /* pace AISCNT / AIINT while PSTAT=1 */
        gcn_vi_tick(device_cycles);              /* sweep the VI beam + latch DIs */
        gcn_di_tick();                           /* complete a deferred DI command */
        gcn_gx_tick(GCN_CORE_CYCLES_PER_BLOCK);  /* drain + execute GX FIFO commands */
        }
        /* Service the debug server between blocks: non-blocking, so it stays
         * responsive even while the guest busy-waits on unmodeled hardware. A
         * client "quit" ends the run cleanly. Pumped every 256 blocks rather than
         * every block — the accept()/recv() syscalls were a per-block hot-path
         * cost, and 256 blocks is well under a millisecond of guest time, so the
         * TCP surface (screenshot/set_input/quit) stays responsive. */
        if ((blocks & 0xFFu) == 0u) {
            gcn_debug_server_pump();
            if (gcn_debug_server_quit_requested())
                return 1;
        }
        blocks++;
    }
    return 1;
}

u32 gcn_dispatch_entry(void) {
    return DOLRECOMP_ENTRY_POINT;
}
