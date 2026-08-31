#ifndef DOLRECOMP_CPU_H
#define DOLRECOMP_CPU_H

#include "common/types.h"

#define GC_MAIN_RAM_SIZE    (24 * 1024 * 1024)
#define GC_RAM_BASE         0x80000000u
#define GC_RAM_UNCACHED     0xC0000000u

#define WII_MEM2_SIZE       (64 * 1024 * 1024)
#define WII_MEM2_BASE       0x90000000u
#define WII_MEM2_UNCACHED   0xD0000000u

#define PPC_EXC_PROGRAM       0x00000001u
#define PPC_EXC_DSI           0x00000002u
#define PPC_EXC_ALIGNMENT     0x00000004u
#define PPC_EXC_SYSTEM_CALL   0x00000008u
#define PPC_EXC_MACHINE_CHECK 0x00000010u
#define PPC_EXC_FP_UNAVAILABLE 0x00000040u /* FP op with MSR[FP]=0 (lazy-FPU trap) */

#define PPC_PROGRAM_FP        0x00100000u
#define PPC_PROGRAM_ILLEGAL   0x00080000u
#define PPC_PROGRAM_PRIV      0x00040000u
#define PPC_PROGRAM_TRAP      0x00020000u

#define PPC_DSI_EAR_DISABLED  0x00100000u

#define PPC_VECTOR_MACHINE_CHECK 0x00200u
#define PPC_VECTOR_DSI           0x00300u
#define PPC_VECTOR_ALIGNMENT     0x00600u
#define PPC_VECTOR_PROGRAM       0x00700u
#define PPC_VECTOR_FP_UNAVAIL    0x00800u
#define PPC_VECTOR_SYSTEM_CALL   0x00C00u

#define PPC_HID2_LSQE   0x80000000u
#define PPC_HID2_PSE    0x20000000u
#define PPC_HID2_LCE    0x10000000u
#define PPC_HID2_DCHERR 0x00800000u
#define PPC_HID2_DCHEE  0x00080000u

#define PPC_GEKKO_PVR 0x00083214u

typedef struct CPUState CPUState;
typedef u64 (*PPCExternalRead)(CPUState* cpu, u32 ea, u8 size);
typedef void (*PPCExternalWrite)(CPUState* cpu, u32 ea, u64 value, u8 size);
typedef u32 (*PPCExternalRead32)(CPUState* cpu, u32 ea, u8 rid);
typedef void (*PPCExternalWrite32)(CPUState* cpu, u32 ea, u32 value, u8 rid);
typedef void (*PPCInstructionFallback)(CPUState* cpu, u32 raw, u32 cia);
typedef bool (*PPCHostCall)(CPUState* cpu, u32 address);

struct CPUState {
    u32 gpr[32];
    f64 fpr[32];
    f64 ps1[32];
    u32 pc;
    u32 lr;
    u32 ctr;
    u32 cr;
    u32 xer;
    u32 fpscr;
    u32 msr;
    u32 srr0;
    u32 srr1;
    u32 dar;
    u32 dsisr;
    u32 ear;
    u32 hid2;
    u64 timebase;
    u32 sr[16];
    u32 gqr[8];
    u32 spr[1024];
    u32 exception;
    u32 program_exception;
    u32 tlb_last_vps;
    u32 tlb_last_index;
    u32 tlb_invalidate_count;
    u32 external_addr;
    u32 external_value;
    u8 external_rid;
    u8 external_read_count;
    u8 external_write_count;
    u32 reserve_addr;
    bool reserve_valid;
    u32 locked_cache_tag[512];
    bool locked_cache_valid[512];
    PPCExternalRead external_read;
    PPCExternalWrite external_write;
    PPCExternalRead32 external_read32;
    PPCExternalWrite32 external_write32;
    PPCInstructionFallback instruction_fallback;
    PPCHostCall host_call;
    void* external_user_data;

    u8* ram;
    u32 ram_size;
    u8* mem2;
    u32 mem2_size;

    /* Derived-cycle-accuracy pacing: every basic block charges its
     * Dolphin-derived cost (recompiler/src/backend/ppc_cycles.c) here in one
     * coalesced `ctx->cycles += Nu;` at each block exit, with a matching
     * retroactive `ctx->cycles -= Pu;` on the switch-dispatch entry path for
     * any mid-block re-entry (recompiler/src/backend/emitter.c,
     * `emit_function`/`compute_block_costs`) -- exact per-instruction value
     * at every point control can leave a chunk, without a per-instruction
     * add. This is no longer runtime-only bookkeeping the way `halted`/
     * `halt_reason` are in runtime/include/cpu/cpu.h (ABI.md "trailing
     * runtime-only fields") — generated code now references `ctx->cycles`
     * unconditionally, so this reference host needs the field too for the
     * codegen_compile test to keep compiling generated output standalone. */
    u64 cycles;
    /* Deadline yield: generated code's taken BACKWARD conditional branches
     * whose target is inside the same chunk stay in-function (goto) while
     * cycles < cycle_deadline, and fall back to `ctx->pc = target; return;`
     * once the deadline expires (recompiler/src/backend/emitter.c). Mirrors
     * runtime/include/cpu/cpu.h's `cycle_deadline` field for the same
     * codegen_compile-standalone-compile reason as `cycles` above. */
    u64 cycle_deadline;
};

