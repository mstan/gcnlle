/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * gcnrecomp runtime — DolRecomp CPUState ABI contract.
 *
 * THIS HEADER IS THE INTEGRATION SEAM WITH TRACK A (the recompiler).
 * DolRecomp's generated C emits, at the top of every output file,
 * `#include "cpu/cpu.h"` (recompiler/src/backend/emitter.c,
 * emit_header_for_cpu). Every generated function is
 * `void func_XXXXXXXX(CPUState* ctx)` (recompiler/src/backend/dispatch.c,
 * emit_chunk_prototype) and its body references `ctx->gpr[..]`, `ctx->fpr[..]`,
 * `ctx->ps1[..]`, `ctx->pc/lr/ctr/cr/xer/fpscr/exception`, calls
 * `mem_read8/16/32/64(ctx, ea)` + `mem_write8/16/32/64(ctx, ea, v)`, and the
 * `ppc_*` helper family. So the host runtime must supply EXACTLY this struct
 * layout (by field name) and these prototypes.
 *
 * PROVENANCE: this is a field-for-field mirror of the in-tree recompiler's
 * recompiler/src/cpu/cpu.h (the fork's authoritative contract), cross-checked
 * against reshine's runtime/include/cpu/cpu.h + cpu_state.h (the gold-reference
 * DolRecomp host). The DolRecomp-required fields appear first, in the exact
 * order/type of recompiler/src/cpu/cpu.h `struct CPUState`; runtime-only
 * bookkeeping is APPENDED at the end (safe — generated code only names fields,
 * never assumes sizeof or trailing layout; reshine does the same with
 * cycles/halted/halt_reason). See runtime/ABI.md for the full citation table.
 *
 * The runtime IMPLEMENTS this contract itself (memory.c + cpu_glue.c); it does
 * NOT link the recompiler's cpu.c. This mirrors reshine, which reimplements the
 * same contract in its own runtime rather than linking DolRecomp internals.
 */
#ifndef GCN_CPU_CPU_H
#define GCN_CPU_CPU_H

#include "common/types.h"

/* --- GameCube / Wii memory geometry (mirror of recompiler cpu.h) --- */
#define GC_MAIN_RAM_SIZE    (24 * 1024 * 1024)
#define GC_RAM_BASE         0x80000000u
#define GC_RAM_UNCACHED     0xC0000000u

#define WII_MEM2_SIZE       (64 * 1024 * 1024)
#define WII_MEM2_BASE       0x90000000u
#define WII_MEM2_UNCACHED   0xD0000000u

/* M1: the true hardware reset-vector ROM window. `cpu_glue.c`'s
 * exception_vector_address already routes to 0xFFF00000+vector when
 * MSR[IP]=1 (the real BS1 entry is 0xFFF00100, vector 0x100 = system reset);
 * this is the READ-ONLY backing for that physical window, mapping guest
 * 0xFFF00000+k to descrambled-ROM byte k (k==0 is the file's plaintext (C)
 * header; BS1 itself starts at k=0x100 — docs/M1_PLAN.md §1/§6.2). Sized to
 * whatever the caller backs (gcn_mem_set_rom_window); reads past that extent
 * are unmapped, same as any other device gap. */
#define GCN_ROM_WINDOW_BASE 0xFFF00000u

/* --- exception/program-cause/vector bits (mirror of recompiler cpu.h) --- */
#define PPC_EXC_PROGRAM       0x00000001u
#define PPC_EXC_DSI           0x00000002u
#define PPC_EXC_ALIGNMENT     0x00000004u
#define PPC_EXC_SYSTEM_CALL   0x00000008u
#define PPC_EXC_MACHINE_CHECK 0x00000010u
#define PPC_EXC_EXTERNAL      0x00000020u  /* external interrupt (PI cause & mask) */

#define PPC_PROGRAM_FP        0x00100000u
#define PPC_PROGRAM_ILLEGAL   0x00080000u
#define PPC_PROGRAM_PRIV      0x00040000u
#define PPC_PROGRAM_TRAP      0x00020000u

