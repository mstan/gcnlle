// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ExpansionPak

#include "emitter.h"
#include "ppc_cycles.h"
#include <stdlib.h>
#include <string.h>

static u32 cr_field_shift(u8 crf) {
    return 4u * (7u - (u32)crf);
}

static u32 ppc_mask32(u8 mb, u8 me) {
    u32 mask = 0;
    u8 bit = mb;

    for (;;) {
        mask |= 0x80000000u >> bit;
        if (bit == me)
            break;
        bit = (u8)((bit + 1) & 31);
    }

    return mask;
}

static void emit_set_cr0_from_gpr(FILE* out, u8 reg) {
    fprintf(out, "        u32 cr_bits = 0;\n");
    fprintf(out, "        s32 cr_value = (s32)ctx->gpr[%u];\n", reg);
    fprintf(out, "        if (cr_value < 0)  cr_bits |= 0x8u;\n");
    fprintf(out, "        if (cr_value > 0)  cr_bits |= 0x4u;\n");
    fprintf(out, "        if (cr_value == 0) cr_bits |= 0x2u;\n");
    fprintf(out, "        cr_bits |= (ctx->xer >> 31) & 1u;\n");
    fprintf(out, "        ctx->cr = (ctx->cr & 0x0FFFFFFFu) | (cr_bits << 28);\n");
}

static void emit_set_cr1_from_fpscr(FILE* out) {
    fprintf(out, "        ctx->cr = (ctx->cr & 0xF0FFFFFFu) | ((ctx->fpscr >> 4) & 0x0F000000u);\n");
}

static void emit_compare_s32(FILE* out, u8 crf, const char* lhs, const char* rhs) {
    u32 shift = cr_field_shift(crf);

    fprintf(out, "    {\n");
    fprintf(out, "        s32 val_a = (s32)(%s);\n", lhs);
    fprintf(out, "        s32 val_b = (s32)(%s);\n", rhs);
    fprintf(out, "        u32 cr_bits = 0;\n");
    fprintf(out, "        if (val_a < val_b)  cr_bits |= 0x8u;\n");
    fprintf(out, "        if (val_a > val_b)  cr_bits |= 0x4u;\n");
    fprintf(out, "        if (val_a == val_b) cr_bits |= 0x2u;\n");
    fprintf(out, "        cr_bits |= (ctx->xer >> 31) & 1u;\n");
    fprintf(out, "        ctx->cr = (ctx->cr & ~(0xFu << %u)) | (cr_bits << %u);\n",
            shift, shift);
    fprintf(out, "    }\n");
}

static void emit_compare_u32(FILE* out, u8 crf, const char* lhs, const char* rhs) {
    u32 shift = cr_field_shift(crf);

    fprintf(out, "    {\n");
    fprintf(out, "        u32 val_a = (u32)(%s);\n", lhs);
    fprintf(out, "        u32 val_b = (u32)(%s);\n", rhs);
    fprintf(out, "        u32 cr_bits = 0;\n");
    fprintf(out, "        if (val_a < val_b)  cr_bits |= 0x8u;\n");
    fprintf(out, "        if (val_a > val_b)  cr_bits |= 0x4u;\n");
    fprintf(out, "        if (val_a == val_b) cr_bits |= 0x2u;\n");
    fprintf(out, "        cr_bits |= (ctx->xer >> 31) & 1u;\n");
    fprintf(out, "        ctx->cr = (ctx->cr & ~(0xFu << %u)) | (cr_bits << %u);\n",
            shift, shift);
    fprintf(out, "    }\n");
}

static void emit_fcompare(FILE* out, const PPCInst* inst) {
    u32 shift = cr_field_shift(inst->crfD);

    fprintf(out, "    {\n");
    fprintf(out, "        f64 val_a = ctx->fpr[%u];\n", inst->rA);
    fprintf(out, "        f64 val_b = ctx->fpr[%u];\n", inst->rB);
    fprintf(out, "        u32 cr_bits = 0;\n");
    fprintf(out, "        if (val_a < val_b)       cr_bits = 0x8u;\n");
    fprintf(out, "        else if (val_a > val_b)  cr_bits = 0x4u;\n");
    fprintf(out, "        else if (val_a == val_b) cr_bits = 0x2u;\n");
    fprintf(out, "        else                     cr_bits = 0x1u;\n");
    fprintf(out, "        ctx->cr = (ctx->cr & ~(0xFu << %u)) | (cr_bits << %u);\n",
            shift, shift);
    fprintf(out, "    }\n");
}

static void emit_dform_ea(FILE* out, u8 ra, s16 simm, bool update) {
    if (ra == 0 && !update) {
        fprintf(out, "(u32)(s32)(%d)", (int)simm);
    } else {
        fprintf(out, "ctx->gpr[%u] + (u32)(s32)(%d)", ra, (int)simm);
    }
}

static void emit_xform_ea(FILE* out, u8 ra, u8 rb, bool update) {
    if (ra == 0 && !update) {
        fprintf(out, "ctx->gpr[%u]", rb);
    } else {
        fprintf(out, "ctx->gpr[%u] + ctx->gpr[%u]", ra, rb);
    }
}

/* Phase C (codegen speed campaign): plain (non-string, non-reserved,
 * non-quantized) integer/float loads and stores lower to a call to the
 * inline fast-path helpers emitted per-TU by emit_header_for_cpu
 * (dolrecomp_mem_read*_fast/dolrecomp_mem_write*_fast) instead of the bus
 * primitive directly -- gcc inlines those (same TU, `static inline`), which
 * is the whole point: a window-check + direct RAM access (via the dr_ram local) replaces a
 * helper-call round trip for the common RAM-hit case, falling back to the
 * real mem_read* / mem_write* (now always passed this instruction's own
 * address as cia) only for the rare non-RAM case. Update-form writeback to
 * rA is UNCONDITIONAL on every path (matches the pre-Phase-C behavior
 * exactly: mem_read* / mem_write* never signal failure back to the caller for
 * this opcode class -- an unmapped/MMIO access just routes to the device
 * layer or warns and returns 0, it never raises ctx->exception the way
 * psq_load/store or dcbz_l do -- so there is no fault path to guard the
 * writeback against; a faulting lwzu's rA update happens exactly as it does
 * today). */

static void emit_load_fast(FILE* out, const PPCInst* inst, u32 width,
                           bool sign_extend, bool update) {
    const char* fn = width == 32 ? "dolrecomp_mem_read32_fast"
                    : width == 16 ? "dolrecomp_mem_read16_fast"
                    : "dolrecomp_mem_read8_fast";
    fprintf(out, "    {\n");
    fprintf(out, "        u32 ea = ");
    emit_dform_ea(out, inst->rA, inst->simm, update);
    fprintf(out, ";\n");
    if (sign_extend) {
        fprintf(out, "        ctx->gpr[%u] = (u32)(s32)(%s)%s(ctx, dr_ram, ea, 0x%08Xu);\n",
                inst->rD, width == 16 ? "s16" : "s8", fn, inst->address);
    } else {
        fprintf(out, "        ctx->gpr[%u] = %s(ctx, dr_ram, ea, 0x%08Xu);\n",
                inst->rD, fn, inst->address);
    }
    if (update) {
        fprintf(out, "        ctx->gpr[%u] = ea;\n", inst->rA);
    }
    fprintf(out, "    }\n");
}

static void emit_loadx_fast(FILE* out, const PPCInst* inst, u32 width,
                            bool sign_extend, bool update) {
    const char* fn = width == 32 ? "dolrecomp_mem_read32_fast"
                    : width == 16 ? "dolrecomp_mem_read16_fast"
                    : "dolrecomp_mem_read8_fast";
    fprintf(out, "    {\n");
    fprintf(out, "        u32 ea = ");
    emit_xform_ea(out, inst->rA, inst->rB, update);
    fprintf(out, ";\n");
    if (sign_extend) {
        fprintf(out, "        ctx->gpr[%u] = (u32)(s32)(%s)%s(ctx, dr_ram, ea, 0x%08Xu);\n",
                inst->rD, width == 16 ? "s16" : "s8", fn, inst->address);
    } else {
        fprintf(out, "        ctx->gpr[%u] = %s(ctx, dr_ram, ea, 0x%08Xu);\n",
                inst->rD, fn, inst->address);
    }
    if (update) {
        fprintf(out, "        ctx->gpr[%u] = ea;\n", inst->rA);
    }
    fprintf(out, "    }\n");
}

static void emit_store_fast(FILE* out, const PPCInst* inst, u32 width, bool update) {
    const char* fn = width == 32 ? "dolrecomp_mem_write32_fast"
                    : width == 16 ? "dolrecomp_mem_write16_fast"
                    : "dolrecomp_mem_write8_fast";
    const char* cast = width == 32 ? "u32" : width == 16 ? "u16" : "u8";
    fprintf(out, "    {\n");
    fprintf(out, "        u32 ea = ");
    emit_dform_ea(out, inst->rA, inst->simm, update);
    fprintf(out, ";\n");
    fprintf(out, "        %s(ctx, dr_ram, ea, (%s)ctx->gpr[%u], 0x%08Xu);\n",
            fn, cast, inst->rS, inst->address);
    if (update) {
        fprintf(out, "        ctx->gpr[%u] = ea;\n", inst->rA);
    }
    fprintf(out, "    }\n");
}

static void emit_storex_fast(FILE* out, const PPCInst* inst, u32 width, bool update) {
    const char* fn = width == 32 ? "dolrecomp_mem_write32_fast"
                    : width == 16 ? "dolrecomp_mem_write16_fast"
                    : "dolrecomp_mem_write8_fast";
    const char* cast = width == 32 ? "u32" : width == 16 ? "u16" : "u8";
    fprintf(out, "    {\n");
    fprintf(out, "        u32 ea = ");
    emit_xform_ea(out, inst->rA, inst->rB, update);
    fprintf(out, ";\n");
    fprintf(out, "        %s(ctx, dr_ram, ea, (%s)ctx->gpr[%u], 0x%08Xu);\n",
            fn, cast, inst->rS, inst->address);
    if (update) {
        fprintf(out, "        ctx->gpr[%u] = ea;\n", inst->rA);
    }
    fprintf(out, "    }\n");
}

static void emit_fload_fast(FILE* out, const PPCInst* inst, bool single, bool update) {
    fprintf(out, "    {\n");
    fprintf(out, "        u32 ea = ");
    emit_dform_ea(out, inst->rA, inst->simm, update);
    fprintf(out, ";\n");
    if (single) {
        fprintf(out, "        f64 value = (f64)dolrecomp_f32_from_bits(dolrecomp_mem_read32_fast(ctx, dr_ram, ea, 0x%08Xu));\n",
                inst->address);
        fprintf(out, "        ctx->fpr[%u] = value;\n", inst->rD);
        fprintf(out, "        ctx->ps1[%u] = value;\n", inst->rD);
    } else {
        fprintf(out, "        ctx->fpr[%u] = dolrecomp_f64_from_bits(dolrecomp_mem_read64_fast(ctx, dr_ram, ea, 0x%08Xu));\n",
                inst->rD, inst->address);
    }
    if (update) {
        fprintf(out, "        ctx->gpr[%u] = ea;\n", inst->rA);
    }
    fprintf(out, "    }\n");
}

static void emit_floadx_fast(FILE* out, const PPCInst* inst, bool single, bool update) {
    fprintf(out, "    {\n");
    fprintf(out, "        u32 ea = ");
    emit_xform_ea(out, inst->rA, inst->rB, update);
    fprintf(out, ";\n");
    if (single) {
        fprintf(out, "        f64 value = (f64)dolrecomp_f32_from_bits(dolrecomp_mem_read32_fast(ctx, dr_ram, ea, 0x%08Xu));\n",
                inst->address);
        fprintf(out, "        ctx->fpr[%u] = value;\n", inst->rD);
        fprintf(out, "        ctx->ps1[%u] = value;\n", inst->rD);
    } else {
        fprintf(out, "        ctx->fpr[%u] = dolrecomp_f64_from_bits(dolrecomp_mem_read64_fast(ctx, dr_ram, ea, 0x%08Xu));\n",
                inst->rD, inst->address);
    }
    if (update) {
        fprintf(out, "        ctx->gpr[%u] = ea;\n", inst->rA);
    }
    fprintf(out, "    }\n");
}

static void emit_fstore_fast(FILE* out, const PPCInst* inst, bool single, bool update) {
    fprintf(out, "    {\n");
    fprintf(out, "        u32 ea = ");
    emit_dform_ea(out, inst->rA, inst->simm, update);
    fprintf(out, ";\n");
    if (single) {
        fprintf(out, "        dolrecomp_mem_write32_fast(ctx, dr_ram, ea, dolrecomp_f32_to_bits((f32)ctx->fpr[%u]), 0x%08Xu);\n",
                inst->rS, inst->address);
    } else {
        fprintf(out, "        dolrecomp_mem_write64_fast(ctx, dr_ram, ea, dolrecomp_f64_to_bits(ctx->fpr[%u]), 0x%08Xu);\n",
                inst->rS, inst->address);
    }
    if (update) {
        fprintf(out, "        ctx->gpr[%u] = ea;\n", inst->rA);
    }
    fprintf(out, "    }\n");
}

static void emit_fstorex_fast(FILE* out, const PPCInst* inst, bool single, bool update) {
    fprintf(out, "    {\n");
    fprintf(out, "        u32 ea = ");
    emit_xform_ea(out, inst->rA, inst->rB, update);
    fprintf(out, ";\n");
    if (single) {
        fprintf(out, "        dolrecomp_mem_write32_fast(ctx, dr_ram, ea, dolrecomp_f32_to_bits((f32)ctx->fpr[%u]), 0x%08Xu);\n",
                inst->rS, inst->address);
    } else {
        fprintf(out, "        dolrecomp_mem_write64_fast(ctx, dr_ram, ea, dolrecomp_f64_to_bits(ctx->fpr[%u]), 0x%08Xu);\n",
                inst->rS, inst->address);
    }
    if (update) {
        fprintf(out, "        ctx->gpr[%u] = ea;\n", inst->rA);
    }
    fprintf(out, "    }\n");
}

static void emit_psq_load(FILE* out, const PPCInst* inst, bool indexed,
                          bool update, u32 cycle_charge) {
    fprintf(out, "    {\n");
    fprintf(out, "        u32 ea = ");
    if (indexed) {
        emit_xform_ea(out, inst->rA, inst->rB, update);
    } else {
        emit_dform_ea(out, inst->rA, inst->simm, update);
    }
    fprintf(out, ";\n");
    fprintf(out, "        ppc_psq_load(ctx, %uu, ea, %s, %uu, %s, 0x%08Xu);\n",
            inst->rD, inst->w ? "true" : "false", inst->i,
            indexed ? "true" : "false", inst->address);
    /* Premature exit mid-block: charge this instruction's block-leader..here
     * prefix (see emit_function's cum[]/PREFIX design note) before leaving,
     * since the block's normal end-of-block charge is never reached on this
     * path. */
    fprintf(out, "        if (ctx->exception) { dr_cycles += %uu; DR_RET(); }\n",
            cycle_charge);
    if (update) {
        fprintf(out, "        ctx->gpr[%u] = ea;\n", inst->rA);
    }
    fprintf(out, "    }\n");
}

static void emit_psq_store(FILE* out, const PPCInst* inst, bool indexed,
                           bool update, u32 cycle_charge) {
    fprintf(out, "    {\n");
    fprintf(out, "        u32 ea = ");
    if (indexed) {
        emit_xform_ea(out, inst->rA, inst->rB, update);
    } else {
        emit_dform_ea(out, inst->rA, inst->simm, update);
    }
    fprintf(out, ";\n");
    fprintf(out, "        ppc_psq_store(ctx, %uu, ea, %s, %uu, %s, 0x%08Xu);\n",
            inst->rS, inst->w ? "true" : "false", inst->i,
            indexed ? "true" : "false", inst->address);
    fprintf(out, "        if (ctx->exception) { dr_cycles += %uu; DR_RET(); }\n",
            cycle_charge);
    if (update) {
        fprintf(out, "        ctx->gpr[%u] = ea;\n", inst->rA);
    }
    fprintf(out, "    }\n");
}

static void emit_dcbz(FILE* out, const PPCInst* inst) {
    fprintf(out, "    {\n");
    fprintf(out, "        u32 ea = ");
    emit_xform_ea(out, inst->rA, inst->rB, false);
    fprintf(out, ";\n");
    fprintf(out, "        ea &= ~31u;\n");
    fprintf(out, "        for (u32 i = 0; i < 32; i += 4) mem_write32(ctx, ea + i, 0, 0x%08Xu);\n",
            inst->address);
    fprintf(out, "    }\n");
}

static void emit_branch_condition(FILE* out, u8 bo, u8 bi) {
    bool ctr_ignored = (bo & 0x04) != 0;
    bool cond_ignored = (bo & 0x10) != 0;

    if (!ctr_ignored) {
        fprintf(out, "        ctx->ctr--;\n");
        fprintf(out, "        bool ctr_ok = (((ctx->ctr != 0) ? 1u : 0u) ^ %uu) != 0;\n",
                (bo >> 1) & 1u);
    } else {
        fprintf(out, "        bool ctr_ok = true;\n");
    }

    if (!cond_ignored) {
        u32 mask = 0x80000000u >> bi;
        fprintf(out, "        bool cr_ok = (((ctx->cr & 0x%08Xu) != 0) == %s);\n",
                mask, ((bo >> 3) & 1u) ? "true" : "false");
    } else {
        fprintf(out, "        bool cr_ok = true;\n");
    }
}

static bool branch_target_is_local(u32 func_start, u32 func_end, u32 target) {
    return target >= func_start && target < func_end && ((target - func_start) & 3u) == 0;
}