bool cpu_init(CPUState* cpu);
bool cpu_alloc_mem2(CPUState* cpu, u32 size); //mem 2 only exists after first aloc
void cpu_free(CPUState* cpu);
void cpu_reset(CPUState* cpu);

/* Phase C (codegen speed campaign): the canonical ABI is now cia-taking --
 * every mem_read* / mem_write* call carries the originating instruction's own
 * address ("cia"), which the slow (MMIO/unmapped) branch stamps into
 * cpu->pc before dispatching to external_read/external_write (see cpu.c).
 * The bare `mem_read32(...)`-style identifiers below are function-like
 * macros that dispatch on ARGUMENT COUNT to either the new 3/4-arg *_cia
 * implementation or a 2/3-arg *_legacy wrapper (cia = cpu->pc, i.e. the
 * pre-Phase-C convention of the emitter pre-stamping ctx->pc before every
 * memory op) -- this is what lets callers that don't carry a cia (this
 * repo's own test_pc_reference.c/test_jumptables.c, and any pre-Phase-C
 * generated bank) keep compiling unmodified against the SAME identifier the
 * Phase-C emitter now calls with an extra argument. New code should always
 * pass cia explicitly; the *_legacy overload exists ONLY for that transition
 * and is never the target of a newly-written call. */
u64  mem_read64_cia   (CPUState* cpu, u32 addr, u32 cia);
u64  mem_read64_legacy(CPUState* cpu, u32 addr);
void mem_write64_cia   (CPUState* cpu, u32 addr, u64 value, u32 cia);
void mem_write64_legacy(CPUState* cpu, u32 addr, u64 value);
u32  mem_read32_cia   (CPUState* cpu, u32 addr, u32 cia);
u32  mem_read32_legacy(CPUState* cpu, u32 addr);
void mem_write32_cia   (CPUState* cpu, u32 addr, u32 value, u32 cia);
void mem_write32_legacy(CPUState* cpu, u32 addr, u32 value);
u16  mem_read16_cia   (CPUState* cpu, u32 addr, u32 cia);
u16  mem_read16_legacy(CPUState* cpu, u32 addr);
void mem_write16_cia   (CPUState* cpu, u32 addr, u16 value, u32 cia);
void mem_write16_legacy(CPUState* cpu, u32 addr, u16 value);
u8   mem_read8_cia   (CPUState* cpu, u32 addr, u32 cia);
u8   mem_read8_legacy(CPUState* cpu, u32 addr);
void mem_write8_cia   (CPUState* cpu, u32 addr, u8 value, u32 cia);
void mem_write8_legacy(CPUState* cpu, u32 addr, u8 value);

/* Arity-dispatch: an N-arg call site picks *_legacy; an (N+1)-arg call site
 * (trailing cia) picks *_cia. Standard "overload by __VA_ARGS__ count"
 * preprocessor trick -- reads take 2 (legacy) or 3 (cia) args, writes take
 * 3 (legacy) or 4 (cia). */
#define GCN_MEM_PICK_R(_1,_2,_3,NAME,...) NAME
#define GCN_MEM_PICK_W(_1,_2,_3,_4,NAME,...) NAME
#define mem_read64(...)  GCN_MEM_PICK_R(__VA_ARGS__, mem_read64_cia,  mem_read64_legacy )(__VA_ARGS__)
#define mem_read32(...)  GCN_MEM_PICK_R(__VA_ARGS__, mem_read32_cia,  mem_read32_legacy )(__VA_ARGS__)
#define mem_read16(...)  GCN_MEM_PICK_R(__VA_ARGS__, mem_read16_cia,  mem_read16_legacy )(__VA_ARGS__)
#define mem_read8(...)   GCN_MEM_PICK_R(__VA_ARGS__, mem_read8_cia,   mem_read8_legacy  )(__VA_ARGS__)
#define mem_write64(...) GCN_MEM_PICK_W(__VA_ARGS__, mem_write64_cia, mem_write64_legacy)(__VA_ARGS__)
#define mem_write32(...) GCN_MEM_PICK_W(__VA_ARGS__, mem_write32_cia, mem_write32_legacy)(__VA_ARGS__)
#define mem_write16(...) GCN_MEM_PICK_W(__VA_ARGS__, mem_write16_cia, mem_write16_legacy)(__VA_ARGS__)
#define mem_write8(...)  GCN_MEM_PICK_W(__VA_ARGS__, mem_write8_cia,  mem_write8_legacy )(__VA_ARGS__)

