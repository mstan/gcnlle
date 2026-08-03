/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Exactness test for the interpreter's counted-cache-loop batching fast path
 * (interpreter.c: try_batch_cache_loop). Runs the classic 3-instruction
 * PowerPC cache-maintenance idiom --
 *     loop:  <cache-op> 0,rX     ; icbi / dcbi
 *            addi       rX,rX,32
 *            bdnz       loop
 * -- through the interpreter twice, once with the fast path forced OFF
 * (gcn_interpreter_set_cache_loop_batch_enabled(false), the ground truth:
 * every iteration actually steps) and once forced ON, and asserts the two
 * runs land on IDENTICAL final GPR/CTR/cycles state and IDENTICAL native
 * code invalidation. A third case crosses a cpu->cycle_deadline mid-loop to
 * confirm the batch stops exactly at the deadline (same yield semantics as
 * the unbatched path) rather than running the whole loop in one shot.
 *
 * NOTE: cpu_init()'s locked-L1 cache model is a process-wide singleton (only
 * one live CPUState at a time -- a second concurrent cpu_init() fails and
 * frees that CPUState's RAM). Every case below therefore inits, runs, snap-
 * shots the result into plain locals, and cpu_free()s before moving on --
 * never two CPUState objects alive together.
 */
#include "cpu/interpreter.h"
#include "cpu/native_code.h"
#include "debug/rings.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

typedef struct {
    u32 gpr6;
    u32 ctr;
    u64 cycles;
    u32 pc;
    u32 invalid_pages;
} LoopResult;

static void put_insn(CPUState* cpu, u32 address, u32 raw) {
    write_be32(cpu->ram + (address & 0x1FFFFFFFu), raw);
}

/* Lays down the loop at `base`: <op> 0,r6 ; addi r6,r6,32 ; bdnz -8 */
static void put_cache_loop(CPUState* cpu, u32 base, u32 cache_op_raw) {
    put_insn(cpu, base + 0u, cache_op_raw);   /* <op> 0,r6            */
    put_insn(cpu, base + 4u, 0x38C60020u);    /* addi r6,r6,32        */
    put_insn(cpu, base + 8u, 0x4200FFF8u);    /* bdnz -8              */
}

/* Runs until pc reaches exit_pc via repeated gcn_interpreter_step calls --
 * mirroring dispatch.c's per-instruction fallback cadence. */
static void run_to_completion(CPUState* cpu, u32 exit_pc) {
    int guard = 0;
    while (cpu->pc != exit_pc) {
        CHECK(gcn_interpreter_step(cpu));
        if (++guard > 10000000) { CHECK(0 && "run_to_completion did not converge"); break; }
    }
}

/* Lays down a fresh cache_op loop + backing code bytes over the range the
 * icbi loop will flush, and parks cpu->pc at the loop head. Caller must have
 * already cpu_init()'d cpu and gcn_native_code_reset() for a clean bitmap. */
static void setup(CPUState* cpu, u32 loop_base, u32 addr_reg_start, u32 ctr,
                   u32 cache_op_raw) {
    cpu->pc = loop_base;
    cpu->ctr = ctr;
    cpu->msr = 0u;                     /* MSR[EE] clear -- required for the fast path */
    cpu->cycles = 0;
    cpu->cycle_deadline = UINT64_MAX;  /* no deadline pressure by default */
    cpu->exception = 0;
    cpu->gpr[6] = addr_reg_start;
    put_cache_loop(cpu, loop_base, cache_op_raw);
    /* Give the icbi loop real code bytes to invalidate over its range so the
     * native-code bitmap has something to observably diff. */
    for (u32 i = 0; i < ctr; i++)
        put_insn(cpu, addr_reg_start + i * 32u, 0x60000000u /* nop */);
}

/* Runs one full standalone case: init -> setup -> run to completion ->
 * snapshot -> free. batch_enabled selects the fast path for this run. */
static LoopResult run_one(u32 loop_base, u32 addr_reg_start, u32 ctr,
                           u32 cache_op_raw, bool batch_enabled) {
    CPUState cpu;
    CHECK(cpu_init(&cpu));
    gcn_native_code_reset();
    gcn_interpreter_set_cache_loop_batch_enabled(batch_enabled);
    setup(&cpu, loop_base, addr_reg_start, ctr, cache_op_raw);
    run_to_completion(&cpu, loop_base + 12u);
    LoopResult r = {
        cpu.gpr[6], cpu.ctr, cpu.cycles, cpu.pc,
        gcn_native_code_invalid_page_count(),
    };
    cpu_free(&cpu);
    gcn_interpreter_set_cache_loop_batch_enabled(true);
    return r;
}