#define PPC_DSI_EAR_DISABLED  0x00100000u

#define PPC_VECTOR_MACHINE_CHECK 0x00200u
#define PPC_VECTOR_DSI           0x00300u
#define PPC_VECTOR_EXTERNAL      0x00500u
#define PPC_VECTOR_ALIGNMENT     0x00600u
#define PPC_VECTOR_PROGRAM       0x00700u
#define PPC_VECTOR_SYSTEM_CALL   0x00C00u

#define PPC_HID2_LSQE   0x80000000u
#define PPC_HID2_PSE    0x20000000u
#define PPC_HID2_LCE    0x10000000u
#define PPC_HID2_DCHERR 0x00800000u
#define PPC_HID2_DCHEE  0x00080000u

#define PPC_GEKKO_PVR 0x00083214u

typedef struct CPUState CPUState;

/* External (MMIO / device) access callbacks. The generic bus primitives route
 * any access that is NOT backed by flat RAM to these hooks, which the device
 * layer (EXI/VI/GX/DSP/DI/SI — future milestones) installs. This is the
 * faithful LLE boundary: unmapped != silently zero; it goes to the device
 * model. Signatures mirror recompiler/src/cpu/cpu.h. */
typedef u64 (*PPCExternalRead)(CPUState* cpu, u32 ea, u8 size);
typedef void (*PPCExternalWrite)(CPUState* cpu, u32 ea, u64 value, u8 size);
typedef u32 (*PPCExternalRead32)(CPUState* cpu, u32 ea, u8 rid);
typedef void (*PPCExternalWrite32)(CPUState* cpu, u32 ea, u32 value, u8 rid);
typedef void (*PPCInstructionFallback)(CPUState* cpu, u32 raw, u32 cia);
typedef bool (*PPCHostCall)(CPUState* cpu, u32 address);

struct CPUState {
    /* ---- DolRecomp-required layout (order/type identical to
     *      recompiler/src/cpu/cpu.h). Do NOT reorder or insert here. ---- */
    u32 gpr[32];
    f64 fpr[32];
    f64 ps1[32];             /* paired-single upper halves (ps1) */
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

    /* ---- runtime-only bookkeeping (APPENDED; not part of the DolRecomp
     *      contract; safe because generated code never assumes sizeof). ---- */
    u64 cycles;              /* derived cycle accuracy: Dolphin-model Gekko core
                              * cycles retired, incremented per emitted
                              * instruction by the generated code (emitter.c),
                              * consumed as a per-run delta by dispatch.c's
                              * gcn_dispatch_charge. No longer runtime-only:
                              * generated code names this field directly. */
    u64 cycle_deadline;      /* deadline yield: generated code's taken BACKWARD
                              * conditional branches stay in-chunk (goto) while
                              * cycles < cycle_deadline and return to the
                              * dispatch loop once it expires — so tight guest
                              * loops run at native speed while device ticks
                              * keep a bounded cycle granularity (the dispatch
                              * loop sets this to cycles + quantum before every
                              * dolrecomp_call; pre-expired == old always-return
                              * control flow, which is exactly what the
                              * GCN_CYCLES_UNIFORM baseline uses). */
    bool halted;             /* set by the dispatch driver on a terminal state */
    int halt_reason;         /* GCN_HALT_* */

    /* M1: read-only backing for the 0xFFF00000 ROM window (borrowed pointer;
     * owner is whatever descrambled the image, e.g. exi.c's
     * gcn_exi_set_rom_scrambled owned buffer). NULL/0 = unmapped, same as
     * before this field existed (memory.c only consults it in the read path,
     * never gcn_mem_resolve, so a WRITE there stays loud/unmapped). */
    const u8* rom_window;
    u32       rom_window_size;
};

enum {
    GCN_HALT_NONE = 0,
    GCN_HALT_EXCEPTION = 1,   /* unhandled/interesting exception raised */
    GCN_HALT_NO_FUNCTION = 2, /* dispatch found no func for ctx->pc (host entry) */
    GCN_HALT_STUCK = 3,       /* pc did not advance (self-loop) */
};