f64 ppc_approx_reciprocal(f64 value);
f64 ppc_approx_rsqrt(f64 value);
bool ppc_fres(CPUState* cpu, f64 value, f64* result);
bool ppc_frsqrte(CPUState* cpu, f64 value, f64* result);
void ppc_ps_res(CPUState* cpu, f64 a, f64 b, f64* result_a, f64* result_b);
void ppc_ps_rsqrte(CPUState* cpu, f64 a, f64 b, f64* result_a, f64* result_b);
bool ppc_fma(CPUState* cpu, f64 a, f64 c, f64 b, bool single,
             bool subtract, bool negative, f64* output);
bool ppc_fctiw(CPUState* cpu, f64 value, bool toward_zero, u64* result);
void ppc_fcmp(CPUState* cpu, u8 crfd, f64 a, f64 b, bool ordered);
void ppc_fadds(CPUState* cpu, u8 d, u8 a, u8 b);
void ppc_fsubs(CPUState* cpu, u8 d, u8 a, u8 b);
void ppc_fmuls(CPUState* cpu, u8 d, u8 a, u8 c);
void ppc_fdivs(CPUState* cpu, u8 d, u8 a, u8 b);
void ppc_fadd(CPUState* cpu, u8 d, u8 a, u8 b);
void ppc_fsub(CPUState* cpu, u8 d, u8 a, u8 b);
void ppc_fmul(CPUState* cpu, u8 d, u8 a, u8 c);
void ppc_fdiv(CPUState* cpu, u8 d, u8 a, u8 b);
void ppc_frsp(CPUState* cpu, u8 d, u8 b);
void ppc_ps_add_op(CPUState* cpu, u8 d, u8 a, u8 b);
void ppc_ps_sub_op(CPUState* cpu, u8 d, u8 a, u8 b);
void ppc_ps_mul_op(CPUState* cpu, u8 d, u8 a, u8 c);
void ppc_ps_div_op(CPUState* cpu, u8 d, u8 a, u8 b);
bool ppc_add_overflowed(u32 a, u32 b, u32 result);
bool ppc_trap_condition(u8 to, u32 a, u32 b);
void ppc_set_xer_ov(CPUState* cpu, bool ov);
void ppc_take_exception(CPUState* cpu, u32 exception, u32 vector, u32 srr0, u32 srr1_info);
void ppc_program_exception(CPUState* cpu, u32 cause, u32 cia);
/* FP-unavailable (vector 0x800): raised by the emitted MSR[FP] gate before
 * every FP-class instruction (see emitter.c ppc_op_is_fp). SRR0 = cia so
 * the op re-executes after the OS lazy-FPU handler enables FP and rfi's. */
void ppc_fp_unavailable(CPUState* cpu, u32 cia);
void ppc_fallback_instruction(CPUState* cpu, u32 raw, u32 cia);
bool ppc_host_call(CPUState* cpu, u32 address);
void ppc_system_call_exception(CPUState* cpu, u32 cia);
void ppc_dsi_exception(CPUState* cpu, u32 ea, u32 cia, u32 dsisr);
void ppc_alignment_exception(CPUState* cpu, u32 ea, u32 cia);
u32 ppc_mftb(CPUState* cpu, u16 tbr, u32 cia);
u32 ppc_mfspr(CPUState* cpu, u16 spr, u32 cia);
void ppc_mtspr(CPUState* cpu, u16 spr, u32 value, u32 cia);
void ppc_rfi(CPUState* cpu, u32 cia);
void ppc_dcbz(CPUState* cpu, u32 ea, u32 cia);
void ppc_dcbz_l(CPUState* cpu, u32 ea, u32 cia);
void ppc_icbi(CPUState* cpu, u32 ea);
void ppc_psq_load(CPUState* cpu, u8 frD, u32 ea, bool w, u8 gqr, bool indexed, u32 cia);
void ppc_psq_store(CPUState* cpu, u8 frS, u32 ea, bool w, u8 gqr, bool indexed, u32 cia);
u32 ppc_eciwx(CPUState* cpu, u32 ea, u32 cia);
void ppc_ecowx(CPUState* cpu, u32 ea, u32 value, u32 cia);
void ppc_tlbie(CPUState* cpu, u32 ea, u32 cia);
void ppc_fpscr_updated(CPUState* cpu);
void ppc_memory_fence(void);

#endif /* DOLRECOMP_CPU_H */