static void expect_equal(const char* name, LoopResult a, LoopResult b) {
    bool ok = a.gpr6 == b.gpr6 && a.ctr == b.ctr && a.cycles == b.cycles &&
              a.pc == b.pc && a.invalid_pages == b.invalid_pages;
    CHECK(ok);
    if (!ok) {
        fprintf(stderr,
                "%s: unbatched{gpr6=%08X ctr=%08X cycles=%llu pc=%08X inv=%u} "
                "batched{gpr6=%08X ctr=%08X cycles=%llu pc=%08X inv=%u}\n",
                name, a.gpr6, a.ctr, (unsigned long long)a.cycles, a.pc, a.invalid_pages,
                b.gpr6, b.ctr, (unsigned long long)b.cycles, b.pc, b.invalid_pages);
    }
}

int main(void) {
    gcn_rings_init();

    /* icbi 0,r6 ; addi r6,r6,32 ; bdnz -8 -- mirrors BS1's cluster at
     * 0x812FFFA8 (ctr=0x8000 per invocation in the real trace; a smaller
     * count keeps this test fast while exercising the identical shape). */
    {
        LoopResult unbatched = run_one(0x80001000u, 0x80010000u, 0x100u, 0x7C0037ACu, false);
        LoopResult batched   = run_one(0x80001000u, 0x80010000u, 0x100u, 0x7C0037ACu, true);
        expect_equal("icbi loop, run to completion", unbatched, batched);
    }

    /* dcbi 0,r6 ; addi r6,r6,32 ; bdnz -8 -- mirrors BS1's cluster at
     * 0x81201090 (dcbi is a no-op in this interpreter's cache model, so this
     * exercises the pure GPR/CTR/cycle side of the batch with no
     * invalidation side effect at all). */
    {
        LoopResult unbatched = run_one(0x80002000u, 0x80020000u, 0x137u /* odd count */,
                                        0x7C0033ACu, false);
        LoopResult batched   = run_one(0x80002000u, 0x80020000u, 0x137u,
                                        0x7C0033ACu, true);
        expect_equal("dcbi loop, run to completion", unbatched, batched);
    }

    /* Deadline crossing mid-loop: force the fast path to stop after exactly
     * 40 of 256 iterations (fewer cycles than a full loop needs), verify it
     * left CTR/GPR/pc exactly where the unbatched path would be after 40
     * iterations, then let both sides finish and compare final state. */
    {
        LoopResult unbatched = run_one(0x80003000u, 0x80030000u, 0x100u, 0x7C0037ACu, false);

        CPUState cpu;
        CHECK(cpu_init(&cpu));
        gcn_native_code_reset();
        gcn_interpreter_set_cache_loop_batch_enabled(true);
        setup(&cpu, 0x80003000u, 0x80030000u, 0x100u, 0x7C0037ACu);
        cpu.cycle_deadline = cpu.cycles + 40u * 6u; /* icbi(4)+addi(1)+bc(1)=6/iter */
        CHECK(gcn_interpreter_step(&cpu));          /* one batched call: <=40 iters */
        CHECK(cpu.pc == 0x80003000u);               /* still mid-loop, not exited   */
        CHECK(cpu.ctr == 0x100u - 40u);
        CHECK(cpu.gpr[6] == 0x80030000u + 40u * 32u);
        CHECK(cpu.cycles == 40u * 6u);
        /* Deadline exhausted -- dispatch.c would recompute a fresh one for the
         * next outer-loop turn; simulate that, then let the loop finish. */
        cpu.cycle_deadline = UINT64_MAX;
        run_to_completion(&cpu, 0x80003000u + 12u);
        LoopResult batched = {
            cpu.gpr[6], cpu.ctr, cpu.cycles, cpu.pc,
            gcn_native_code_invalid_page_count(),
        };
        cpu_free(&cpu);
        gcn_interpreter_set_cache_loop_batch_enabled(true);

        expect_equal("icbi loop, deadline crossing mid-loop", unbatched, batched);
    }

    /* Non-loop icbi (no addi/bdnz following) must NOT be batched -- ordinary
     * single-instruction icbi semantics still apply. */
    {
        CPUState cpu;
        CHECK(cpu_init(&cpu));
        gcn_native_code_reset();
        gcn_interpreter_set_cache_loop_batch_enabled(true);
        cpu.pc = 0x80004000u;
        cpu.gpr[6] = 0x80040000u;
        cpu.msr = 0u;
        put_insn(&cpu, 0x80004000u, 0x7C0037ACu);  /* icbi 0,r6               */
        put_insn(&cpu, 0x80004004u, 0x38000000u);  /* li r0,0 (not the idiom) */
        put_insn(&cpu, 0x80040000u, 0x60000000u);
        CHECK(!gcn_native_code_is_invalid(0x80040000u));
        CHECK(gcn_interpreter_step(&cpu));
        CHECK(gcn_native_code_is_invalid(0x80040000u));
        CHECK(cpu.pc == 0x80004004u);
        CHECK(cpu.ctr == 0u);   /* unchanged: no bdnz executed */
        cpu_free(&cpu);
        gcn_interpreter_set_cache_loop_batch_enabled(true);
    }

    if (failures)
        fprintf(stderr, "cache loop batch exactness: %d failure(s)\n", failures);
    else
        puts("cache loop batch exactness: ok");
    return failures ? 1 : 0;
}