/* --- lifecycle + flat memory (implemented in memory.c) --- */
bool cpu_init(CPUState* cpu);
bool cpu_alloc_mem2(CPUState* cpu, u32 size);
void cpu_free(CPUState* cpu);
void cpu_reset(CPUState* cpu);

/* --- bus primitives the generated C links against (implemented in memory.c).
 *     Faithful big-endian; RAM-backed accesses swap, everything else routes to
 *     the external device callbacks above. --- */
u64  mem_read64(CPUState* cpu, u32 addr);
void mem_write64(CPUState* cpu, u32 addr, u64 value);
u32  mem_read32(CPUState* cpu, u32 addr);
void mem_write32(CPUState* cpu, u32 addr, u32 value);
u16  mem_read16(CPUState* cpu, u32 addr);
void mem_write16(CPUState* cpu, u32 addr, u16 value);
u8   mem_read8(CPUState* cpu, u32 addr);
void mem_write8(CPUState* cpu, u32 addr, u8 value);

/* --- PPC semantic helpers the generated C links against (cpu_glue.c). ---
 *     Prototypes mirror recompiler/src/cpu/cpu.h exactly. */
f64 ppc_approx_reciprocal(f64 value);
f64 ppc_approx_rsqrt(f64 value);
bool ppc_fres(CPUState* cpu, f64 value, f64* result);
bool ppc_frsqrte(CPUState* cpu, f64 value, f64* result);
void ppc_ps_res(CPUState* cpu, f64 a, f64 b, f64* result_a, f64* result_b);
void ppc_ps_rsqrte(CPUState* cpu, f64 a, f64 b, f64* result_a, f64* result_b);
bool ppc_fma(CPUState* cpu, f64 a, f64 c, f64 b, bool single,
             bool subtract, bool negative, f64* output);
bool ppc_fctiw(CPUState* cpu, f64 value, bool toward_zero, u64* result);
bool ppc_add_overflowed(u32 a, u32 b, u32 result);
bool ppc_trap_condition(u8 to, u32 a, u32 b);
void ppc_set_xer_ov(CPUState* cpu, bool ov);
void ppc_take_exception(CPUState* cpu, u32 exception, u32 vector, u32 srr0, u32 srr1_info);
void ppc_program_exception(CPUState* cpu, u32 cause, u32 cia);
void ppc_fallback_instruction(CPUState* cpu, u32 raw, u32 cia);
bool ppc_host_call(CPUState* cpu, u32 address);
void ppc_system_call_exception(CPUState* cpu, u32 cia);
void ppc_dsi_exception(CPUState* cpu, u32 ea, u32 cia, u32 dsisr);
void ppc_alignment_exception(CPUState* cpu, u32 ea, u32 cia);
u32 ppc_mftb(CPUState* cpu, u16 tbr, u32 cia);
u32 ppc_mfspr(CPUState* cpu, u16 spr, u32 cia);
void ppc_mtspr(CPUState* cpu, u16 spr, u32 value, u32 cia);
void ppc_rfi(CPUState* cpu, u32 cia);
void ppc_dcbz_l(CPUState* cpu, u32 ea, u32 cia);
void ppc_psq_load(CPUState* cpu, u8 frD, u32 ea, bool w, u8 gqr, bool indexed, u32 cia);
void ppc_psq_store(CPUState* cpu, u8 frS, u32 ea, bool w, u8 gqr, bool indexed, u32 cia);
u32 ppc_eciwx(CPUState* cpu, u32 ea, u32 cia);
void ppc_ecowx(CPUState* cpu, u32 ea, u32 value, u32 cia);
void ppc_tlbie(CPUState* cpu, u32 ea, u32 cia);
void ppc_fpscr_updated(CPUState* cpu);
void ppc_memory_fence(void);

#endif /* GCN_CPU_CPU_H */