/* Exact tight-poll-loop class optimization.
 *
 * Recognize the common three-instruction hardware/firmware wait shape:
 *
 *   lwz     rD, disp(rA)
 *   cmplwi  crF, rD, imm
 *   beq     crF, loop
 *
 * The ordinary emitter already keeps a taken backward conditional inside the
 * generated chunk until dr_deadline, but its generic RAM helper must classify
 * the effective address on every iteration.  Large chunk TUs can also cause
 * GCC to tail-share that RAM-hit body far away from the loop.  This predicate
 * lets emit_function add a compact path at the load label which computes the
 * loop-invariant EA and RAM pointer once, then performs every guest load in a
 * small native loop.  It is a class transform -- no PC or firmware identity is
 * involved -- and deliberately accepts only the simplest fully-auditable
 * shape.  The normal instruction bodies/labels remain emitted as the exact
 * fallback for uniform mode, non-RAM/unaligned addresses, and dispatch entry at
 * either of the two interior instructions. */
static bool is_lwz_cmpli_beq_poll(const PPCInst* insts, u32 count, u32 i) {
    if (i + 3u >= count)
        return false; /* needs a real fallthrough label after the branch */

    const PPCInst* load = &insts[i];
    const PPCInst* cmp = &insts[i + 1u];
    const PPCInst* branch = &insts[i + 2u];

    return load->op == PPC_OP_LWZ && !load->embedded_data &&
           cmp->op == PPC_OP_CMPLI && !cmp->embedded_data && cmp->l == 0u &&
           cmp->rA == load->rD &&
           branch->op == PPC_OP_BC && !branch->embedded_data && !branch->lk &&
           branch->bo == 12u && branch->bi == (u8)(cmp->crfD * 4u + 2u) &&
           branch->branch_target == load->address &&
           load->address + 4u == cmp->address &&
           cmp->address + 4u == branch->address &&
           /* The base must remain invariant across the load itself. */
           (load->rA == 0u || load->rD != load->rA);
}

static void emit_lwz_cmpli_beq_poll(FILE* out, const PPCInst* load,
                                    const PPCInst* cmp, const PPCInst* branch,
                                    u32 cycle_charge, u32 fallthrough) {
    const u32 cr_shift = 28u - (u32)cmp->crfD * 4u;
    const u32 cr_mask = 0xFu << cr_shift;

    fprintf(out, "    /* Exact lwz/cmplwi/beq poll fast path (class peephole). */\n");
    fprintf(out, "    if (dr_cycles < dr_deadline) {\n");
    fprintf(out, "        u32 dr_poll_ea = ");
    emit_dform_ea(out, load->rA, load->simm, false);
    fprintf(out, ";\n");
    fprintf(out, "        u8* dr_poll_ptr = NULL;\n");
    fprintf(out, "        if ((dr_poll_ea & 3u) == 0u) {\n");
    fprintf(out, "            if (dr_poll_ea >= GC_RAM_BASE && dr_poll_ea <= GC_RAM_BASE + (GC_MAIN_RAM_SIZE - 4u))\n");
    fprintf(out, "                dr_poll_ptr = dr_ram + (dr_poll_ea - GC_RAM_BASE);\n");
    fprintf(out, "            else if (dr_poll_ea >= GC_RAM_UNCACHED && dr_poll_ea <= GC_RAM_UNCACHED + (GC_MAIN_RAM_SIZE - 4u))\n");
    fprintf(out, "                dr_poll_ptr = dr_ram + (dr_poll_ea - GC_RAM_UNCACHED);\n");
    fprintf(out, "            else if (dr_poll_ea <= GC_MAIN_RAM_SIZE - 4u)\n");
    fprintf(out, "                dr_poll_ptr = dr_ram + dr_poll_ea;\n");
    fprintf(out, "        }\n");
    fprintf(out, "        if (dr_poll_ptr) {\n");
    fprintf(out, "            u32 dr_poll_value;\n");
    fprintf(out, "            for (;;) {\n");
    /* Aligned atomic load makes every guest lwz an actual host load even if a
     * device worker can change the polled word concurrently; it compiles to a
     * plain aligned load on x86 and avoids introducing a C data-race/hoist. */
    fprintf(out, "                dr_poll_value = bswap32(__atomic_load_n((const u32*)dr_poll_ptr, __ATOMIC_RELAXED));\n");
    fprintf(out, "                dr_cycles += %uu;\n", cycle_charge);
    fprintf(out, "                if (dr_poll_value != 0x%04Xu) break;\n", cmp->uimm);
    fprintf(out, "                if (dr_cycles >= dr_deadline) {\n");
    fprintf(out, "                    ctx->gpr[%u] = dr_poll_value;\n", load->rD);
    fprintf(out, "                    ctx->cr = (ctx->cr & ~0x%08Xu) | ((0x2u | ((ctx->xer >> 31) & 1u)) << %uu);\n",
            cr_mask, cr_shift);
    fprintf(out, "                    ctx->pc = 0x%08Xu;\n", branch->branch_target);
    fprintf(out, "                    DR_RET();\n");
    fprintf(out, "                }\n");
    fprintf(out, "            }\n");
    fprintf(out, "            ctx->gpr[%u] = dr_poll_value;\n", load->rD);
    fprintf(out, "            {\n");
    fprintf(out, "                u32 dr_poll_cr = dr_poll_value < 0x%04Xu ? 0x8u : 0x4u;\n", cmp->uimm);
    fprintf(out, "                dr_poll_cr |= (ctx->xer >> 31) & 1u;\n");
    fprintf(out, "                ctx->cr = (ctx->cr & ~0x%08Xu) | (dr_poll_cr << %uu);\n",
            cr_mask, cr_shift);
    fprintf(out, "            }\n");
    fprintf(out, "            goto label_%08X;\n", fallthrough);
    fprintf(out, "        }\n");
    fprintf(out, "    }\n");
}

static void emit_direct_branch(FILE* out, const PPCInst* inst, bool local_target,
                               bool conditional, u32 cycle_charge,
                               u32 func_start, u32 func_end) {
    bool local_backward = local_target && inst->branch_target <= inst->address;

    /* Every path below leaves this instruction's block (either an actual
     * `return` out of the chunk function, or a `goto` to another block's
     * label), so each one charges this instruction's block-leader..here
     * cumulative cost (`cycle_charge`, emit_function's cum[i]) exactly once,
     * before the exit. On the deadline-goto path specifically, the charge
     * MUST land before the `if (dr_cycles < dr_deadline)` check --
     * that check has to observe the fully-charged value, matching what
     * per-instruction charging would have accumulated through this branch
     * instruction itself. */
    if (inst->lk) {
        /* bl / bcl: call semantics (return address in LR). LR is updated to
         * the exact same value on EVERY path below -- the fast in-chunk-call
         * path and the always-correct fallback agree on architectural state
         * byte-for-byte; they only differ in which host C construct reaches
         * the next instruction.
         *
         * Fast path (in-chunk call): when both the callee AND the return
         * site are local labels of THIS chunk function (branch_target_is_
         * local checked for both -- a bl as the chunk's last instruction has
         * no local return label and always falls back), push the return
         * label + architectural return PC onto the bounded, function-local
         * shadow stack (dr_ret_lbl/dr_ret_pc/dr_ret_sp, declared once at the
         * top of every emit_function body -- see its doc comment) and `goto`
         * the callee directly, skipping the dispatch-loop round trip
         * (runtime/src/dispatch.c: dolrecomp_call -> chunk lookup -> switch
         * (ctx->pc) re-dispatch) this call would otherwise pay. blr later
         * pops the matching entry the same way a local backward branch
         * reuses its own label (see local_backward below) -- this is the
         * SAME `goto label_X` mechanism already proven for local branches,
         * just entered via a shadow-stack lookup at the return instruction
         * instead of a compile-time-resolved target.
         *
         * Gated on `dr_cycles < dr_deadline`, exactly like the
         * backward-branch goto below: a chain of in-chunk calls (or a
         * call inside a guest loop) must still yield to the dispatch loop
         * at least once per quantum, so device ticks / interrupt delivery
         * lag by no more than one quantum -- identical bound to what
         * backward branches already accept. Pre-expired deadline (the
         * GCN_CYCLES_UNIFORM baseline's ctx->cycle_deadline == 0) makes the
         * check always false, so this fast path NEVER fires there and the
         * emitted control flow is byte-for-byte the pre-existing always-
         * return shape -- the golden baseline is untouched by construction,
         * same guarantee the backward-branch goto already relies on.
         *
         * Shadow-full (dr_ret_sp == DOLRECOMP_BL_SHADOW_DEPTH) also falls
         * back to the plain return -- a full shadow is not an error, just a
         * missed optimization for this one call. */
        bool local_call = local_target &&
                           branch_target_is_local(func_start, func_end, inst->address + 4u);
        fprintf(out, "            dr_cycles += %uu;\n", cycle_charge);
        fprintf(out, "            ctx->lr = 0x%08Xu;\n", inst->address + 4);
        if (local_call) {
            fprintf(out, "            if (dr_ret_sp < DOLRECOMP_BL_SHADOW_DEPTH && dr_cycles < dr_deadline) {\n");
            fprintf(out, "                dr_ret_pc[dr_ret_sp] = 0x%08Xu;\n", inst->address + 4);
            fprintf(out, "                dr_ret_lbl[dr_ret_sp] = &&label_%08X;\n", inst->address + 4);
            fprintf(out, "                dr_ret_sp++;\n");
            fprintf(out, "                goto label_%08X;\n", inst->branch_target);
            fprintf(out, "            }\n");
        }
        fprintf(out, "            ctx->pc = 0x%08Xu;\n", inst->branch_target);
        fprintf(out, "            DR_RET();\n");
        return;
    }
    if (local_backward) {
        fprintf(out, "            dr_cycles += %uu;\n", cycle_charge);
        /* Deadline yield (derived cycle accuracy). Only a TAKEN CONDITIONAL
         * (bc, including bdnz/bdz) backward edge inside this chunk can form
         * a tight guest delay loop, so only `conditional` gets the
         * in-function goto; a plain unconditional `b` (conditional==false)
         * falls through unchanged to the same `ctx->pc = ...; return;` it
         * always emitted -- changing that would alter GCN_CYCLES_UNIFORM
         * block counts for no benefit (an unconditional backward edge is an
         * infinite loop unless something else exits the chunk first, so
         * bounding it here buys nothing bc/bdnz doesn't already cover).
         *
         * Bookkeeping enumeration -- what the goto path skips relative to
         * the return path, and why it's safe for up to GCN_CYCLE_QUANTUM
         * cycles of guest time:
         *   - The emitter puts NOTHING else on the return path for a plain
         *     (non-lk) conditional branch -- no inline interrupt check, no
         *     extra bookkeeping between `ctx->pc = ...` and `return;` besides
         *     the `ctx->cycles += cycle_charge;` above, which is shared by
         *     both the goto and return outcomes (it runs before the `if`,
         *     so it's charged exactly once regardless of which fires). So
         *     there is nothing else here for the goto path to replicate.
         *   - The dispatch loop (runtime/src/dispatch.c) does its per-return
         *     work only on an actual return: it drains/latches
         *     ctx->exception (`pending = ctx->exception; ctx->exception =
         *     0;`) and ticks VI/DI/GX/DSP/AI, which is also where a PI
         *     interrupt gets raised into ctx->exception for the NEXT return
         *     to observe. None of that runs while we stay on the goto path.
         *     That is precisely what ctx->cycle_deadline bounds: the
         *     dispatch loop sets it to ctx->cycles + the quantum before
         *     every dolrecomp_call, so device ticks / interrupt delivery can
         *     lag real guest progress by at most one quantum, never
         *     unboundedly.
         *   - CTR-decrementing forms (bdnz/bdz) already had their
         *     `ctx->ctr--` executed by emit_branch_condition BEFORE this
         *     function is even called (see the `if (ctr_ok && cr_ok) {`
         *     wrapper the PPC_OP_BC case emits around us) -- unconditionally,
         *     whether the branch is taken or not. So CTR decrements exactly
         *     once per iteration on both the goto and the return path.
         *
         * Pre-expired deadline (ctx->cycle_deadline <= ctx->cycles, e.g. the
         * GCN_CYCLES_UNIFORM baseline's 0) makes `ctx->cycles <
         * ctx->cycle_deadline` always false, so this goto NEVER fires and
         * the emitted control flow reproduces the pre-change always-return
         * behavior byte-for-byte. */
        if (conditional) {
            fprintf(out, "            if (dr_cycles < dr_deadline) goto label_%08X;\n",
                    inst->branch_target);
        }
        fprintf(out, "            ctx->pc = 0x%08Xu;\n", inst->branch_target);
        fprintf(out, "            DR_RET();\n");
    } else if (local_target) {
        fprintf(out, "            dr_cycles += %uu;\n", cycle_charge);
        fprintf(out, "            goto label_%08X;\n", inst->branch_target);
    } else {
        fprintf(out, "            dr_cycles += %uu;\n", cycle_charge);
        fprintf(out, "            ctx->pc = 0x%08Xu;\n", inst->branch_target);
        fprintf(out, "            DR_RET();\n");
    }
}

static void emit_dynamic_branch(FILE* out, const PPCInst* inst,
                                const char* target_expr, u32 cycle_charge,
                                bool is_blr_class) {
    fprintf(out, "    {\n");
    fprintf(out, "        u32 target = %s;\n", target_expr);
    emit_branch_condition(out, inst->bo, inst->bi);
    fprintf(out, "        if (ctr_ok && cr_ok) {\n");
    /* Taken path leaves this block via return (or, for the blr shadow-pop
     * fast path below, a `goto` back into this same chunk function) --
     * charge the block-leader..here cumulative cost once. The not-taken
     * path falls through to the next instruction and is covered by
     * emit_function's block-boundary flush (bclr/bcctr always start a
     * fresh leader on the instruction that follows them, so that flush
     * fires exactly here). */
    fprintf(out, "            dr_cycles += %uu;\n", cycle_charge);
    if (is_blr_class) {
        /* Shadow-stack pop: this fires only when a same-chunk bl already
         * pushed a matching entry (see emit_direct_branch's local_call
         * path) -- dr_ret_pc[dr_ret_sp-1] is architecturally exactly the
         * `target` this blr computed from ctx->lr, so jumping straight to
         * the remembered label is observably identical to the fallback
         * (`ctx->pc = target; return;`) reaching the same label via the
         * dispatch loop's switch(ctx->pc) re-entry -- just without the
         * round trip. Any mismatch (target didn't come from an in-chunk
         * bl, shadow empty, or this chunk invocation never pushed at all)
         * falls straight through to the untouched, always-correct
         * fallback below.
         *
         * Deadline-gated exactly like the bl push side and the backward-
         * branch goto: bounds a bl/blr call-chain (or a guest loop that
         * calls a leaf function every iteration) to the same one-quantum
         * dispatch-loop lag those already accept, and pins this fast path
         * off entirely under the GCN_CYCLES_UNIFORM golden baseline
         * (ctx->cycle_deadline == 0) for the same byte-for-byte reason
         * documented at the bl push site. */
        fprintf(out, "            if (dr_ret_sp > 0 && target == dr_ret_pc[dr_ret_sp - 1] &&\n");
        fprintf(out, "                dr_cycles < dr_deadline) {\n");
        fprintf(out, "                void* dr_ret_target = dr_ret_lbl[--dr_ret_sp];\n");
        if (inst->lk) {
            fprintf(out, "                ctx->lr = 0x%08Xu;\n", inst->address + 4);
        }
        fprintf(out, "                goto *dr_ret_target;\n");
        fprintf(out, "            }\n");
    }
    if (inst->lk) {
        fprintf(out, "            ctx->lr = 0x%08Xu;\n", inst->address + 4);
    }
    fprintf(out, "            ctx->pc = target;\n");
    fprintf(out, "            DR_RET();\n");
    fprintf(out, "        }\n");
    fprintf(out, "    }\n");
}

static void emit_cr_logical(FILE* out, const PPCInst* inst, const char* expr) {
    fprintf(out, "    {\n");
    fprintf(out, "        u32 a = (ctx->cr >> (31u - %uu)) & 1u;\n", inst->rA);
    fprintf(out, "        u32 b = (ctx->cr >> (31u - %uu)) & 1u;\n", inst->rB);
    fprintf(out, "        u32 mask = 0x80000000u >> %u;\n", inst->rD);
    fprintf(out, "        u32 value = (%s) & 1u;\n", expr);
    fprintf(out, "        ctx->cr = (ctx->cr & ~mask) | (value ? mask : 0u);\n");
    fprintf(out, "    }\n");
}

static void emit_record_if_needed(FILE* out, const PPCInst* inst, u8 reg) {
    if (inst->rc) {
        emit_set_cr0_from_gpr(out, reg);
    }
}

static const char* emit_cpu_macro(DolRecompCPU cpu) {
    switch (cpu) {
    case DOLRECOMP_CPU_BROADWAY:
        return "BROADWAY";
    case DOLRECOMP_CPU_ESPRESSO:
        return "ESPRESSO";
    case DOLRECOMP_CPU_GEKKO:
    default:
        return "GEKKO";
    }
}

static const char* emit_cpu_label(DolRecompCPU cpu) {
    switch (cpu) {
    case DOLRECOMP_CPU_BROADWAY:
        return "broadway";
    case DOLRECOMP_CPU_ESPRESSO:
        return "espresso";
    case DOLRECOMP_CPU_GEKKO:
    default:
        return "gekko";
    }
}

