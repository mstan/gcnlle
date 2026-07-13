/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Generator half of the bl/blr in-chunk shadow-call-stack exactness test
 * (Phase D2 codegen speed campaign). Emits ONE synthetic chunk function
 * spanning 0x80003000..0x80003014 that exercises exactly the new fast path
 * added to emit_direct_branch (lk-branch push) and emit_dynamic_branch
 * (is_blr_class pop) in recompiler/src/backend/emitter.c:
 *
 *   0x80003000: bl   0x8000300C     ; call local callee (push-eligible: both
 *                                   ; the callee and the return site 0x80003004
 *                                   ; are local labels of this same chunk)
 *   0x80003004: addi r3, r3, 1      ; return-path marker (only reachable by
 *                                   ; correctly resuming here, fast or slow)
 *   0x80003008: b    0xDEAD0000     ; unconditional exit to an out-of-chunk
 *                                   ; sentinel -- a clean, unambiguous end of
 *                                   ; this synthetic "function"
 *   0x8000300C: addi r3, r3, 100    ; callee body
 *   0x80003010: blr                 ; callee return (bo=20,bi=0: unconditional)
 *
 * tests/test_bl_shadow_harness.c (a FIXED, non-generated file compiled
 * alongside this generator's output by tests/cmake/bl_shadow_run.cmake) runs
 * the resulting func_80003000 twice -- once with a generous cycle_deadline
 * (the shadow push/pop fast path fires) and once with cycle_deadline pinned
 * to 0 exactly like the GCN_CYCLES_UNIFORM golden baseline (the fast path is
 * provably dead, every branch falls back to the pre-existing always-return
 * shape) -- and asserts the two runs reach IDENTICAL architectural end state
 * (final pc, gpr[3], lr, ctx->cycles), differing only in how many times the
 * mini dispatch loop had to re-enter func_80003000. That equality is the
 * exactness proof for the optimization: it is observationally a no-op.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/emitter.h"
#include "../src/common/types.h"

#define BASE 0x80003000u

int main(int argc, char** argv) {
    FILE* out = stdout;

    if (argc > 1) {
        out = fopen(argv[1], "w");
        if (!out) {
            perror(argv[1]);
            return 1;
        }
    }

    PPCInst insts[5];
    memset(insts, 0, sizeof(insts));

    /* 0x80003000: bl 0x8000300C */
    insts[0].op = PPC_OP_B;
    insts[0].address = BASE + 0x00u;
    insts[0].branch_target = BASE + 0x0Cu;
    insts[0].lk = true;

    /* 0x80003004: addi r3, r3, 1 */
    insts[1].op = PPC_OP_ADDI;
    insts[1].address = BASE + 0x04u;
    insts[1].rD = 3;
    insts[1].rA = 3;
    insts[1].simm = 1;

    /* 0x80003008: b 0xDEAD0000 (external sentinel, unconditional, non-lk) */
    insts[2].op = PPC_OP_B;
    insts[2].address = BASE + 0x08u;
    insts[2].branch_target = 0xDEAD0000u;

    /* 0x8000300C: addi r3, r3, 100 */
    insts[3].op = PPC_OP_ADDI;
    insts[3].address = BASE + 0x0Cu;
    insts[3].rD = 3;
    insts[3].rA = 3;
    insts[3].simm = 100;

    /* 0x80003010: blr (bo=20 -> ctr_ignored && cond_ignored: unconditional) */
    insts[4].op = PPC_OP_BCLR;
    insts[4].address = BASE + 0x10u;
    insts[4].bo = 20;
    insts[4].bi = 0;

    emit_header(out);
    emit_function(out, insts, 5, BASE);
    emit_footer(out);

    if (out != stdout) {
        if (fclose(out) != 0) {
            perror(argv[1]);
            return 1;
        }
    }

    return 0;
}
