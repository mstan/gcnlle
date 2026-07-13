/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Fixed (non-generated) execution harness for the bl/blr in-chunk shadow
 * fast path. Linked against the synthetic func_80003000 that tests/
 * test_bl_shadow_gen.c emits (see that file's header comment for the exact
 * instruction layout and why it's shaped this way) by tests/cmake/
 * bl_shadow_run.cmake, which also RUNS the resulting executable as part of
 * the recompiler's ctest suite (bl_shadow_exactness).
 *
 * This is a real link+execute test, not just "does it compile": it proves
 * the fast (shadow push/pop) path and the slow (pre-existing, always-return)
 * fallback path reach bit-identical architectural end state from the same
 * generated code, differing only in how many dispatch-loop round trips it
 * took to get there.
 */
#include <stdio.h>
#include <string.h>

#include "cpu/cpu.h"

extern void func_80003000(CPUState* ctx);

#define CHUNK_START 0x80003000u
#define CHUNK_END   0x80003014u
#define SENTINEL_PC 0xDEAD0000u
#define INITIAL_LR  0xCAFEF00Du /* unconditionally clobbered by the bl; never read back */
#define INITIAL_R3  1000u
#define EXPECT_R3_DELTA 101u /* +100 (callee) + 1 (caller, post-return) */
#define EXPECT_LR   0x80003004u /* the bl's own return address */

/* uniform_deadline: mirrors GCN_CYCLES_UNIFORM's pinned ctx->cycle_deadline
 * == 0 (the fast path is provably dead -- see emitter.c's doc comments on
 * the lk-branch push and is_blr_class pop). When false, cycle_deadline is
 * re-armed generously every "dolrecomp_call" (mimicking runtime/src/
 * dispatch.c's real quantum re-arm), letting the fast path fire. */
static int run_scenario(const char* name, int uniform_deadline,
                        int expect_calls, u32* out_cycles) {
    CPUState ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.pc = CHUNK_START;
    ctx.lr = INITIAL_LR;
    ctx.gpr[3] = INITIAL_R3;
    ctx.cycles = 0;

    int calls = 0;
    while (ctx.pc >= CHUNK_START && ctx.pc < CHUNK_END) {
        ctx.cycle_deadline = uniform_deadline ? 0u : (ctx.cycles + 1000u);
        func_80003000(&ctx);
        calls++;
        if (calls > 10) {
            printf("BL_SHADOW,%s,FAIL,runaway (>10 dispatch-loop re-entries)\n", name);
            return 1;
        }
    }

    int ok = 1;
    if (ctx.pc != SENTINEL_PC) {
        printf("BL_SHADOW,%s,FAIL,final pc = 0x%08X (want 0x%08X)\n", name, ctx.pc, SENTINEL_PC);
        ok = 0;
    }
    if (ctx.gpr[3] != INITIAL_R3 + EXPECT_R3_DELTA) {
        printf("BL_SHADOW,%s,FAIL,gpr[3] = %u (want %u)\n", name, ctx.gpr[3], INITIAL_R3 + EXPECT_R3_DELTA);
        ok = 0;
    }
    if (ctx.lr != EXPECT_LR) {
        printf("BL_SHADOW,%s,FAIL,lr = 0x%08X (want 0x%08X)\n", name, ctx.lr, EXPECT_LR);
        ok = 0;
    }
    if (calls != expect_calls) {
        printf("BL_SHADOW,%s,FAIL,dispatch-loop re-entries = %d (want %d)\n", name, calls, expect_calls);
        ok = 0;
    }
    if (ok) {
        printf("BL_SHADOW,%s,PASS,calls=%d pc=0x%08X gpr3=%u lr=0x%08X cycles=%u\n",
               name, calls, ctx.pc, ctx.gpr[3], ctx.lr, (unsigned)ctx.cycles);
    }
    if (out_cycles) *out_cycles = (u32)ctx.cycles;
    return ok ? 0 : 1;
}

int main(void) {
    int fails = 0;
    u32 fast_cycles = 0, slow_cycles = 0;

    /* Fast path: generous deadline -- the bl push and the callee's blr pop
     * both fire, so the whole call+return chain resolves inside a SINGLE
     * func_80003000 invocation via goto, never round-tripping through the
     * (simulated) dispatch loop. */
    fails += run_scenario("fast_shadow_path", 0, 1, &fast_cycles);

    /* Slow path: cycle_deadline pinned to 0 on every entry, exactly like the
     * GCN_CYCLES_UNIFORM golden baseline -- every push/pop condition is
     * always false, so every branch falls back to the pre-existing `ctx->pc
     * = target; return;` shape byte-for-byte, and the mini dispatch loop
     * re-enters func_80003000 once per hop: bl's slow return, the callee's
     * blr's slow return, then the final exit branch. Exactly the pre-change
     * 3-hop shape. */
    fails += run_scenario("slow_fallback_path", 1, 3, &slow_cycles);

    /* The coalesced per-block cycle charge is emitted unconditionally on
     * both the fast and slow branches of every push/pop site (see emitter.c:
     * `ctx->cycles += cycle_charge;` precedes the push/pop check in both
     * emit_direct_branch and emit_dynamic_branch) -- so the TOTAL cycles
     * charged across the whole call+return chain must be identical whether
     * it took 1 host-level call or 3. A mismatch here would mean the fast
     * path double-charges or under-charges relative to derived cycle
     * accuracy, which GCN_CYCLES_UNIFORM's byte-identical guarantee depends
     * on staying exact. */
    if (fast_cycles != slow_cycles) {
        printf("BL_SHADOW,cycle_parity,FAIL,fast=%u slow=%u\n", fast_cycles, slow_cycles);
        fails++;
    } else {
        printf("BL_SHADOW,cycle_parity,PASS,cycles=%u\n", fast_cycles);
    }

    printf("BL_SHADOW,total,%s\n", fails == 0 ? "PASS" : "FAIL");
    return fails == 0 ? 0 : 1;
}