void emit_header_for_cpu(FILE* out, DolRecompCPU cpu) {
    fprintf(out,
        "// DolRecomp output\n"
        "// cpu: %s\n"
        "\n"
        "#define DOLRECOMP_CPU_%s 1\n"
        "#define DOLRECOMP_CPU_NAME \"%s\"\n"
        "\n"
        /* In-chunk bl/blr fast path (codegen speed campaign, Phase D2): bounded
         * depth of the per-invocation shadow call stack every emit_function
         * body declares (dr_ret_lbl/dr_ret_pc/dr_ret_sp) -- see emit_direct_
         * branch's lk-branch handling and emit_dynamic_branch's is_blr_class
         * handling for the push/pop sites and their exactness argument. A
         * deeper guest call chain than this simply spills to the pre-existing,
         * always-correct dispatch-loop return for the calls past the limit --
         * never a correctness bound, only how much of a call chain can skip
         * the round trip. Sized generously above realistic non-recursive IPL
         * call depth while staying tiny (<1KB) per chunk-function stack frame. */
        "#define DOLRECOMP_BL_SHADOW_DEPTH 32\n"
        "\n"
        "#include <string.h>\n"
        "#include <math.h>\n"
        "#include \"cpu/cpu.h\"\n"
        "\n"
        /* E1 (perf campaign 2): every emit_function body keeps ctx->cycles in
         * a local (dr_cycles) for the whole invocation -- ctx->cycles is
         * observed by nothing reachable from inside a chunk (verified:
         * dispatch.c reads it only after the call returns; no MMIO handler or
         * ppc_* helper touches it), so the ONLY places the memory field must
         * be coherent are the function's exits. DR_RET() is that spill+return;
         * every `return` emitted inside a chunk body goes through it. */
        "#define DR_RET() do { ctx->cycles = dr_cycles; return; } while (0)\n"
        "\n"
        /* always_inline: Phase D2's --param budgets still left 391/1206 fast-
         * path call sites uninlined (gcc cold-call heuristics, documented in
         * runtime/CMakeLists.txt); forcing the issue is exact -- these
         * helpers are the hot path by construction. */
        "#define DR_MEM_INLINE static inline __attribute__((always_inline))\n"
        "\n"
        "static inline u32 dolrecomp_rotl32(u32 value, u32 sh) {\n"
        "    sh &= 31u;\n"
        "    return sh ? ((value << sh) | (value >> (32u - sh))) : value;\n"
        "}\n"
        "\n"
        "static inline f32 dolrecomp_f32_from_bits(u32 bits) {\n"
        "    f32 value;\n"
        "    memcpy(&value, &bits, sizeof(value));\n"
        "    return value;\n"
        "}\n"
        "\n"
        "static inline u32 dolrecomp_f32_to_bits(f32 value) {\n"
        "    u32 bits;\n"
        "    memcpy(&bits, &value, sizeof(bits));\n"
        "    return bits;\n"
        "}\n"
        "\n"
        "static inline f64 dolrecomp_f64_from_bits(u64 bits) {\n"
        "    f64 value;\n"
        "    memcpy(&value, &bits, sizeof(value));\n"
        "    return value;\n"
        "}\n"
        "\n"
        "static inline u64 dolrecomp_f64_to_bits(f64 value) {\n"
        "    u64 bits;\n"
        "    memcpy(&bits, &value, sizeof(bits));\n"
        "    return bits;\n"
        "}\n"
        "\n"
        "static inline f64 dolrecomp_ps_round(f64 value) {\n"
        "    return (f64)(f32)value;\n"
        "}\n"
        "\n"
        "static inline f64 dolrecomp_ps_from_bits(u32 bits) {\n"
        "    return (f64)dolrecomp_f32_from_bits(bits);\n"
        "}\n"
        "\n"
        "static inline u32 dolrecomp_ps_to_bits(f64 value) {\n"
        "    return dolrecomp_f32_to_bits((f32)value);\n"
        "}\n"
        "\n"
        "/* Phase C (codegen speed campaign): inline replica of memory.c's\n"
        " * mem_read / mem_write RAM-hit fast path -- the three non-\n"
        " * overlapping RAM windows (cached 0x80000000, uncached 0xC0000000,\n"
        " * and the physical/real-mode mirror at addresses < GC_MAIN_RAM_SIZE\n"
        " * used by exception handlers and BS1 pre-MSR[DR] bring-up) get a\n"
        " * direct big-endian access against the ram param (the caller's\n"
        " * dr_ram local, unaliasable by guest stores); anything else (ROM\n"
        " * window, MEM2, MMIO, unmapped) falls back to the real mem_read /\n"
        " * mem_write bus primitive, passed this call cia explicitly.\n"
        " * ctx->ram_size is always GC_MAIN_RAM_SIZE (cpu_init only\n"
        " * assignment, never changed after), so the window bounds are safe\n"
        " * to bake in as compile-time constants instead of a runtime\n"
        " * ctx->ram_size load. Store variants replicate mem_write\n"
        " * reservation-clear (lwarx/stwcx) on every RAM hit, not just the\n"
        " * slow path -- a plain store to a reserved line must still\n"
        " * invalidate the reservation. */\n"
        "DR_MEM_INLINE u8 dolrecomp_mem_read8_fast(CPUState* ctx, u8* const ram, u32 ea, u32 cia) {\n"
        "    if (ea >= GC_RAM_BASE && ea < GC_RAM_BASE + GC_MAIN_RAM_SIZE)\n"
        "        return ram[ea - GC_RAM_BASE];\n"
        "    if (ea >= GC_RAM_UNCACHED && ea < GC_RAM_UNCACHED + GC_MAIN_RAM_SIZE)\n"
        "        return ram[ea - GC_RAM_UNCACHED];\n"
        "    if (ea < GC_MAIN_RAM_SIZE)\n"
        "        return ram[ea];\n"
        "    return mem_read8(ctx, ea, cia);\n"
        "}\n"
        "\n"
        "DR_MEM_INLINE u16 dolrecomp_mem_read16_fast(CPUState* ctx, u8* const ram, u32 ea, u32 cia) {\n"
        "    if (ea >= GC_RAM_BASE && ea <= GC_RAM_BASE + (GC_MAIN_RAM_SIZE - 2u))\n"
        "        return read_be16(ram + (ea - GC_RAM_BASE));\n"
        "    if (ea >= GC_RAM_UNCACHED && ea <= GC_RAM_UNCACHED + (GC_MAIN_RAM_SIZE - 2u))\n"
        "        return read_be16(ram + (ea - GC_RAM_UNCACHED));\n"
        "    if (ea <= GC_MAIN_RAM_SIZE - 2u)\n"
        "        return read_be16(ram + ea);\n"
        "    return mem_read16(ctx, ea, cia);\n"
        "}\n"
        "\n"
        "DR_MEM_INLINE u32 dolrecomp_mem_read32_fast(CPUState* ctx, u8* const ram, u32 ea, u32 cia) {\n"
        "    if (ea >= GC_RAM_BASE && ea <= GC_RAM_BASE + (GC_MAIN_RAM_SIZE - 4u))\n"
        "        return read_be32(ram + (ea - GC_RAM_BASE));\n"
        "    if (ea >= GC_RAM_UNCACHED && ea <= GC_RAM_UNCACHED + (GC_MAIN_RAM_SIZE - 4u))\n"
        "        return read_be32(ram + (ea - GC_RAM_UNCACHED));\n"
        "    if (ea <= GC_MAIN_RAM_SIZE - 4u)\n"
        "        return read_be32(ram + ea);\n"
        "    return mem_read32(ctx, ea, cia);\n"
        "}\n"
        "\n"
        "DR_MEM_INLINE u64 dolrecomp_mem_read64_fast(CPUState* ctx, u8* const ram, u32 ea, u32 cia) {\n"
        "    if (ea >= GC_RAM_BASE && ea <= GC_RAM_BASE + (GC_MAIN_RAM_SIZE - 8u))\n"
        "        return read_be64(ram + (ea - GC_RAM_BASE));\n"
        "    if (ea >= GC_RAM_UNCACHED && ea <= GC_RAM_UNCACHED + (GC_MAIN_RAM_SIZE - 8u))\n"
        "        return read_be64(ram + (ea - GC_RAM_UNCACHED));\n"
        "    if (ea <= GC_MAIN_RAM_SIZE - 8u)\n"
        "        return read_be64(ram + ea);\n"
        "    return mem_read64(ctx, ea, cia);\n"
        "}\n"
        "\n"
        "DR_MEM_INLINE void dolrecomp_mem_write8_fast(CPUState* ctx, u8* const ram, u32 ea, u8 value, u32 cia) {\n"
        "    if (ea >= GC_RAM_BASE && ea < GC_RAM_BASE + GC_MAIN_RAM_SIZE) {\n"
        "        if (ctx->reserve_valid && ((ctx->reserve_addr ^ ea) & ~31u) == 0) ctx->reserve_valid = false;\n"
        "        ram[ea - GC_RAM_BASE] = value;\n"
        "        return;\n"
        "    }\n"
        "    if (ea >= GC_RAM_UNCACHED && ea < GC_RAM_UNCACHED + GC_MAIN_RAM_SIZE) {\n"
        "        if (ctx->reserve_valid && ((ctx->reserve_addr ^ ea) & ~31u) == 0) ctx->reserve_valid = false;\n"
        "        ram[ea - GC_RAM_UNCACHED] = value;\n"
        "        return;\n"
        "    }\n"
        "    if (ea < GC_MAIN_RAM_SIZE) {\n"
        "        if (ctx->reserve_valid && ((ctx->reserve_addr ^ ea) & ~31u) == 0) ctx->reserve_valid = false;\n"
        "        ram[ea] = value;\n"
        "        return;\n"
        "    }\n"
        "    mem_write8(ctx, ea, value, cia);\n"
        "}\n"
        "\n"
        "DR_MEM_INLINE void dolrecomp_mem_write16_fast(CPUState* ctx, u8* const ram, u32 ea, u16 value, u32 cia) {\n"
        "    if (ea >= GC_RAM_BASE && ea <= GC_RAM_BASE + (GC_MAIN_RAM_SIZE - 2u)) {\n"
        "        if (ctx->reserve_valid && ((ctx->reserve_addr ^ ea) & ~31u) == 0) ctx->reserve_valid = false;\n"
        "        write_be16(ram + (ea - GC_RAM_BASE), value);\n"
        "        return;\n"
        "    }\n"
        "    if (ea >= GC_RAM_UNCACHED && ea <= GC_RAM_UNCACHED + (GC_MAIN_RAM_SIZE - 2u)) {\n"
        "        if (ctx->reserve_valid && ((ctx->reserve_addr ^ ea) & ~31u) == 0) ctx->reserve_valid = false;\n"
        "        write_be16(ram + (ea - GC_RAM_UNCACHED), value);\n"
        "        return;\n"
        "    }\n"
        "    if (ea <= GC_MAIN_RAM_SIZE - 2u) {\n"
        "        if (ctx->reserve_valid && ((ctx->reserve_addr ^ ea) & ~31u) == 0) ctx->reserve_valid = false;\n"
        "        write_be16(ram + ea, value);\n"
        "        return;\n"
        "    }\n"
        "    mem_write16(ctx, ea, value, cia);\n"
        "}\n"
        "\n"
        "DR_MEM_INLINE void dolrecomp_mem_write32_fast(CPUState* ctx, u8* const ram, u32 ea, u32 value, u32 cia) {\n"
        "    if (ea >= GC_RAM_BASE && ea <= GC_RAM_BASE + (GC_MAIN_RAM_SIZE - 4u)) {\n"
        "        if (ctx->reserve_valid && ((ctx->reserve_addr ^ ea) & ~31u) == 0) ctx->reserve_valid = false;\n"
        "        write_be32(ram + (ea - GC_RAM_BASE), value);\n"
        "        return;\n"
        "    }\n"
        "    if (ea >= GC_RAM_UNCACHED && ea <= GC_RAM_UNCACHED + (GC_MAIN_RAM_SIZE - 4u)) {\n"
        "        if (ctx->reserve_valid && ((ctx->reserve_addr ^ ea) & ~31u) == 0) ctx->reserve_valid = false;\n"
        "        write_be32(ram + (ea - GC_RAM_UNCACHED), value);\n"
        "        return;\n"
        "    }\n"
        "    if (ea <= GC_MAIN_RAM_SIZE - 4u) {\n"
        "        if (ctx->reserve_valid && ((ctx->reserve_addr ^ ea) & ~31u) == 0) ctx->reserve_valid = false;\n"
        "        write_be32(ram + ea, value);\n"
        "        return;\n"
        "    }\n"
        "    mem_write32(ctx, ea, value, cia);\n"
        "}\n"
        "\n"
        "DR_MEM_INLINE void dolrecomp_mem_write64_fast(CPUState* ctx, u8* const ram, u32 ea, u64 value, u32 cia) {\n"
        "    if (ea >= GC_RAM_BASE && ea <= GC_RAM_BASE + (GC_MAIN_RAM_SIZE - 8u)) {\n"
        "        if (ctx->reserve_valid && ((ctx->reserve_addr ^ ea) & ~31u) == 0) ctx->reserve_valid = false;\n"
        "        write_be64(ram + (ea - GC_RAM_BASE), value);\n"
        "        return;\n"
        "    }\n"
        "    if (ea >= GC_RAM_UNCACHED && ea <= GC_RAM_UNCACHED + (GC_MAIN_RAM_SIZE - 8u)) {\n"
        "        if (ctx->reserve_valid && ((ctx->reserve_addr ^ ea) & ~31u) == 0) ctx->reserve_valid = false;\n"
        "        write_be64(ram + (ea - GC_RAM_UNCACHED), value);\n"
        "        return;\n"
        "    }\n"
        "    if (ea <= GC_MAIN_RAM_SIZE - 8u) {\n"
        "        if (ctx->reserve_valid && ((ctx->reserve_addr ^ ea) & ~31u) == 0) ctx->reserve_valid = false;\n"
        "        write_be64(ram + ea, value);\n"
        "        return;\n"
        "    }\n"
        "    mem_write64(ctx, ea, value, cia);\n"
        "}\n"
        "\n"
        ,
        emit_cpu_label(cpu),
        emit_cpu_macro(cpu),
        emit_cpu_label(cpu));
}

void emit_header(FILE* out) {
    emit_header_for_cpu(out, DOLRECOMP_CPU_GEKKO);
}

void emit_footer(FILE* out) {
    fprintf(out, "\n// end\n");
}

/* FP-class instructions: everything that hardware refuses with the FP-
 * unavailable exception (vector 0x800) when MSR[FP]=0 — float loads/stores,
 * FP arithmetic/moves/compares, FPSCR ops, every paired-single op, and the
 * quantized psq family. Gekko User's Manual 2.1.1/6.4.6.4: paired-single
 * and quantized ops are FP-class and take 0x800 before any HID2 program
 * check. The OS relies on this trap for lazy FPU context switching — an
 * interrupt handler's first FP instruction must fault so the OS can save
 * the interrupted thread's FPU state. Without the gate, handler FP work
 * executes on the interrupted code's live registers (this exact bug drew
 * the IPL menu's flood-garbled frames). */
static bool ppc_op_is_fp(PPCOpcode op) {
    switch (op) {
    case PPC_OP_FADDS: case PPC_OP_FSUBS: case PPC_OP_FMULS: case PPC_OP_FDIVS:
    case PPC_OP_FRES: case PPC_OP_FMADDS: case PPC_OP_FMSUBS:
    case PPC_OP_FNMADDS: case PPC_OP_FNMSUBS:
    case PPC_OP_FADD: case PPC_OP_FSUB: case PPC_OP_FMUL: case PPC_OP_FDIV:
    case PPC_OP_FRSQRTE: case PPC_OP_FMADD: case PPC_OP_FMSUB:
    case PPC_OP_FNMADD: case PPC_OP_FNMSUB:
    case PPC_OP_FCTIW: case PPC_OP_FCTIWZ: case PPC_OP_FRSP: case PPC_OP_FSEL:
    case PPC_OP_FMR: case PPC_OP_FNEG: case PPC_OP_FABS: case PPC_OP_FNABS:
    case PPC_OP_FCMPU: case PPC_OP_FCMPO:
    case PPC_OP_MTFSB0: case PPC_OP_MTFSB1: case PPC_OP_MCRFS:
    case PPC_OP_MFFS: case PPC_OP_MTFSF: case PPC_OP_MTFSFI:
    case PPC_OP_PS_ADD: case PPC_OP_PS_SUB: case PPC_OP_PS_MUL:
    case PPC_OP_PS_DIV: case PPC_OP_PS_RES: case PPC_OP_PS_RSQRTE:
    case PPC_OP_PS_MADD: case PPC_OP_PS_MSUB: case PPC_OP_PS_NMADD:
    case PPC_OP_PS_NMSUB: case PPC_OP_PS_NEG: case PPC_OP_PS_ABS:
    case PPC_OP_PS_NABS: case PPC_OP_PS_MR:
    case PPC_OP_PS_SUM0: case PPC_OP_PS_SUM1:
    case PPC_OP_PS_MULS0: case PPC_OP_PS_MULS1:
    case PPC_OP_PS_MADDS0: case PPC_OP_PS_MADDS1:
    case PPC_OP_PS_MERGE00: case PPC_OP_PS_MERGE01:
    case PPC_OP_PS_MERGE10: case PPC_OP_PS_MERGE11:
    case PPC_OP_PS_CMPU0: case PPC_OP_PS_CMPO0:
    case PPC_OP_PS_CMPU1: case PPC_OP_PS_CMPO1: case PPC_OP_PS_SEL:
    case PPC_OP_LFS: case PPC_OP_LFSU: case PPC_OP_LFSX: case PPC_OP_LFSUX:
    case PPC_OP_LFD: case PPC_OP_LFDU: case PPC_OP_LFDX: case PPC_OP_LFDUX:
    case PPC_OP_STFS: case PPC_OP_STFSU: case PPC_OP_STFSX: case PPC_OP_STFSUX:
    case PPC_OP_STFD: case PPC_OP_STFDU: case PPC_OP_STFDX: case PPC_OP_STFDUX:
    case PPC_OP_STFIWX:
    case PPC_OP_PSQ_L: case PPC_OP_PSQ_LU: case PPC_OP_PSQ_LX: case PPC_OP_PSQ_LUX:
    case PPC_OP_PSQ_ST: case PPC_OP_PSQ_STU: case PPC_OP_PSQ_STX: case PPC_OP_PSQ_STUX:
        return true;
    default:
        return false;
    }
}

static void emit_instruction_with_range(FILE* out, const PPCInst* inst,
                                        u32 func_start, u32 func_end,
                                        u32 cycle_charge) {
    char disasm[64];
    ppc_disasm(disasm, sizeof(disasm), inst);
    fprintf(out, "    // %08X: %s\n", inst->address, disasm);

    if (inst->embedded_data) {
        fprintf(out, "    // embedded data\n\n");
        return;
    }

    /* Lazy-FPU trap (see ppc_op_is_fp above). SRR0 = this instruction so it
     * re-executes after the OS handler enables FP and rfi's — the exact
     * hardware restart semantic the lazy context switch depends on. The
     * helper takes cia explicitly and the dispatcher resumes at the vector,
     * so no ctx->pc pre-stamp is needed (same reasoning as the Phase B/C
     * pc-pure whitelist). */
    if (ppc_op_is_fp(inst->op)) {
        fprintf(out, "    if (!(ctx->msr & 0x2000u)) {\n");
        fprintf(out, "        ppc_fp_unavailable(ctx, 0x%08Xu);\n", inst->address);
        fprintf(out, "        dr_cycles += %uu; DR_RET();\n", cycle_charge);
        fprintf(out, "    }\n");
    }

    switch (inst->op) {
    case PPC_OP_MULLI:
        fprintf(out, "    ctx->gpr[%u] = (u32)((s64)(s32)ctx->gpr[%u] * (s64)(s32)%d);\n",
                inst->rD, inst->rA, (int)inst->simm);
        break;

    case PPC_OP_SUBFIC:
        fprintf(out, "    {\n");
        fprintf(out, "        u64 res = (u64)(u32)(s32)(%d) + (u64)(~ctx->gpr[%u]) + 1u;\n",
                (int)inst->simm, inst->rA);
        fprintf(out, "        ctx->gpr[%u] = (u32)res;\n", inst->rD);
        fprintf(out, "        ctx->xer = (ctx->xer & ~0x20000000u) | (((u32)(res >> 32) & 1u) << 29);\n");
        fprintf(out, "    }\n");
        break;

    case PPC_OP_ADDI:
        if (inst->rA == 0) {
            fprintf(out, "    ctx->gpr[%u] = (u32)(s32)(%d);\n",
                    inst->rD, (int)inst->simm);
        } else {
            fprintf(out, "    ctx->gpr[%u] = ctx->gpr[%u] + (u32)(s32)(%d);\n",
                    inst->rD, inst->rA, (int)inst->simm);
        }
        break;

    case PPC_OP_ADDIC:
    case PPC_OP_ADDIC_DOT:
        fprintf(out, "    {\n");
        fprintf(out, "        u64 a = ctx->gpr[%u];\n", inst->rA);
        fprintf(out, "        u64 b = (u32)(s32)(%d);\n", (int)inst->simm);
        fprintf(out, "        u64 res = a + b;\n");
        fprintf(out, "        ctx->gpr[%u] = (u32)res;\n", inst->rD);
        fprintf(out, "        ctx->xer = (ctx->xer & ~0x20000000u) | (((u32)(res >> 32) & 1u) << 29);\n");
        if (inst->op == PPC_OP_ADDIC_DOT) {
            emit_set_cr0_from_gpr(out, inst->rD);
        }
        fprintf(out, "    }\n");
        break;

    case PPC_OP_ADDIS:
        if (inst->rA == 0) {
            fprintf(out, "    ctx->gpr[%u] = ((u32)(s32)(%d) << 16);\n",
                    inst->rD, (int)inst->simm);
        } else {
            fprintf(out, "    ctx->gpr[%u] = ctx->gpr[%u] + ((u32)(s32)(%d) << 16);\n",
                    inst->rD, inst->rA, (int)inst->simm);
        }
        break;

    case PPC_OP_CMPI:
        {
            char rhs[32];
            snprintf(rhs, sizeof(rhs), "%d", (int)inst->simm);
            char lhs[32];
            snprintf(lhs, sizeof(lhs), "ctx->gpr[%u]", inst->rA);
            emit_compare_s32(out, inst->crfD, lhs, rhs);
        }
        break;

    case PPC_OP_CMPLI:
        {
            char rhs[32];
            snprintf(rhs, sizeof(rhs), "0x%04Xu", inst->uimm);
            char lhs[32];
            snprintf(lhs, sizeof(lhs), "ctx->gpr[%u]", inst->rA);
            emit_compare_u32(out, inst->crfD, lhs, rhs);
        }
        break;

    case PPC_OP_CMP:
        {
            char lhs[32], rhs[32];
            snprintf(lhs, sizeof(lhs), "ctx->gpr[%u]", inst->rA);
            snprintf(rhs, sizeof(rhs), "ctx->gpr[%u]", inst->rB);
            emit_compare_s32(out, inst->crfD, lhs, rhs);
        }
        break;

    case PPC_OP_CMPL:
        {
            char lhs[32], rhs[32];
            snprintf(lhs, sizeof(lhs), "ctx->gpr[%u]", inst->rA);
            snprintf(rhs, sizeof(rhs), "ctx->gpr[%u]", inst->rB);
            emit_compare_u32(out, inst->crfD, lhs, rhs);
        }
        break;

    case PPC_OP_ORI:
        if (inst->rS == 0 && inst->rA == 0 && inst->uimm == 0) {
            fprintf(out, "    // nop\n");
        } else {
            fprintf(out, "    ctx->gpr[%u] = ctx->gpr[%u] | 0x%04Xu;\n",
                    inst->rA, inst->rS, inst->uimm);
        }
        break;

    case PPC_OP_ORIS:
        fprintf(out, "    ctx->gpr[%u] = ctx->gpr[%u] | (0x%04Xu << 16);\n",
                inst->rA, inst->rS, inst->uimm);
        break;

    case PPC_OP_XORI:
        fprintf(out, "    ctx->gpr[%u] = ctx->gpr[%u] ^ 0x%04Xu;\n",
                inst->rA, inst->rS, inst->uimm);
        break;

    case PPC_OP_XORIS:
        fprintf(out, "    ctx->gpr[%u] = ctx->gpr[%u] ^ (0x%04Xu << 16);\n",
                inst->rA, inst->rS, inst->uimm);
        break;

    case PPC_OP_ANDI:
        fprintf(out, "    {\n");
        fprintf(out, "        ctx->gpr[%u] = ctx->gpr[%u] & 0x%04Xu;\n",
                inst->rA, inst->rS, inst->uimm);
        emit_set_cr0_from_gpr(out, inst->rA);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_ANDIS:
        fprintf(out, "    {\n");
        fprintf(out, "        ctx->gpr[%u] = ctx->gpr[%u] & (0x%04Xu << 16);\n",
                inst->rA, inst->rS, inst->uimm);
        emit_set_cr0_from_gpr(out, inst->rA);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_ADD:
    case PPC_OP_ADDO:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 a = ctx->gpr[%u];\n", inst->rA);
        fprintf(out, "        u32 b = ctx->gpr[%u];\n", inst->rB);
        fprintf(out, "        u32 res = a + b;\n");
        fprintf(out, "        ctx->gpr[%u] = res;\n", inst->rD);
        if (inst->oe)
            fprintf(out, "        ppc_set_xer_ov(ctx, ppc_add_overflowed(a, b, res));\n");
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_ADDC:
    case PPC_OP_ADDCO:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 a = ctx->gpr[%u];\n", inst->rA);
        fprintf(out, "        u32 b = ctx->gpr[%u];\n", inst->rB);
        fprintf(out, "        u64 wide = (u64)a + (u64)b;\n");
        fprintf(out, "        u32 res = (u32)wide;\n");
        fprintf(out, "        ctx->gpr[%u] = res;\n", inst->rD);
        fprintf(out, "        ctx->xer = (ctx->xer & ~0x20000000u) | (((u32)(wide >> 32) & 1u) << 29);\n");
        if (inst->oe)
            fprintf(out, "        ppc_set_xer_ov(ctx, ppc_add_overflowed(a, b, res));\n");
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_ADDE:
    case PPC_OP_ADDEO:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 carry = (ctx->xer >> 29) & 1u;\n");
        fprintf(out, "        u32 a = ctx->gpr[%u];\n", inst->rA);
        fprintf(out, "        u32 b = ctx->gpr[%u];\n", inst->rB);
        fprintf(out, "        u64 wide = (u64)a + (u64)b + carry;\n");
        fprintf(out, "        u32 res = (u32)wide;\n");
        fprintf(out, "        ctx->gpr[%u] = res;\n", inst->rD);
        fprintf(out, "        ctx->xer = (ctx->xer & ~0x20000000u) | (((u32)(wide >> 32) & 1u) << 29);\n");
        if (inst->oe)
            fprintf(out, "        ppc_set_xer_ov(ctx, ppc_add_overflowed(a, b, res));\n");
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_ADDME:
    case PPC_OP_ADDMEO:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 input = ctx->gpr[%u];\n", inst->rA);
        fprintf(out, "        u32 carry = (ctx->xer >> 29) & 1u;\n");
        fprintf(out, "        u64 res = (u64)input + 0xFFFFFFFFull + carry;\n");
        fprintf(out, "        ctx->gpr[%u] = (u32)res;\n", inst->rD);
        fprintf(out, "        ctx->xer = (ctx->xer & ~0x20000000u) | ((res >> 32) ? 0x20000000u : 0u);\n");
        if (inst->oe)
            fprintf(out, "        ppc_set_xer_ov(ctx, ppc_add_overflowed(input, 0xFFFFFFFFu, (u32)res));\n");
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_ADDZE:
    case PPC_OP_ADDZEO:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 a = ctx->gpr[%u];\n", inst->rA);
        fprintf(out, "        u64 wide = (u64)a + ((ctx->xer >> 29) & 1u);\n");
        fprintf(out, "        u32 res = (u32)wide;\n");
        fprintf(out, "        ctx->gpr[%u] = res;\n", inst->rD);
        fprintf(out, "        ctx->xer = (ctx->xer & ~0x20000000u) | (((u32)(wide >> 32) & 1u) << 29);\n");
        if (inst->oe)
            fprintf(out, "        ppc_set_xer_ov(ctx, ppc_add_overflowed(a, 0u, res));\n");
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_SUBF:
    case PPC_OP_SUBFO:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 a = ~ctx->gpr[%u];\n", inst->rA);
        fprintf(out, "        u32 b = ctx->gpr[%u];\n", inst->rB);
        fprintf(out, "        u32 res = a + b + 1u;\n");
        fprintf(out, "        ctx->gpr[%u] = res;\n", inst->rD);
        if (inst->oe)
            fprintf(out, "        ppc_set_xer_ov(ctx, ppc_add_overflowed(a, b, res));\n");
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_SUBFC:
    case PPC_OP_SUBFCO:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 a = ~ctx->gpr[%u];\n", inst->rA);
        fprintf(out, "        u32 b = ctx->gpr[%u];\n", inst->rB);
        fprintf(out, "        u64 wide = (u64)b + (u64)a + 1u;\n");
        fprintf(out, "        u32 res = (u32)wide;\n");
        fprintf(out, "        ctx->gpr[%u] = res;\n", inst->rD);
        fprintf(out, "        ctx->xer = (ctx->xer & ~0x20000000u) | (((u32)(wide >> 32) & 1u) << 29);\n");
        if (inst->oe)
            fprintf(out, "        ppc_set_xer_ov(ctx, ppc_add_overflowed(a, b, res));\n");
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_SUBFE:
    case PPC_OP_SUBFEO:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 a = ~ctx->gpr[%u];\n", inst->rA);
        fprintf(out, "        u32 b = ctx->gpr[%u];\n", inst->rB);
        fprintf(out, "        u32 carry = (ctx->xer >> 29) & 1u;\n");
        fprintf(out, "        u64 wide = (u64)a + (u64)b + carry;\n");
        fprintf(out, "        u32 res = (u32)wide;\n");
        fprintf(out, "        ctx->gpr[%u] = res;\n", inst->rD);
        fprintf(out, "        ctx->xer = (ctx->xer & ~0x20000000u) | (((u32)(wide >> 32) & 1u) << 29);\n");
        if (inst->oe)
            fprintf(out, "        ppc_set_xer_ov(ctx, ppc_add_overflowed(a, b, res));\n");
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_SUBFME:
    case PPC_OP_SUBFMEO:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 input = ~ctx->gpr[%u];\n", inst->rA);
        fprintf(out, "        u32 carry = (ctx->xer >> 29) & 1u;\n");
        fprintf(out, "        u64 res = (u64)input + 0xFFFFFFFFull + carry;\n");
        fprintf(out, "        ctx->gpr[%u] = (u32)res;\n", inst->rD);
        fprintf(out, "        ctx->xer = (ctx->xer & ~0x20000000u) | ((res >> 32) ? 0x20000000u : 0u);\n");
        if (inst->oe)
            fprintf(out, "        ppc_set_xer_ov(ctx, ppc_add_overflowed(input, 0xFFFFFFFFu, (u32)res));\n");
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_SUBFZE:
    case PPC_OP_SUBFZEO:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 a = ~ctx->gpr[%u];\n", inst->rA);
        fprintf(out, "        u64 wide = (u64)a + ((ctx->xer >> 29) & 1u);\n");
        fprintf(out, "        u32 res = (u32)wide;\n");
        fprintf(out, "        ctx->gpr[%u] = res;\n", inst->rD);
        fprintf(out, "        ctx->xer = (ctx->xer & ~0x20000000u) | (((u32)(wide >> 32) & 1u) << 29);\n");
        if (inst->oe)
            fprintf(out, "        ppc_set_xer_ov(ctx, ppc_add_overflowed(a, 0u, res));\n");
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_NEG:
    case PPC_OP_NEGO:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 a = ctx->gpr[%u];\n", inst->rA);
        fprintf(out, "        ctx->gpr[%u] = (~a) + 1u;\n", inst->rD);
        if (inst->oe)
            fprintf(out, "        ppc_set_xer_ov(ctx, a == 0x80000000u);\n");
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_MULLW:
    case PPC_OP_MULLWO:
        fprintf(out, "    {\n");
        fprintf(out, "        s64 product = (s64)(s32)ctx->gpr[%u] * (s64)(s32)ctx->gpr[%u];\n",
                inst->rA, inst->rB);
        fprintf(out, "        ctx->gpr[%u] = (u32)product;\n", inst->rD);
        if (inst->oe)
            fprintf(out, "        ppc_set_xer_ov(ctx, product < -0x80000000ll || product > 0x7fffffffll);\n");
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_MULHW:
        fprintf(out, "    {\n");
        fprintf(out, "        s64 product = (s64)(s32)ctx->gpr[%u] * (s64)(s32)ctx->gpr[%u];\n",
                inst->rA, inst->rB);
        fprintf(out, "        ctx->gpr[%u] = (u32)(product >> 32);\n", inst->rD);
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_MULHWU:
        fprintf(out, "    {\n");
        fprintf(out, "        u64 product = (u64)ctx->gpr[%u] * (u64)ctx->gpr[%u];\n",
                inst->rA, inst->rB);
        fprintf(out, "        ctx->gpr[%u] = (u32)(product >> 32);\n", inst->rD);
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_DIVW:
    case PPC_OP_DIVWO:
        fprintf(out, "    {\n");
        fprintf(out, "        s32 dividend = (s32)ctx->gpr[%u];\n", inst->rA);
        fprintf(out, "        s32 divisor = (s32)ctx->gpr[%u];\n", inst->rB);
        fprintf(out, "        bool ov = divisor == 0 || ((u32)dividend == 0x80000000u && divisor == -1);\n");
        fprintf(out, "        ctx->gpr[%u] = ov ? ((dividend < 0) ? 0xFFFFFFFFu : 0u) : (u32)(dividend / divisor);\n",
                inst->rD);
        if (inst->oe)
            fprintf(out, "        ppc_set_xer_ov(ctx, ov);\n");
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_DIVWU:
    case PPC_OP_DIVWUO:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 divisor = ctx->gpr[%u];\n", inst->rB);
        fprintf(out, "        ctx->gpr[%u] = divisor == 0 ? 0u : ctx->gpr[%u] / divisor;\n",
                inst->rD, inst->rA);
        if (inst->oe)
            fprintf(out, "        ppc_set_xer_ov(ctx, divisor == 0);\n");
        emit_record_if_needed(out, inst, inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_AND:
    case PPC_OP_ANDC:
    case PPC_OP_OR:
    case PPC_OP_ORC:
    case PPC_OP_XOR:
    case PPC_OP_NAND:
    case PPC_OP_NOR:
    case PPC_OP_EQV: {
        const char* expr = NULL;
        switch (inst->op) {
        case PPC_OP_AND:  expr = "ctx->gpr[%u] & ctx->gpr[%u]"; break;
        case PPC_OP_ANDC: expr = "ctx->gpr[%u] & ~ctx->gpr[%u]"; break;
        case PPC_OP_OR:   expr = "ctx->gpr[%u] | ctx->gpr[%u]"; break;
        case PPC_OP_ORC:  expr = "ctx->gpr[%u] | ~ctx->gpr[%u]"; break;
        case PPC_OP_XOR:  expr = "ctx->gpr[%u] ^ ctx->gpr[%u]"; break;
        case PPC_OP_NAND: expr = "~(ctx->gpr[%u] & ctx->gpr[%u])"; break;
        case PPC_OP_NOR:  expr = "~(ctx->gpr[%u] | ctx->gpr[%u])"; break;
        default:          expr = "~(ctx->gpr[%u] ^ ctx->gpr[%u])"; break;
        }
        fprintf(out, "    {\n");
        fprintf(out, "        ctx->gpr[%u] = ", inst->rA);
        fprintf(out, expr, inst->rS, inst->rB);
        fprintf(out, ";\n");
        emit_record_if_needed(out, inst, inst->rA);
        fprintf(out, "    }\n");
        break;
    }

    case PPC_OP_CNTLZW:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 v = ctx->gpr[%u];\n", inst->rS);
        fprintf(out, "        u32 n = 0;\n");
        fprintf(out, "        while (n < 32 && ((v & (0x80000000u >> n)) == 0)) n++;\n");
        fprintf(out, "        ctx->gpr[%u] = n;\n", inst->rA);
        emit_record_if_needed(out, inst, inst->rA);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_EXTSB:
        fprintf(out, "    {\n");
        fprintf(out, "        ctx->gpr[%u] = (u32)(s32)(s8)ctx->gpr[%u];\n",
                inst->rA, inst->rS);
        emit_record_if_needed(out, inst, inst->rA);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_EXTSH:
        fprintf(out, "    {\n");
        fprintf(out, "        ctx->gpr[%u] = (u32)(s32)(s16)ctx->gpr[%u];\n",
                inst->rA, inst->rS);
        emit_record_if_needed(out, inst, inst->rA);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_SLW:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 sh = ctx->gpr[%u] & 0x3Fu;\n", inst->rB);
        fprintf(out, "        ctx->gpr[%u] = sh > 31 ? 0u : (ctx->gpr[%u] << sh);\n",
                inst->rA, inst->rS);
        emit_record_if_needed(out, inst, inst->rA);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_SRW:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 sh = ctx->gpr[%u] & 0x3Fu;\n", inst->rB);
        fprintf(out, "        ctx->gpr[%u] = sh > 31 ? 0u : (ctx->gpr[%u] >> sh);\n",
                inst->rA, inst->rS);
        emit_record_if_needed(out, inst, inst->rA);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_SRAW:
    case PPC_OP_SRAWI:
        fprintf(out, "    {\n");
        if (inst->op == PPC_OP_SRAWI) {
            fprintf(out, "        u32 sh = %uu;\n", inst->sh);
        } else {
            fprintf(out, "        u32 sh = ctx->gpr[%u] & 0x3Fu;\n", inst->rB);
        }
        fprintf(out, "        u32 value = ctx->gpr[%u];\n", inst->rS);
        fprintf(out, "        bool ca = false;\n");
        fprintf(out, "        if (sh == 0) {\n");
        fprintf(out, "            ctx->gpr[%u] = value;\n", inst->rA);
        fprintf(out, "        } else if (sh > 31) {\n");
        fprintf(out, "            ctx->gpr[%u] = (value & 0x80000000u) ? 0xFFFFFFFFu : 0u;\n", inst->rA);
        fprintf(out, "            ca = (value & 0x80000000u) != 0;\n");
        fprintf(out, "        } else {\n");
        fprintf(out, "            ctx->gpr[%u] = (u32)((s32)value >> sh);\n", inst->rA);
        fprintf(out, "            ca = (value & 0x80000000u) && ((value << (32u - sh)) != 0);\n");
        fprintf(out, "        }\n");
        fprintf(out, "        ctx->xer = (ctx->xer & ~0x20000000u) | (ca ? 0x20000000u : 0u);\n");
        emit_record_if_needed(out, inst, inst->rA);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_RLWINM:
        {
            u32 mask = ppc_mask32(inst->mb, inst->me);
            fprintf(out, "    {\n");
            fprintf(out, "        ctx->gpr[%u] = dolrecomp_rotl32(ctx->gpr[%u], %uu) & 0x%08Xu;\n",
                    inst->rA, inst->rS, inst->sh, mask);
            emit_record_if_needed(out, inst, inst->rA);
            fprintf(out, "    }\n");
        }
        break;

    case PPC_OP_RLWNM:
        {
            u32 mask = ppc_mask32(inst->mb, inst->me);
            fprintf(out, "    {\n");
            fprintf(out, "        ctx->gpr[%u] = dolrecomp_rotl32(ctx->gpr[%u], ctx->gpr[%u]) & 0x%08Xu;\n",
                    inst->rA, inst->rS, inst->rB, mask);
            emit_record_if_needed(out, inst, inst->rA);
            fprintf(out, "    }\n");
        }
        break;

    case PPC_OP_RLWIMI:
        {
            u32 mask = ppc_mask32(inst->mb, inst->me);
            fprintf(out, "    {\n");
            fprintf(out, "        u32 rot = dolrecomp_rotl32(ctx->gpr[%u], %uu);\n",
                    inst->rS, inst->sh);
            fprintf(out, "        ctx->gpr[%u] = (ctx->gpr[%u] & ~0x%08Xu) | (rot & 0x%08Xu);\n",
                    inst->rA, inst->rA, mask, mask);
            emit_record_if_needed(out, inst, inst->rA);
            fprintf(out, "    }\n");
        }
        break;

    case PPC_OP_FADDS:
        fprintf(out, "    ctx->fpr[%u] = (f64)(f32)(ctx->fpr[%u] + ctx->fpr[%u]);\n",
                inst->rD, inst->rA, inst->rB);
        break;

    case PPC_OP_FSUBS:
        fprintf(out, "    ctx->fpr[%u] = (f64)(f32)(ctx->fpr[%u] - ctx->fpr[%u]);\n",
                inst->rD, inst->rA, inst->rB);
        break;

    case PPC_OP_FMULS:
        fprintf(out, "    ctx->fpr[%u] = (f64)(f32)(ctx->fpr[%u] * ctx->fpr[%u]);\n",
                inst->rD, inst->rA, inst->rC);
        break;

    case PPC_OP_FDIVS:
        fprintf(out, "    ctx->fpr[%u] = (f64)(f32)(ctx->fpr[%u] / ctx->fpr[%u]);\n",
                inst->rD, inst->rA, inst->rB);
        break;

    case PPC_OP_FRES:
        fprintf(out, "    { f64 result; if (ppc_fres(ctx, ctx->fpr[%u], &result)) ctx->fpr[%u] = ctx->ps1[%u] = result; }\n",
                inst->rB, inst->rD, inst->rD);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_FMADDS:
    case PPC_OP_FMSUBS:
    case PPC_OP_FNMADDS:
    case PPC_OP_FNMSUBS: {
        const bool sub = inst->op == PPC_OP_FMSUBS || inst->op == PPC_OP_FNMSUBS;
        const bool neg = inst->op == PPC_OP_FNMADDS || inst->op == PPC_OP_FNMSUBS;
        fprintf(out, "    {\n");
        fprintf(out, "        f64 result;\n");
        fprintf(out, "        if (ppc_fma(ctx, ctx->fpr[%u], ctx->fpr[%u], ctx->fpr[%u], true, %s, %s, &result))\n",
                inst->rA, inst->rC, inst->rB, sub ? "true" : "false", neg ? "true" : "false");
        fprintf(out, "            ctx->fpr[%u] = ctx->ps1[%u] = result;\n", inst->rD, inst->rD);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        fprintf(out, "    }\n");
        break;
    }

    case PPC_OP_FADD:
        fprintf(out, "    ctx->fpr[%u] = ctx->fpr[%u] + ctx->fpr[%u];\n",
                inst->rD, inst->rA, inst->rB);
        break;

    case PPC_OP_FSUB:
        fprintf(out, "    ctx->fpr[%u] = ctx->fpr[%u] - ctx->fpr[%u];\n",
                inst->rD, inst->rA, inst->rB);
        break;

    case PPC_OP_FMUL:
        fprintf(out, "    ctx->fpr[%u] = ctx->fpr[%u] * ctx->fpr[%u];\n",
                inst->rD, inst->rA, inst->rC);
        break;

    case PPC_OP_FDIV:
        fprintf(out, "    ctx->fpr[%u] = ctx->fpr[%u] / ctx->fpr[%u];\n",
                inst->rD, inst->rA, inst->rB);
        break;

    case PPC_OP_FRSQRTE:
        fprintf(out, "    { f64 result; if (ppc_frsqrte(ctx, ctx->fpr[%u], &result)) ctx->fpr[%u] = result; }\n",
                inst->rB, inst->rD);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_FMADD:
    case PPC_OP_FMSUB:
    case PPC_OP_FNMADD:
    case PPC_OP_FNMSUB: {
        const bool sub = inst->op == PPC_OP_FMSUB || inst->op == PPC_OP_FNMSUB;
        const bool neg = inst->op == PPC_OP_FNMADD || inst->op == PPC_OP_FNMSUB;
        fprintf(out, "    {\n");
        fprintf(out, "        f64 result;\n");
        fprintf(out, "        if (ppc_fma(ctx, ctx->fpr[%u], ctx->fpr[%u], ctx->fpr[%u], false, %s, %s, &result))\n",
                inst->rA, inst->rC, inst->rB, sub ? "true" : "false", neg ? "true" : "false");
        fprintf(out, "            ctx->fpr[%u] = result;\n", inst->rD);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        fprintf(out, "    }\n");
        break;
    }

    case PPC_OP_FCTIW:
    case PPC_OP_FCTIWZ:
        fprintf(out, "    { u64 result; if (ppc_fctiw(ctx, ctx->fpr[%u], %s, &result)) ctx->fpr[%u] = dolrecomp_f64_from_bits(result); }\n",
                inst->rB, inst->op == PPC_OP_FCTIWZ ? "true" : "false", inst->rD);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_FMR:
        fprintf(out, "    ctx->fpr[%u] = ctx->fpr[%u];\n", inst->rD, inst->rB);
        break;

    case PPC_OP_FNEG:
        fprintf(out, "    ctx->fpr[%u] = dolrecomp_f64_from_bits(dolrecomp_f64_to_bits(ctx->fpr[%u]) ^ 0x8000000000000000ull);\n",
                inst->rD, inst->rB);
        break;

    case PPC_OP_FABS:
        fprintf(out, "    ctx->fpr[%u] = dolrecomp_f64_from_bits(dolrecomp_f64_to_bits(ctx->fpr[%u]) & 0x7FFFFFFFFFFFFFFFull);\n",
                inst->rD, inst->rB);
        break;

    case PPC_OP_FNABS:
        fprintf(out, "    ctx->fpr[%u] = dolrecomp_f64_from_bits(dolrecomp_f64_to_bits(ctx->fpr[%u]) | 0x8000000000000000ull);\n",
                inst->rD, inst->rB);
        break;

    case PPC_OP_FRSP:
        fprintf(out, "    ctx->fpr[%u] = (f64)(f32)ctx->fpr[%u];\n", inst->rD, inst->rB);
        break;

    case PPC_OP_FSEL:
        fprintf(out, "    {\n");
        fprintf(out, "        ctx->fpr[%u] = (ctx->fpr[%u] >= 0.0) ? ctx->fpr[%u] : ctx->fpr[%u];\n",
                inst->rD, inst->rA, inst->rC, inst->rB);
        if (inst->rc) {
            emit_set_cr1_from_fpscr(out);
        }
        fprintf(out, "    }\n");
        break;

    case PPC_OP_MTFSB0:
    case PPC_OP_MTFSB1:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 mask = 0x80000000u >> %u;\n", inst->rD);
        if (inst->op == PPC_OP_MTFSB0) {
            fprintf(out, "        if (%u != 1 && %u != 2) ctx->fpscr &= ~mask;\n",
                    inst->rD, inst->rD);
        } else {
            fprintf(out, "        if (%u != 1 && %u != 2) ctx->fpscr |= mask;\n",
                    inst->rD, inst->rD);
        }
        if (inst->rc) {
            emit_set_cr1_from_fpscr(out);
        }
        fprintf(out, "    }\n");
        break;

    case PPC_OP_MFFS:
        fprintf(out, "    ctx->fpr[%u] = dolrecomp_f64_from_bits(0xFFF8000000000000ull | ctx->fpscr);\n", inst->rD);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_MCRFS: {
        u32 shift = cr_field_shift(inst->crfS);
        u32 dst_shift = cr_field_shift(inst->crfD);
        fprintf(out, "    {\n");
        fprintf(out, "        u32 field = (ctx->fpscr >> %u) & 0xFu;\n", shift);
        fprintf(out, "        ctx->fpscr &= ~((0xFu << %u) & 0x83F80700u);\n", shift);
        fprintf(out, "        ppc_fpscr_updated(ctx);\n");
        fprintf(out, "        ctx->cr = (ctx->cr & ~(0xFu << %u)) | (field << %u);\n", dst_shift, dst_shift);
        fprintf(out, "    }\n");
        break;
    }

    case PPC_OP_MTFSFI: {
        u32 shift = cr_field_shift(inst->crfD);
        fprintf(out, "    ctx->fpscr = (ctx->fpscr & ~(0xFu << %u)) | (0x%Xu << %u);\n",
                shift, inst->imm, shift);
        fprintf(out, "    ppc_fpscr_updated(ctx);\n");
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;
    }

    case PPC_OP_MTFSF:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 mask = 0;\n");
        fprintf(out, "        for (u32 i = 0; i < 8; i++) if (0x%02Xu & (1u << i)) mask |= 0xFu << (i * 4);\n", inst->fm);
        fprintf(out, "        u32 source = (u32)dolrecomp_f64_to_bits(ctx->fpr[%u]);\n", inst->rB);
        fprintf(out, "        ctx->fpscr = (ctx->fpscr & ~mask) | (source & mask);\n");
        fprintf(out, "        ppc_fpscr_updated(ctx);\n");
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_PS_ADD:
        fprintf(out, "    {\n");
        fprintf(out, "        ctx->fpr[%u] = dolrecomp_ps_round((f32)ctx->fpr[%u] + (f32)ctx->fpr[%u]);\n",
                inst->rD, inst->rA, inst->rB);
        fprintf(out, "        ctx->ps1[%u] = dolrecomp_ps_round((f32)ctx->ps1[%u] + (f32)ctx->ps1[%u]);\n",
                inst->rD, inst->rA, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_PS_SUB:
        fprintf(out, "    {\n");
        fprintf(out, "        ctx->fpr[%u] = dolrecomp_ps_round((f32)ctx->fpr[%u] - (f32)ctx->fpr[%u]);\n",
                inst->rD, inst->rA, inst->rB);
        fprintf(out, "        ctx->ps1[%u] = dolrecomp_ps_round((f32)ctx->ps1[%u] - (f32)ctx->ps1[%u]);\n",
                inst->rD, inst->rA, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_PS_MUL:
        fprintf(out, "    {\n");
        fprintf(out, "        ctx->fpr[%u] = dolrecomp_ps_round((f32)ctx->fpr[%u] * (f32)ctx->fpr[%u]);\n",
                inst->rD, inst->rA, inst->rC);
        fprintf(out, "        ctx->ps1[%u] = dolrecomp_ps_round((f32)ctx->ps1[%u] * (f32)ctx->ps1[%u]);\n",
                inst->rD, inst->rA, inst->rC);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_PS_DIV:
        fprintf(out, "    {\n");
        fprintf(out, "        ctx->fpr[%u] = dolrecomp_ps_round((f32)ctx->fpr[%u] / (f32)ctx->fpr[%u]);\n",
                inst->rD, inst->rA, inst->rB);
        fprintf(out, "        ctx->ps1[%u] = dolrecomp_ps_round((f32)ctx->ps1[%u] / (f32)ctx->ps1[%u]);\n",
                inst->rD, inst->rA, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_PS_RES:
        fprintf(out, "    { f64 a, b; ppc_ps_res(ctx, ctx->fpr[%u], ctx->ps1[%u], &a, &b); ctx->fpr[%u] = dolrecomp_ps_round(a); ctx->ps1[%u] = dolrecomp_ps_round(b); }\n",
                inst->rB, inst->rB, inst->rD, inst->rD);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_RSQRTE:
        fprintf(out, "    { f64 a, b; ppc_ps_rsqrte(ctx, ctx->fpr[%u], ctx->ps1[%u], &a, &b); ctx->fpr[%u] = dolrecomp_ps_round(a); ctx->ps1[%u] = dolrecomp_ps_round(b); }\n",
                inst->rB, inst->rB, inst->rD, inst->rD);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_MADD:
    case PPC_OP_PS_MSUB:
    case PPC_OP_PS_NMADD:
    case PPC_OP_PS_NMSUB:
        fprintf(out, "    {\n");
        fprintf(out, "        f32 ps0 = (f32)ctx->fpr[%u] * (f32)ctx->fpr[%u];\n",
                inst->rA, inst->rC);
        fprintf(out, "        f32 ps1 = (f32)ctx->ps1[%u] * (f32)ctx->ps1[%u];\n",
                inst->rA, inst->rC);
        if (inst->op == PPC_OP_PS_MADD || inst->op == PPC_OP_PS_NMADD) {
            fprintf(out, "        ps0 += (f32)ctx->fpr[%u];\n", inst->rB);
            fprintf(out, "        ps1 += (f32)ctx->ps1[%u];\n", inst->rB);
        } else {
            fprintf(out, "        ps0 -= (f32)ctx->fpr[%u];\n", inst->rB);
            fprintf(out, "        ps1 -= (f32)ctx->ps1[%u];\n", inst->rB);
        }
        if (inst->op == PPC_OP_PS_NMADD || inst->op == PPC_OP_PS_NMSUB) {
            fprintf(out, "        ps0 = -ps0;\n");
            fprintf(out, "        ps1 = -ps1;\n");
        }
        fprintf(out, "        ctx->fpr[%u] = dolrecomp_ps_round(ps0);\n", inst->rD);
        fprintf(out, "        ctx->ps1[%u] = dolrecomp_ps_round(ps1);\n", inst->rD);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_PS_NEG:
        fprintf(out, "    ctx->fpr[%u] = dolrecomp_ps_from_bits(dolrecomp_ps_to_bits(ctx->fpr[%u]) ^ 0x80000000u);\n",
                inst->rD, inst->rB);
        fprintf(out, "    ctx->ps1[%u] = dolrecomp_ps_from_bits(dolrecomp_ps_to_bits(ctx->ps1[%u]) ^ 0x80000000u);\n",
                inst->rD, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_ABS:
        fprintf(out, "    ctx->fpr[%u] = dolrecomp_ps_from_bits(dolrecomp_ps_to_bits(ctx->fpr[%u]) & 0x7FFFFFFFu);\n",
                inst->rD, inst->rB);
        fprintf(out, "    ctx->ps1[%u] = dolrecomp_ps_from_bits(dolrecomp_ps_to_bits(ctx->ps1[%u]) & 0x7FFFFFFFu);\n",
                inst->rD, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_NABS:
        fprintf(out, "    ctx->fpr[%u] = dolrecomp_ps_from_bits(dolrecomp_ps_to_bits(ctx->fpr[%u]) | 0x80000000u);\n",
                inst->rD, inst->rB);
        fprintf(out, "    ctx->ps1[%u] = dolrecomp_ps_from_bits(dolrecomp_ps_to_bits(ctx->ps1[%u]) | 0x80000000u);\n",
                inst->rD, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_MR:
        fprintf(out, "    ctx->fpr[%u] = ctx->fpr[%u];\n", inst->rD, inst->rB);
        fprintf(out, "    ctx->ps1[%u] = ctx->ps1[%u];\n", inst->rD, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_SUM0:
        fprintf(out, "    ctx->fpr[%u] = dolrecomp_ps_round((f32)ctx->fpr[%u] + (f32)ctx->ps1[%u]);\n",
                inst->rD, inst->rA, inst->rB);
        fprintf(out, "    ctx->ps1[%u] = dolrecomp_ps_round(ctx->ps1[%u]);\n",
                inst->rD, inst->rC);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_SUM1:
        fprintf(out, "    ctx->fpr[%u] = dolrecomp_ps_round(ctx->fpr[%u]);\n",
                inst->rD, inst->rC);
        fprintf(out, "    ctx->ps1[%u] = dolrecomp_ps_round((f32)ctx->fpr[%u] + (f32)ctx->ps1[%u]);\n",
                inst->rD, inst->rA, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_MULS0:
        fprintf(out, "    ctx->fpr[%u] = dolrecomp_ps_round((f32)ctx->fpr[%u] * (f32)ctx->fpr[%u]);\n",
                inst->rD, inst->rA, inst->rC);
        fprintf(out, "    ctx->ps1[%u] = dolrecomp_ps_round((f32)ctx->ps1[%u] * (f32)ctx->fpr[%u]);\n",
                inst->rD, inst->rA, inst->rC);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_MULS1:
        fprintf(out, "    ctx->fpr[%u] = dolrecomp_ps_round((f32)ctx->fpr[%u] * (f32)ctx->ps1[%u]);\n",
                inst->rD, inst->rA, inst->rC);
        fprintf(out, "    ctx->ps1[%u] = dolrecomp_ps_round((f32)ctx->ps1[%u] * (f32)ctx->ps1[%u]);\n",
                inst->rD, inst->rA, inst->rC);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_MADDS0:
        fprintf(out, "    ctx->fpr[%u] = dolrecomp_ps_round((f32)ctx->fpr[%u] * (f32)ctx->fpr[%u] + (f32)ctx->fpr[%u]);\n",
                inst->rD, inst->rA, inst->rC, inst->rB);
        fprintf(out, "    ctx->ps1[%u] = dolrecomp_ps_round((f32)ctx->ps1[%u] * (f32)ctx->fpr[%u] + (f32)ctx->ps1[%u]);\n",
                inst->rD, inst->rA, inst->rC, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_MADDS1:
        fprintf(out, "    ctx->fpr[%u] = dolrecomp_ps_round((f32)ctx->fpr[%u] * (f32)ctx->ps1[%u] + (f32)ctx->fpr[%u]);\n",
                inst->rD, inst->rA, inst->rC, inst->rB);
        fprintf(out, "    ctx->ps1[%u] = dolrecomp_ps_round((f32)ctx->ps1[%u] * (f32)ctx->ps1[%u] + (f32)ctx->ps1[%u]);\n",
                inst->rD, inst->rA, inst->rC, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_MERGE00:
        fprintf(out, "    ctx->fpr[%u] = dolrecomp_ps_round(ctx->fpr[%u]);\n", inst->rD, inst->rA);
        fprintf(out, "    ctx->ps1[%u] = dolrecomp_ps_round(ctx->fpr[%u]);\n", inst->rD, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_MERGE01:
        fprintf(out, "    ctx->fpr[%u] = dolrecomp_ps_round(ctx->fpr[%u]);\n", inst->rD, inst->rA);
        fprintf(out, "    ctx->ps1[%u] = dolrecomp_ps_round(ctx->ps1[%u]);\n", inst->rD, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_MERGE10:
        fprintf(out, "    ctx->fpr[%u] = dolrecomp_ps_round(ctx->ps1[%u]);\n", inst->rD, inst->rA);
        fprintf(out, "    ctx->ps1[%u] = dolrecomp_ps_round(ctx->fpr[%u]);\n", inst->rD, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_MERGE11:
        fprintf(out, "    ctx->fpr[%u] = dolrecomp_ps_round(ctx->ps1[%u]);\n", inst->rD, inst->rA);
        fprintf(out, "    ctx->ps1[%u] = dolrecomp_ps_round(ctx->ps1[%u]);\n", inst->rD, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_CMPU0:
    case PPC_OP_PS_CMPO0:
    case PPC_OP_PS_CMPU1:
    case PPC_OP_PS_CMPO1:
        fprintf(out, "    {\n");
        if (inst->op == PPC_OP_PS_CMPU0 || inst->op == PPC_OP_PS_CMPO0) {
            fprintf(out, "        f32 val_a = (f32)ctx->fpr[%u];\n", inst->rA);
            fprintf(out, "        f32 val_b = (f32)ctx->fpr[%u];\n", inst->rB);
        } else {
            fprintf(out, "        f32 val_a = (f32)ctx->ps1[%u];\n", inst->rA);
            fprintf(out, "        f32 val_b = (f32)ctx->ps1[%u];\n", inst->rB);
        }
        fprintf(out, "        u32 cr_bits = 0;\n");
        fprintf(out, "        if (val_a < val_b)       cr_bits = 0x8u;\n");
        fprintf(out, "        else if (val_a > val_b)  cr_bits = 0x4u;\n");
        fprintf(out, "        else if (val_a == val_b) cr_bits = 0x2u;\n");
        fprintf(out, "        else                     cr_bits = 0x1u;\n");
        fprintf(out, "        ctx->cr = (ctx->cr & ~(0xFu << %u)) | (cr_bits << %u);\n",
                cr_field_shift(inst->crfD), cr_field_shift(inst->crfD));
        fprintf(out, "    }\n");
        break;

    case PPC_OP_PS_SEL:
        fprintf(out, "    ctx->fpr[%u] = ((f32)ctx->fpr[%u] >= 0.0f) ? ctx->fpr[%u] : ctx->fpr[%u];\n",
                inst->rD, inst->rA, inst->rC, inst->rB);
        fprintf(out, "    ctx->ps1[%u] = ((f32)ctx->ps1[%u] >= 0.0f) ? ctx->ps1[%u] : ctx->ps1[%u];\n",
                inst->rD, inst->rA, inst->rC, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_FCMPU:
    case PPC_OP_FCMPO:
        emit_fcompare(out, inst);
        break;

    case PPC_OP_LWZ:  emit_load_fast(out, inst, 32, false, false); break;
    case PPC_OP_LWZU: emit_load_fast(out, inst, 32, false, true); break;
    case PPC_OP_LBZ:  emit_load_fast(out, inst, 8,  false, false); break;
    case PPC_OP_LBZU: emit_load_fast(out, inst, 8,  false, true); break;
    case PPC_OP_LHZ:  emit_load_fast(out, inst, 16, false, false); break;
    case PPC_OP_LHZU: emit_load_fast(out, inst, 16, false, true); break;
    case PPC_OP_LHA:  emit_load_fast(out, inst, 16, true,  false); break;
    case PPC_OP_LHAU: emit_load_fast(out, inst, 16, true,  true); break;

    case PPC_OP_LWZX:  emit_loadx_fast(out, inst, 32, false, false); break;
    case PPC_OP_LWZUX: emit_loadx_fast(out, inst, 32, false, true); break;
    case PPC_OP_LBZX:  emit_loadx_fast(out, inst, 8,  false, false); break;
    case PPC_OP_LBZUX: emit_loadx_fast(out, inst, 8,  false, true); break;
    case PPC_OP_LHZX:  emit_loadx_fast(out, inst, 16, false, false); break;
    case PPC_OP_LHZUX: emit_loadx_fast(out, inst, 16, false, true); break;
    case PPC_OP_LHAX:  emit_loadx_fast(out, inst, 16, true,  false); break;
    case PPC_OP_LHAUX: emit_loadx_fast(out, inst, 16, true,  true); break;

    /* Phase C: byte-reversed loads stay an unconditional slow call (not part
     * of the "plain" inlined class) but still gain cia; their mem_read* call
     * no longer needs ctx->pc pre-stamped (see ppc_op_is_pc_pure). */
    case PPC_OP_LWBRX:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 ea = ");
        emit_xform_ea(out, inst->rA, inst->rB, false);
        fprintf(out, ";\n");
        fprintf(out, "        ctx->gpr[%u] = bswap32(mem_read32(ctx, ea, 0x%08Xu));\n",
                inst->rD, inst->address);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_LHBRX:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 ea = ");
        emit_xform_ea(out, inst->rA, inst->rB, false);
        fprintf(out, ";\n");
        fprintf(out, "        ctx->gpr[%u] = bswap16(mem_read16(ctx, ea, 0x%08Xu));\n",
                inst->rD, inst->address);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_LFS:   emit_fload_fast(out, inst, true,  false); break;
    case PPC_OP_LFSU:  emit_fload_fast(out, inst, true,  true); break;
    case PPC_OP_LFD:   emit_fload_fast(out, inst, false, false); break;
    case PPC_OP_LFDU:  emit_fload_fast(out, inst, false, true); break;

    case PPC_OP_LFSX:  emit_floadx_fast(out, inst, true,  false); break;
    case PPC_OP_LFSUX: emit_floadx_fast(out, inst, true,  true); break;
    case PPC_OP_LFDX:  emit_floadx_fast(out, inst, false, false); break;
    case PPC_OP_LFDUX: emit_floadx_fast(out, inst, false, true); break;

    case PPC_OP_PSQ_L:   emit_psq_load(out, inst, false, false, cycle_charge); break;
    case PPC_OP_PSQ_LU:  emit_psq_load(out, inst, false, true, cycle_charge); break;
    case PPC_OP_PSQ_LX:  emit_psq_load(out, inst, true,  false, cycle_charge); break;
    case PPC_OP_PSQ_LUX: emit_psq_load(out, inst, true,  true, cycle_charge); break;

    case PPC_OP_STW:  emit_store_fast(out, inst, 32, false); break;
    case PPC_OP_STWU: emit_store_fast(out, inst, 32, true); break;
    case PPC_OP_STB:  emit_store_fast(out, inst, 8,  false); break;
    case PPC_OP_STBU: emit_store_fast(out, inst, 8,  true); break;
    case PPC_OP_STH:  emit_store_fast(out, inst, 16, false); break;
    case PPC_OP_STHU: emit_store_fast(out, inst, 16, true); break;

    case PPC_OP_STWX:  emit_storex_fast(out, inst, 32, false); break;
    case PPC_OP_STWUX: emit_storex_fast(out, inst, 32, true); break;
    case PPC_OP_STBX:  emit_storex_fast(out, inst, 8,  false); break;
    case PPC_OP_STBUX: emit_storex_fast(out, inst, 8,  true); break;
    case PPC_OP_STHX:  emit_storex_fast(out, inst, 16, false); break;
    case PPC_OP_STHUX: emit_storex_fast(out, inst, 16, true); break;

    case PPC_OP_STFS:   emit_fstore_fast(out, inst, true,  false); break;
    case PPC_OP_STFSU:  emit_fstore_fast(out, inst, true,  true); break;
    case PPC_OP_STFD:   emit_fstore_fast(out, inst, false, false); break;
    case PPC_OP_STFDU:  emit_fstore_fast(out, inst, false, true); break;

    case PPC_OP_STFSX:  emit_fstorex_fast(out, inst, true,  false); break;
    case PPC_OP_STFSUX: emit_fstorex_fast(out, inst, true,  true); break;
    case PPC_OP_STFDX:  emit_fstorex_fast(out, inst, false, false); break;
    case PPC_OP_STFDUX: emit_fstorex_fast(out, inst, false, true); break;

    case PPC_OP_PSQ_ST:   emit_psq_store(out, inst, false, false, cycle_charge); break;
    case PPC_OP_PSQ_STU:  emit_psq_store(out, inst, false, true, cycle_charge); break;
    case PPC_OP_PSQ_STX:  emit_psq_store(out, inst, true,  false, cycle_charge); break;
    case PPC_OP_PSQ_STUX: emit_psq_store(out, inst, true,  true, cycle_charge); break;

    case PPC_OP_STWBRX:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 ea = ");
        emit_xform_ea(out, inst->rA, inst->rB, false);
        fprintf(out, ";\n");
        fprintf(out, "        mem_write32(ctx, ea, bswap32(ctx->gpr[%u]), 0x%08Xu);\n",
                inst->rS, inst->address);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_STHBRX:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 ea = ");
        emit_xform_ea(out, inst->rA, inst->rB, false);
        fprintf(out, ";\n");
        fprintf(out, "        mem_write16(ctx, ea, bswap16((u16)ctx->gpr[%u]), 0x%08Xu);\n",
                inst->rS, inst->address);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_LSWI:
    case PPC_OP_LSWX: {
        u32 count = inst->op == PPC_OP_LSWI ? (inst->nb ? inst->nb : 32u) : 0u;
        fprintf(out, "    {\n");
        if (inst->op == PPC_OP_LSWX) {
            fprintf(out, "        u32 ea = ctx->gpr[%u];\n", inst->rB);
            if (inst->rA)
                fprintf(out, "        ea += ctx->gpr[%u];\n", inst->rA);
            fprintf(out, "        u32 count = ctx->xer & 0x7Fu;\n");
            fprintf(out, "        u32 reg_count = (count + 3u) / 4u;\n");
            fprintf(out, "        for (u32 r = 0; r < reg_count; r++) {\n");
            fprintf(out, "            u32 reg = (%uu + r) & 31u;\n", inst->rD);
            fprintf(out, "            if (reg == %uu || reg == %uu) {\n", inst->rA, inst->rB);
            fprintf(out, "                dr_cycles += %uu;\n", cycle_charge);
            fprintf(out, "                ppc_program_exception(ctx, PPC_PROGRAM_ILLEGAL, 0x%08Xu);\n",
                    inst->address);
            fprintf(out, "                DR_RET();\n");
            fprintf(out, "            }\n");
            fprintf(out, "        }\n");
        } else {
            if (inst->rA) fprintf(out, "        u32 ea = ctx->gpr[%u];\n", inst->rA);
            else fprintf(out, "        u32 ea = 0u;\n");
            fprintf(out, "        u32 count = %uu;\n", count);
        }
        fprintf(out, "        for (u32 n = 0; n < count; n++) {\n");
        fprintf(out, "            u32 reg = (%uu + n / 4u) & 31u;\n", inst->rD);
        fprintf(out, "            if ((n & 3u) == 0) ctx->gpr[reg] = 0;\n");
        fprintf(out, "            ctx->gpr[reg] |= (u32)mem_read8(ctx, ea + n, 0x%08Xu) << (24u - 8u * (n & 3u));\n",
                inst->address);
        fprintf(out, "        }\n");
        fprintf(out, "    }\n");
        break;
    }

    case PPC_OP_STSWI:
    case PPC_OP_STSWX: {
        u32 count = inst->op == PPC_OP_STSWI ? (inst->nb ? inst->nb : 32u) : 0u;
        fprintf(out, "    {\n");
        if (inst->op == PPC_OP_STSWX) {
            fprintf(out, "        u32 ea = ctx->gpr[%u]", inst->rB);
            if (inst->rA) fprintf(out, " + ctx->gpr[%u]", inst->rA);
            fprintf(out, ";\n        u32 count = ctx->xer & 0x7Fu;\n");
        } else {
            if (inst->rA) fprintf(out, "        u32 ea = ctx->gpr[%u];\n", inst->rA);
            else fprintf(out, "        u32 ea = 0u;\n");
            fprintf(out, "        u32 count = %uu;\n", count);
        }
        fprintf(out, "        for (u32 n = 0; n < count; n++) {\n");
        fprintf(out, "            u32 reg = (%uu + n / 4u) & 31u;\n", inst->rS);
        fprintf(out, "            u8 value = (u8)(ctx->gpr[reg] >> (24u - 8u * (n & 3u)));\n");
        fprintf(out, "            mem_write8(ctx, ea + n, value, 0x%08Xu);\n", inst->address);
        fprintf(out, "        }\n");
        fprintf(out, "    }\n");
        break;
    }

    case PPC_OP_LWARX:
        fprintf(out, "    {\n        u32 ea = ");
        emit_xform_ea(out, inst->rA, inst->rB, false);
        fprintf(out, ";\n        ctx->gpr[%u] = mem_read32(ctx, ea, 0x%08Xu);\n",
                inst->rD, inst->address);
        fprintf(out, "        ctx->reserve_addr = ea;\n        ctx->reserve_valid = true;\n    }\n");
        break;

    case PPC_OP_STWCX:
        fprintf(out, "    {\n        u32 ea = ");
        emit_xform_ea(out, inst->rA, inst->rB, false);
        fprintf(out, ";\n        bool success = ctx->reserve_valid;\n");
        fprintf(out, "        ctx->reserve_valid = false;\n");
        fprintf(out, "        if (success) mem_write32(ctx, ea, ctx->gpr[%u], 0x%08Xu);\n",
                inst->rS, inst->address);
        fprintf(out, "        ctx->cr = (ctx->cr & 0x0FFFFFFFu) | ((success ? 2u : 0u) << 28) | ((ctx->xer >> 3) & 0x10000000u);\n");
        fprintf(out, "    }\n");
        break;

    case PPC_OP_STFIWX:
        fprintf(out, "    {\n        u32 ea = ");
        emit_xform_ea(out, inst->rA, inst->rB, false);
        fprintf(out, ";\n        mem_write32(ctx, ea, (u32)dolrecomp_f64_to_bits(ctx->fpr[%u]), 0x%08Xu);\n    }\n",
                inst->rS, inst->address);
        break;

    case PPC_OP_DCBZ:
        emit_dcbz(out, inst);
        break;

    case PPC_OP_DCBZ_L:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 ea = ");
        emit_xform_ea(out, inst->rA, inst->rB, false);
        fprintf(out, ";\n");
        fprintf(out, "        ppc_dcbz_l(ctx, ea, 0x%08Xu);\n", inst->address);
        fprintf(out, "        if (ctx->exception) { dr_cycles += %uu; DR_RET(); }\n", cycle_charge);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_DCBST:
    case PPC_OP_DCBF:
    case PPC_OP_DCBTST:
    case PPC_OP_DCBT:
    case PPC_OP_DCBI:
    case PPC_OP_ICBI:
        fprintf(out, "    (void)ctx;\n");
        break;

    case PPC_OP_LMW:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 ea = ");
        emit_dform_ea(out, inst->rA, inst->simm, false);
        fprintf(out, ";\n");
        fprintf(out, "        for (u32 r = %u; r < 32; r++, ea += 4) ctx->gpr[r] = mem_read32(ctx, ea, 0x%08Xu);\n",
                inst->rD, inst->address);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_STMW:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 ea = ");
        emit_dform_ea(out, inst->rA, inst->simm, false);
        fprintf(out, ";\n");
        fprintf(out, "        for (u32 r = %u; r < 32; r++, ea += 4) mem_write32(ctx, ea, ctx->gpr[r], 0x%08Xu);\n",
                inst->rS, inst->address);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_B:
        fprintf(out, "    {\n");
        emit_direct_branch(out, inst,
                           branch_target_is_local(func_start, func_end, inst->branch_target),
                           false /* conditional: plain b never gets the deadline goto */,
                           cycle_charge, func_start, func_end);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_BC:
        fprintf(out, "    {\n");
        emit_branch_condition(out, inst->bo, inst->bi);
        fprintf(out, "        if (ctr_ok && cr_ok) {\n");
        emit_direct_branch(out, inst,
                           branch_target_is_local(func_start, func_end, inst->branch_target),
                           true /* conditional: eligible for the deadline goto (bdnz/bdz too) */,
                           cycle_charge, func_start, func_end);
        fprintf(out, "        }\n");
        fprintf(out, "    }\n");
        break;

    case PPC_OP_BCLR:
        /* is_blr_class=true: the canonical return instruction. Eligible for
         * the shadow-stack pop fast path -- see emit_dynamic_branch. */
        emit_dynamic_branch(out, inst, "ctx->lr & ~3u", cycle_charge, true);
        break;

    case PPC_OP_BCCTR:
        /* bcctr targets CTR, not LR -- computed jump / tail-call idiom, not
         * architecturally a "return". Deliberately NOT wired to the shadow
         * stack: matching by coincidental ctx->ctr == some pushed return PC
         * would be semantically bogus (bcctr never pops a call frame on real
         * hardware), so it keeps the original always-return codegen. */
        emit_dynamic_branch(out, inst, "ctx->ctr & ~3u", cycle_charge, false);
        break;

    case PPC_OP_TWI:
        fprintf(out, "    if (ppc_trap_condition(%uu, ctx->gpr[%u], (u32)(s32)%d)) {\n",
                inst->to, inst->rA, (int)inst->simm);
        fprintf(out, "        dr_cycles += %uu;\n", cycle_charge);
        fprintf(out, "        ppc_program_exception(ctx, PPC_PROGRAM_TRAP, 0x%08Xu);\n", inst->address);
        fprintf(out, "        DR_RET();\n");
        fprintf(out, "    }\n");
        break;

    case PPC_OP_TW:
        fprintf(out, "    if (ppc_trap_condition(%uu, ctx->gpr[%u], ctx->gpr[%u])) {\n",
                inst->to, inst->rA, inst->rB);
        fprintf(out, "        dr_cycles += %uu;\n", cycle_charge);
        fprintf(out, "        ppc_program_exception(ctx, PPC_PROGRAM_TRAP, 0x%08Xu);\n", inst->address);
        fprintf(out, "        DR_RET();\n");
        fprintf(out, "    }\n");
        break;

    case PPC_OP_SC:
        fprintf(out, "    dr_cycles += %uu;\n", cycle_charge);
        fprintf(out, "    ppc_system_call_exception(ctx, 0x%08Xu);\n", inst->address);
        fprintf(out, "    DR_RET();\n");
        break;

    case PPC_OP_RFI:
        fprintf(out, "    dr_cycles += %uu;\n", cycle_charge);
        fprintf(out, "    ppc_rfi(ctx, 0x%08Xu);\n", inst->address);
        fprintf(out, "    DR_RET();\n");
        break;

    case PPC_OP_CRAND:  emit_cr_logical(out, inst, "a & b"); break;
    case PPC_OP_CRANDC: emit_cr_logical(out, inst, "a & ~b"); break;
    case PPC_OP_CREQV:  emit_cr_logical(out, inst, "~(a ^ b)"); break;
    case PPC_OP_CRNAND: emit_cr_logical(out, inst, "~(a & b)"); break;
    case PPC_OP_CRNOR:  emit_cr_logical(out, inst, "~(a | b)"); break;
    case PPC_OP_CROR:   emit_cr_logical(out, inst, "a | b"); break;
    case PPC_OP_CRORC:  emit_cr_logical(out, inst, "a | ~b"); break;
    case PPC_OP_CRXOR:  emit_cr_logical(out, inst, "a ^ b"); break;

    case PPC_OP_MCRF: {
        u32 dst_shift = cr_field_shift(inst->crfD);
        u32 src_shift = cr_field_shift(inst->crfS);
        fprintf(out, "    {\n");
        fprintf(out, "        u32 bits = (ctx->cr >> %u) & 0xFu;\n", src_shift);
        fprintf(out, "        ctx->cr = (ctx->cr & ~(0xFu << %u)) | (bits << %u);\n",
                dst_shift, dst_shift);
        fprintf(out, "    }\n");
        break;
    }

    case PPC_OP_MCRXR: {
        u32 dst_shift = cr_field_shift(inst->crfD);
        fprintf(out, "    {\n");
        fprintf(out, "        u32 bits = (ctx->xer >> 28) & 0xFu;\n");
        fprintf(out, "        ctx->cr = (ctx->cr & ~(0xFu << %u)) | (bits << %u);\n",
                dst_shift, dst_shift);
        fprintf(out, "        ctx->xer &= ~0xE0000000u;\n");
        fprintf(out, "    }\n");
        break;
    }

    case PPC_OP_MFCR:
        fprintf(out, "    ctx->gpr[%u] = ctx->cr;\n", inst->rD);
        break;

    case PPC_OP_MTCRF: {
        u32 mask = 0;
        for (u32 crf = 0; crf < 8; crf++) {
            if (inst->crm & (0x80u >> crf))
                mask |= 0xFu << cr_field_shift((u8)crf);
        }
        if (mask) {
            fprintf(out, "    ctx->cr = (ctx->cr & ~0x%08Xu) | (ctx->gpr[%u] & 0x%08Xu);\n",
                    mask, inst->rS, mask);
        } else {
            fprintf(out, "    // mtcrf mask selects no CR fields\n");
        }
        break;
    }

    case PPC_OP_MFMSR:
        fprintf(out, "    ctx->gpr[%u] = ctx->msr;\n", inst->rD);
        break;

    case PPC_OP_MTMSR:
        fprintf(out, "    ctx->msr = ctx->gpr[%u];\n", inst->rS);
        break;

    case PPC_OP_MFSR:
        fprintf(out, "    ctx->gpr[%u] = ctx->sr[%u];\n", inst->rD, inst->sr);
        break;

    case PPC_OP_MFSRIN:
        fprintf(out, "    ctx->gpr[%u] = ctx->sr[(ctx->gpr[%u] >> 28) & 0xFu];\n",
                inst->rD, inst->rB);
        break;

    case PPC_OP_MTSR:
        fprintf(out, "    ctx->sr[%u] = ctx->gpr[%u];\n", inst->sr, inst->rS);
        break;

    case PPC_OP_MTSRIN:
        fprintf(out, "    ctx->sr[(ctx->gpr[%u] >> 28) & 0xFu] = ctx->gpr[%u];\n",
                inst->rB, inst->rS);
        break;

    case PPC_OP_MFTB:
        fprintf(out, "    ctx->gpr[%u] = ppc_mftb(ctx, %uu, 0x%08Xu);\n",
                inst->rD, inst->spr, inst->address);
        fprintf(out, "    if (ctx->exception) { dr_cycles += %uu; DR_RET(); }\n", cycle_charge);
        break;

    case PPC_OP_MFSPR:
        fprintf(out, "    ctx->gpr[%u] = ppc_mfspr(ctx, %uu, 0x%08Xu);\n",
                inst->rD, inst->spr, inst->address);
        fprintf(out, "    if (ctx->exception) { dr_cycles += %uu; DR_RET(); }\n", cycle_charge);
        break;

    case PPC_OP_MTSPR:
        fprintf(out, "    ppc_mtspr(ctx, %uu, ctx->gpr[%u], 0x%08Xu);\n",
                inst->spr, inst->rS, inst->address);
        fprintf(out, "    if (ctx->exception) { dr_cycles += %uu; DR_RET(); }\n", cycle_charge);
        break;

    case PPC_OP_TLBIE:
        fprintf(out, "    ppc_tlbie(ctx, ctx->gpr[%u], 0x%08Xu);\n", inst->rB, inst->address);
        fprintf(out, "    if (ctx->exception) { dr_cycles += %uu; DR_RET(); }\n", cycle_charge);
        break;

    case PPC_OP_SYNC:
    case PPC_OP_EIEIO:
    case PPC_OP_ISYNC:
    case PPC_OP_TLBSYNC:
        fprintf(out, "    ppc_memory_fence();\n");
        break;

    case PPC_OP_ECIWX:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 ea = ");
        emit_xform_ea(out, inst->rA, inst->rB, false);
        fprintf(out, ";\n");
        fprintf(out, "        u32 value = ppc_eciwx(ctx, ea, 0x%08Xu);\n", inst->address);
        fprintf(out, "        if (ctx->exception) { dr_cycles += %uu; DR_RET(); }\n", cycle_charge);
        fprintf(out, "        ctx->gpr[%u] = value;\n", inst->rD);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_ECOWX:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 ea = ");
        emit_xform_ea(out, inst->rA, inst->rB, false);
        fprintf(out, ";\n");
        fprintf(out, "        ppc_ecowx(ctx, ea, ctx->gpr[%u], 0x%08Xu);\n",
                inst->rS, inst->address);
        fprintf(out, "        if (ctx->exception) { dr_cycles += %uu; DR_RET(); }\n", cycle_charge);
        fprintf(out, "    }\n");
        break;

    default:
        fprintf(out, "    dr_cycles += %uu;\n", cycle_charge);
        fprintf(out, "    ppc_fallback_instruction(ctx, 0x%08Xu, 0x%08Xu);\n",
                inst->raw, inst->address);
        fprintf(out, "    DR_RET();\n");
        break;
    }

    fprintf(out, "\n");
}

void emit_instruction(FILE* out, const PPCInst* inst) {
    /* Standalone single-instruction emission has no surrounding chunk/block
     * context (no switch-dispatch entry compensation, no block-exit flush --
     * see emit_function for that machinery), so this instruction is its own
     * one-instruction "block": charge exactly its own cost at any exit. */
    emit_instruction_with_range(out, inst, 0, (u32)-1, dr_ppc_num_cycles(inst->op));
}

/* A "branch" for basic-block boundary purposes: any instruction that can
 * transfer control away from plain sequential flow, statically (b/bc, whose
 * targets can be resolved to a local label) or dynamically (bclr/bcctr,
 * whose target is never resolvable at emit time). bl/bcl (the lk forms of
 * b/bc) count too: they always return to the dispatch loop rather than
 * falling straight through in this chunk, but a *later* dispatch can land
 * back on the instruction right after them (the callee's blr targets LR,
 * i.e. this address), which is exactly the re-entry case leader rule (c)
 * below exists to cover. */
static bool ppc_op_is_branch(PPCOpcode op) {
    return op == PPC_OP_B || op == PPC_OP_BC || op == PPC_OP_BCLR || op == PPC_OP_BCCTR;
}

/* An instruction whose every emitted exit path already carries its own
 * `ctx->cycles += ...;` charge (see emit_direct_branch/emit_instruction_with_
 * range's PPC_OP_SC/PPC_OP_RFI cases) and never falls through to more code in
 * this chunk on ANY path -- so emit_function's per-block fallthrough flush
 * would only ever reach unreachable code after these and must be skipped. */
static bool ppc_op_always_exits(PPCOpcode op) {
    return op == PPC_OP_B || op == PPC_OP_SC || op == PPC_OP_RFI;
}

/* Phase B (codegen speed campaign): whitelist of opcodes whose emitted body
 * NEVER needs an accurate ctx->pc -- i.e. it is safe to elide the per-
 * instruction `ctx->pc = 0x...;` stamp emit_function used to print
 * unconditionally before every instruction. Derived by auditing every case
 * emit_instruction_with_range can emit against cpu.h's helper prototypes and
 * the real runtime implementations (recompiler/src/cpu/cpu.c,
 * runtime/src/cpu_glue.c, memory.c, mmio.c):
 *
 *   - Pure register/CR/FPU/PS arithmetic and compares emit nothing but
 *     `ctx->gpr/fpr/ps1/cr/xer/fpscr[...]` reads+writes, plus (for the
 *     .-oe/.rc forms) calls to ppc_set_xer_ov/ppc_fpscr_updated/ppc_fres/
 *     ppc_frsqrte/ppc_fma/ppc_fctiw/ppc_ps_res/ppc_ps_rsqrte -- every one of
 *     those helpers takes CPUState* but (verified in cpu.c/cpu_glue.c) never
 *     reads or writes cpu->pc, so ctx->pc is dead across the call.
 *   - SYNC/EIEIO/ISYNC/TLBSYNC call ppc_memory_fence(void), which doesn't
 *     even take a CPUState* -- structurally can't touch pc.
 *   - DCBST/DCBF/DCBTST/DCBT/DCBI/ICBI emit only `(void)ctx;` (no-ops in
 *     this model) -- nothing to observe.
 *   - MFMSR/MTMSR/MFSR/MFSRIN/MTSR/MTSRIN/MFCR/MTCRF/MCRF/MCRXR and the CR
 *     logical ops touch only their own named fields, no helper calls at all.
 *   - B/BC/BCLR/BCCTR (ppc_op_is_branch) are deliberately NOT elided here --
 *     they get their own entry via emit_direct_branch/emit_dynamic_branch,
 *     which ALREADY write ctx->pc explicitly on every taken/exit path
 *     (verified: emit_branch_condition/emit_direct_branch/emit_dynamic_branch
 *     never *read* ctx->pc, so the pre-instruction stamp was always
 *     redundant for them -- this table just makes that existing fact
 *     explicit instead of re-deriving it ad hoc).
 *
 * Phase C (codegen speed campaign) ADDS a second, disjoint reason an opcode
 * can be pc_pure: EVERY direct mem_read* / mem_write* call the emitter prints
 * (whether the inline dolrecomp_mem_read*_fast/write*_fast fast path used by
 * the plain load/store class, or an unconditional slow call kept for a
 * complex memory op) now carries this instruction's own address as `cia`
 * explicitly (see emit_load_fast/emit_store_fast/.../emit_dcbz and the
 * LWARX/STWCX/STFIWX/LSWI/LSWX/STSWI/STSWX/LMW/STMW/LWBRX/LHBRX/STWBRX/
 * STHBRX cases). mem_read* / mem_write*'s slow branch stamps `cpu->pc = cia;`
 * itself before it could be observed by anything (memory.c), so ctx->pc no
 * longer needs to be pre-stamped for ANY of these opcodes either -- this is
 * the SAME "pick one convention" decision as Phase C's ABI change: once a
 * call site passes cia, its pc pre-stamp is provably dead, by the identical
 * reasoning Phase B already applied to pure-arithmetic ops:
 *   - LWZ/LBZ/LHZ/LHA (+U/X/UX forms), STW/STB/STH (+U/X/UX forms), LFS/LFD/
 *     STFS/STFD (+U/X/UX forms): the plain load/store class, fully inlined.
 *   - LWBRX/LHBRX/STWBRX/STHBRX, LWARX/STWCX/STFIWX, LSWI/LSWX/STSWI/STSWX,
 *     LMW/STMW, DCBZ: kept on an unconditional mem_read* / mem_write* call
 *     (not inlined -- reservation/string/multi-register semantics are
 *     ambiguous or low-frequency enough that the fast-path replication
 *     isn't worth it), but their calls now carry cia too, so they join the
 *     whitelist on the same basis.
 *
 * Everything else stays conservatively pc-observing and keeps its stamp
 * byte-identical to before Phase B/C:
 *   - PSQ_L/PSQ_LU/PSQ_LX/PSQ_LUX/PSQ_ST/PSQ_STU/PSQ_STX/PSQ_STUX call
 *     ppc_psq_load/ppc_psq_store (cpu_glue.c), which internally call
 *     mem_read* / mem_write* WITHOUT a per-op cia of their own -- they rely on
 *     the caller's pc pre-stamp being accurate (the *_legacy overload
 *     resolves cia = cpu->pc), so PSQ stays in the conservative "calls
 *     another CPUState helper" class below, unchanged from Phase B.
 *   - SPR access (mftb/mfspr/mtspr/tlbie), dcbz_l, eciwx/ecowx, and sc/trap/
 *     rfi all call ctx-taking runtime helpers that can raise ctx->exception
 *     (ppc_take_exception, called through ppc_program_exception/ppc_dsi_
 *     exception/ppc_alignment_exception, unconditionally OVERWRITES ctx->pc
 *     to the exception vector using the explicitly-passed cia, not ctx->pc)
 *     -- kept stamped per the class RULE (any ctx-taking helper call is
 *     conservatively pc-observing) even where today's implementation
 *     doesn't strictly need the pre-existing value, because (a) correctness
 *     must win over elision on an ambiguous/low-frequency class and (b) it
 *     keeps this table auditable against the RULE alone, not against every
 *     runtime .c file's current internals.
 *   - The unresolved-opcode default calls ppc_fallback_instruction, which
 *     can invoke an opaque host callback (ctx->instruction_fallback) whose
 *     behavior isn't visible here -- always kept stamped, along with
 *     PPC_OP_UNKNOWN and any future opcode not yet added to either switch
 *     (the `default: return false` below is deliberately the safe branch).
 *
 * Interrupt-delivery note (why eliding the stamp on PURE instructions is
 * safe even though the dispatch loop delivers asynchronous interrupts using
 * ctx->pc as the resume address): dispatch only ever reads ctx->pc for that
 * purpose AFTER a dolrecomp_call return (runtime/src/dispatch.c), and every
 * return path in generated code -- branch taken, exception guard, deadline
 * yield, end-of-chunk fallthrough -- already writes ctx->pc explicitly
 * before returning. A pc left stale between two PURE instructions inside a
 * still-running chunk is never observed by anything. */
static bool ppc_op_is_pc_pure(PPCOpcode op) {
    switch (op) {
    case PPC_OP_MULLI:
    case PPC_OP_SUBFIC:
    case PPC_OP_ADDI:
    case PPC_OP_ADDIC:
    case PPC_OP_ADDIC_DOT:
    case PPC_OP_ADDIS:
    case PPC_OP_CMPI:
    case PPC_OP_CMPLI:
    case PPC_OP_CMP:
    case PPC_OP_CMPL:
    case PPC_OP_ORI:
    case PPC_OP_ORIS:
    case PPC_OP_XORI:
    case PPC_OP_XORIS:
    case PPC_OP_ANDI:
    case PPC_OP_ANDIS:
    case PPC_OP_ADD:
    case PPC_OP_ADDO:
    case PPC_OP_ADDC:
    case PPC_OP_ADDCO:
    case PPC_OP_ADDE:
    case PPC_OP_ADDEO:
    case PPC_OP_ADDME:
    case PPC_OP_ADDMEO:
    case PPC_OP_ADDZE:
    case PPC_OP_ADDZEO:
    case PPC_OP_SUBF:
    case PPC_OP_SUBFO:
    case PPC_OP_SUBFC:
    case PPC_OP_SUBFCO:
    case PPC_OP_SUBFE:
    case PPC_OP_SUBFEO:
    case PPC_OP_SUBFME:
    case PPC_OP_SUBFMEO:
    case PPC_OP_SUBFZE:
    case PPC_OP_SUBFZEO:
    case PPC_OP_NEG:
    case PPC_OP_NEGO:
    case PPC_OP_MULLW:
    case PPC_OP_MULLWO:
    case PPC_OP_MULHW:
    case PPC_OP_MULHWU:
    case PPC_OP_DIVW:
    case PPC_OP_DIVWO:
    case PPC_OP_DIVWU:
    case PPC_OP_DIVWUO:
    case PPC_OP_AND:
    case PPC_OP_ANDC:
    case PPC_OP_OR:
    case PPC_OP_ORC:
    case PPC_OP_XOR:
    case PPC_OP_NAND:
    case PPC_OP_NOR:
    case PPC_OP_EQV:
    case PPC_OP_CNTLZW:
    case PPC_OP_EXTSB:
    case PPC_OP_EXTSH:
    case PPC_OP_SLW:
    case PPC_OP_SRW:
    case PPC_OP_SRAW:
    case PPC_OP_SRAWI:
    case PPC_OP_RLWINM:
    case PPC_OP_RLWNM:
    case PPC_OP_RLWIMI:
    case PPC_OP_FADDS:
    case PPC_OP_FSUBS:
    case PPC_OP_FMULS:
    case PPC_OP_FDIVS:
    case PPC_OP_FRES:
    case PPC_OP_FMADDS:
    case PPC_OP_FMSUBS:
    case PPC_OP_FNMADDS:
    case PPC_OP_FNMSUBS:
    case PPC_OP_FADD:
    case PPC_OP_FSUB:
    case PPC_OP_FMUL:
    case PPC_OP_FDIV:
    case PPC_OP_FRSQRTE:
    case PPC_OP_FMADD:
    case PPC_OP_FMSUB:
    case PPC_OP_FNMADD:
    case PPC_OP_FNMSUB:
    case PPC_OP_FCTIW:
    case PPC_OP_FCTIWZ:
    case PPC_OP_FMR:
    case PPC_OP_FNEG:
    case PPC_OP_FABS:
    case PPC_OP_FNABS:
    case PPC_OP_FRSP:
    case PPC_OP_FSEL:
    case PPC_OP_FCMPU:
    case PPC_OP_FCMPO:
    case PPC_OP_MTFSB0:
    case PPC_OP_MTFSB1:
    case PPC_OP_MCRFS:
    case PPC_OP_MFFS:
    case PPC_OP_MTFSF:
    case PPC_OP_MTFSFI:
    case PPC_OP_PS_ADD:
    case PPC_OP_PS_SUB:
    case PPC_OP_PS_MUL:
    case PPC_OP_PS_DIV:
    case PPC_OP_PS_RES:
    case PPC_OP_PS_RSQRTE:
    case PPC_OP_PS_MADD:
    case PPC_OP_PS_MSUB:
    case PPC_OP_PS_NMADD:
    case PPC_OP_PS_NMSUB:
    case PPC_OP_PS_NEG:
    case PPC_OP_PS_ABS:
    case PPC_OP_PS_NABS:
    case PPC_OP_PS_MR:
    case PPC_OP_PS_SUM0:
    case PPC_OP_PS_SUM1:
    case PPC_OP_PS_MULS0:
    case PPC_OP_PS_MULS1:
    case PPC_OP_PS_MADDS0:
    case PPC_OP_PS_MADDS1:
    case PPC_OP_PS_MERGE00:
    case PPC_OP_PS_MERGE01:
    case PPC_OP_PS_MERGE10:
    case PPC_OP_PS_MERGE11:
    case PPC_OP_PS_CMPU0:
    case PPC_OP_PS_CMPO0:
    case PPC_OP_PS_CMPU1:
    case PPC_OP_PS_CMPO1:
    case PPC_OP_PS_SEL:
    case PPC_OP_DCBST:
    case PPC_OP_DCBF:
    case PPC_OP_DCBTST:
    case PPC_OP_DCBT:
    case PPC_OP_DCBI:
    case PPC_OP_ICBI:
    case PPC_OP_B:
    case PPC_OP_BC:
    case PPC_OP_BCLR:
    case PPC_OP_BCCTR:
    case PPC_OP_CRAND:
    case PPC_OP_CRANDC:
    case PPC_OP_CREQV:
    case PPC_OP_CRNAND:
    case PPC_OP_CRNOR:
    case PPC_OP_CROR:
    case PPC_OP_CRORC:
    case PPC_OP_CRXOR:
    case PPC_OP_MCRF:
    case PPC_OP_MCRXR:
    case PPC_OP_MFCR:
    case PPC_OP_MTCRF:
    case PPC_OP_MFMSR:
    case PPC_OP_MTMSR:
    case PPC_OP_MFSR:
    case PPC_OP_MFSRIN:
    case PPC_OP_MTSR:
    case PPC_OP_MTSRIN:
    case PPC_OP_SYNC:
    case PPC_OP_EIEIO:
    case PPC_OP_ISYNC:
    case PPC_OP_TLBSYNC:
    /* Phase C: plain load/store class (fully inlined) -- see the doc
     * comment above. */
    case PPC_OP_LWZ:
    case PPC_OP_LWZU:
    case PPC_OP_LWZX:
    case PPC_OP_LWZUX:
    case PPC_OP_LBZ:
    case PPC_OP_LBZU:
    case PPC_OP_LBZX:
    case PPC_OP_LBZUX:
    case PPC_OP_LHZ:
    case PPC_OP_LHZU:
    case PPC_OP_LHZX:
    case PPC_OP_LHZUX:
    case PPC_OP_LHA:
    case PPC_OP_LHAU:
    case PPC_OP_LHAX:
    case PPC_OP_LHAUX:
    case PPC_OP_STW:
    case PPC_OP_STWU:
    case PPC_OP_STWX:
    case PPC_OP_STWUX:
    case PPC_OP_STB:
    case PPC_OP_STBU:
    case PPC_OP_STBX:
    case PPC_OP_STBUX:
    case PPC_OP_STH:
    case PPC_OP_STHU:
    case PPC_OP_STHX:
    case PPC_OP_STHUX:
    case PPC_OP_LFS:
    case PPC_OP_LFSU:
    case PPC_OP_LFSX:
    case PPC_OP_LFSUX:
    case PPC_OP_LFD:
    case PPC_OP_LFDU:
    case PPC_OP_LFDX:
    case PPC_OP_LFDUX:
    case PPC_OP_STFS:
    case PPC_OP_STFSU:
    case PPC_OP_STFSX:
    case PPC_OP_STFSUX:
    case PPC_OP_STFD:
    case PPC_OP_STFDU:
    case PPC_OP_STFDX:
    case PPC_OP_STFDUX:
    /* Phase C: kept on an unconditional mem_read* / mem_write* call (not
     * inlined) but that call now carries cia -- see the doc comment above. */
    case PPC_OP_LWBRX:
    case PPC_OP_LHBRX:
    case PPC_OP_STWBRX:
    case PPC_OP_STHBRX:
    case PPC_OP_LWARX:
    case PPC_OP_STWCX:
    case PPC_OP_STFIWX:
    case PPC_OP_LSWI:
    case PPC_OP_LSWX:
    case PPC_OP_STSWI:
    case PPC_OP_STSWX:
    case PPC_OP_LMW:
    case PPC_OP_STMW:
    case PPC_OP_DCBZ:
        return true;
    default:
        /* Everything not explicitly whitelisted above -- PSQ_L/PSQ_LU/
         * PSQ_LX/PSQ_LUX/PSQ_ST/PSQ_STU/PSQ_STX/PSQ_STUX, DCBZ_L, MFTB/
         * MFSPR/MTSPR/TLBIE, ECIWX/ECOWX, SC/RFI/TWI/TW, PPC_OP_UNKNOWN,
         * and any opcode added later that isn't yet cased here -- stays
         * pc-observing. */
        return false;
    }
}

/* Basic-block leaders and per-block cumulative cycle costs for `insts`.
 *
 * `is_leader[i]` marks instruction i as the start of a fresh block (cum[]
 * resets there): the chunk's first instruction, a local branch target of
 * b/bc, or the instruction immediately following any branch (b/bc/bclr/
 * bcctr, lk or not -- see ppc_op_is_branch).
 *
 * `cum[i]` is the running sum of dr_ppc_num_cycles() from instruction i's
 * block leader through i, inclusive -- i.e. exactly what old per-instruction
 * charging would have accumulated in ctx->cycles by the time i's own charge
 * had applied, given execution started at the leader and ran straight
 * through to i. `cum[i] - dr_ppc_num_cycles(insts[i].op)` (computed by the
 * caller where needed) is therefore the PREFIX a switch-dispatch entry
 * directly at i must retroactively subtract, since the block-exit charge
 * this scheme applies always assumes execution started at the leader.
 *
 * Both arrays must be pre-sized to `count` by the caller; a `count == 0`
 * chunk leaves them untouched. */
static void compute_block_costs(const PPCInst* insts, u32 count, u32 func_start,
                                u32 func_end, bool* is_leader, u32* cum) {
    u32 i;

    if (count == 0)
        return;

    memset(is_leader, 0, count * sizeof(bool));

    is_leader[0] = true; /* rule (a): chunk start */

    for (i = 0; i < count; i++) { /* rule (b): local branch targets of b/bc */
        if (insts[i].op == PPC_OP_B || insts[i].op == PPC_OP_BC) {
            u32 target = insts[i].branch_target;
            if (branch_target_is_local(func_start, func_end, target)) {
                u32 k = (target - func_start) / 4u;
                if (k < count)
                    is_leader[k] = true;
            }
        }
    }

    for (i = 0; i < count; i++) { /* rule (c): instruction after any branch */
        if (ppc_op_is_branch(insts[i].op) && i + 1 < count)
            is_leader[i + 1] = true;
    }

    {
        u32 running = 0;
        for (i = 0; i < count; i++) {
            u32 cost = dr_ppc_num_cycles(insts[i].op);
            if (is_leader[i])
                running = 0;
            running += cost;
            cum[i] = running;
        }
    }
}

void emit_function(FILE* out, const PPCInst* insts, u32 count, u32 func_addr) {
    u32 i;
    u32 func_end = func_addr + count * 4u;
    bool* is_leader = NULL;
    u32* cum = NULL;

    if (count > 0) {
        is_leader = (bool*)malloc(count * sizeof(bool));
        cum = (u32*)malloc(count * sizeof(u32));
        if (!is_leader || !cum) {
            fprintf(stderr, "error: out of memory computing block cycle costs\n");
            free(is_leader);
            free(cum);
            exit(1);
        }
        compute_block_costs(insts, count, func_addr, func_end, is_leader, cum);
    }

    fprintf(out, "void func_%08X(CPUState* ctx) {\n", func_addr);
    /* In-chunk bl/blr fast-path shadow call stack (see emit_direct_branch's
     * lk-branch handling and emit_dynamic_branch's is_blr_class handling).
     * Declared UNCONDITIONALLY, before the switch, so `dr_ret_sp = 0` runs on
     * EVERY entry into this function regardless of which case the switch
     * lands on -- including a mid-function dispatch-loop re-entry landing
     * past this declaration, which in C never re-runs a skipped initializer.
     * Putting the declaration (and its zero-init) ahead of the switch instead
     * is what makes "reset shadow at function entry" true unconditionally: a
     * fresh empty shadow on every call to func_%08X, guest recursion included
     * (each nested dolrecomp_call gets its own C stack frame's dr_ret_*, same
     * as any other automatic-storage local). The arrays themselves need no
     * initializer -- they are only ever read at indices the matching push
     * already wrote in THIS SAME invocation. */
    fprintf(out, "    void* dr_ret_lbl[DOLRECOMP_BL_SHADOW_DEPTH];\n");
    fprintf(out, "    u32 dr_ret_pc[DOLRECOMP_BL_SHADOW_DEPTH];\n");
    fprintf(out, "    u32 dr_ret_sp = 0;\n");
    /* E1 (perf campaign 2): invocation-scoped locals. dr_ram: the RAM base
     * never changes after cpu_init, and a local (address never taken) cannot
     * be aliased by guest stores, so gcc keeps it in a host register instead
     * of reloading ctx->ram around every u8 store through it. dr_cycles /
     * dr_deadline: same aliasing story for the per-branch deadline check and
     * per-block charges (2 loads + cmp each becomes reg add + reg cmp);
     * nothing reachable from inside a chunk observes ctx->cycles (dispatch.c
     * reads it only after we return; no MMIO handler or ppc_* helper touches
     * it), so DR_RET() spilling at every exit is the complete coherence
     * contract. cycle_deadline is armed by the dispatch loop before every
     * call and never written inside a chunk — const. The (void) uses keep
     * chunks with no memory ops warning-clean. */
    fprintf(out, "    u8* const dr_ram = ctx->ram;\n");
    fprintf(out, "    u64 dr_cycles = ctx->cycles;\n");
    fprintf(out, "    const u64 dr_deadline = ctx->cycle_deadline;\n");
    fprintf(out, "    (void)dr_ram; (void)dr_deadline;\n");
    fprintf(out, "    switch (ctx->pc) {\n");
    for (i = 0; i < count; i++) {
        /* Every label below is a valid switch-dispatch entry point (a call
         * return, an external tail branch, ...), not just this chunk's block
         * leaders -- so a dispatch landing mid-block must retroactively
         * undo the PREFIX of the block-exit charge (see compute_block_costs)
         * that assumed it started at the leader. Leaders (PREFIX == 0) skip
         * the subtraction; it would be a no-op anyway. */
        u32 prefix = cum[i] - dr_ppc_num_cycles(insts[i].op);
        if (prefix != 0) {
            fprintf(out, "    case 0x%08Xu: dr_cycles -= %uu; goto label_%08X;\n",
                    insts[i].address, prefix, insts[i].address);
        } else {
            fprintf(out, "    case 0x%08Xu: goto label_%08X;\n",
                    insts[i].address, insts[i].address);
        }
    }
    fprintf(out, "    default: DR_RET();\n");
    fprintf(out, "    }\n");

    for (i = 0; i < count; i++) {
        bool block_ends_here = (i + 1 == count) || is_leader[i + 1];

        fprintf(out, "label_%08X:\n", insts[i].address);
        /* Phase B (codegen speed campaign): the per-instruction ctx->pc
         * store used to be unconditional here -- the single largest
         * remaining per-instruction memory write after Phase A coalesced
         * cycle charging. Emit it only when this instruction's body can
         * actually observe ctx->pc (see ppc_op_is_pc_pure's derivation
         * above); every switch-dispatch/goto entry above already lands on
         * this label with ctx->pc already equal to insts[i].address (that's
         * how the switch(ctx->pc) dispatch works), so skipping the stamp on
         * a run of PURE instructions is a no-op until either (a) a later
         * pc-observing instruction in the same block re-stamps accurately,
         * or (b) an exit path (branch taken, guard, end-of-chunk) writes
         * ctx->pc explicitly -- both of which happen on every path that can
         * actually leave this chunk or call into the host. */
        if (!insts[i].embedded_data && !ppc_op_is_pc_pure(insts[i].op)) {
            fprintf(out, "    ctx->pc = 0x%08Xu;\n", insts[i].address);
        }
        /* The fast path is emitted only at the load label.  Interior switch
         * entries still land on their ordinary cmplwi/bc labels below, and
         * uniform mode (deadline==0) cannot enter it, preserving the pinned
         * uniform control-flow shape exactly. */
        if (is_lwz_cmpli_beq_poll(insts, count, i)) {
            emit_lwz_cmpli_beq_poll(out, &insts[i], &insts[i + 1u],
                                    &insts[i + 2u], cum[i + 2u],
                                    insts[i + 3u].address);
        }
        /* Cycle cost is coalesced per basic block, charged at each exit
         * (return/goto emitted inside the instruction body, e.g. a taken
         * branch or an exception guard) rather than once per instruction:
         * see compute_block_costs and the exit charges wired through
         * emit_instruction_with_range/emit_direct_branch/emit_dynamic_branch/
         * emit_psq_load/emit_psq_store. This label's own switch-dispatch
         * entry above already retroactively adjusts for mid-block entry, so
         * the charge below (cum[i], the block-leader..here inclusive sum)
         * is exact regardless of where execution actually entered. Cost
         * values are Dolphin's own oracle timing model — see ppc_cycles.c. */
        emit_instruction_with_range(out, &insts[i], func_addr, func_end, cum[i]);

        if (block_ends_here && !ppc_op_always_exits(insts[i].op)) {
            /* This instruction can fall through (on at least one path) into
             * a fresh block (the next instruction is a leader, or this is
             * the chunk's last instruction) without itself emitting an exit
             * charge on that path -- flush the block's accumulated cost
             * here so it isn't silently dropped. Exit paths that already
             * charged and returned/goto'd never reach this line. */
            fprintf(out, "    dr_cycles += %uu;\n", cum[i]);
        }
    }

    fprintf(out, "    ctx->pc = 0x%08Xu;\n", func_end);
    /* Falling off the chunk's end is an exit like any other — spill the
     * cycles local (DR_RET()'s job everywhere else). */
    fprintf(out, "    ctx->cycles = dr_cycles;\n");
    fprintf(out, "}\n\n");

    free(is_leader);
    free(cum);
}
