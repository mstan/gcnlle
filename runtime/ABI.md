<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# gcnrecomp runtime — DolRecomp codegen ABI

This documents the exact ABI that DolRecomp's generated C expects from its host
runtime, so **Track A** (the recompiler) and **Track B** (this runtime) can be
reconciled at integration. Every claim cites the source it was reversed from:

- **[BK]** the in-tree recompiler backend that *emits* the C —
  `recompiler/src/backend/{emitter.c,dispatch.c,codegen.h}`.
- **[CPU]** the recompiler's own reference host —
  `recompiler/src/cpu/cpu.h` + `cpu.c` (what its generated code links against).
- **[RS]** reshine, the gold-reference DolRecomp host that already boots
  DolRecomp output — `repos/reshine/runtime/include/cpu/{cpu.h,cpu_state.h}`,
  `src/cpu/recomp_cpu.c`, `recomp_dispatch.c`.

The runtime **reimplements** this contract itself (it does not link the
recompiler's `cpu.c`), exactly as reshine reimplements it rather than linking
DolRecomp internals **[RS]**. Our implementation lives in
`runtime/include/cpu/cpu.h` (contract), `runtime/src/memory.c` (bus +
lifecycle), and `runtime/src/cpu_glue.c` (`ppc_*` helpers).

---

## 1. The integration seam: `#include "cpu/cpu.h"`

Every generated output file begins with, verbatim **[BK emitter.c
`emit_header_for_cpu`]**:

```c
// DolRecomp output
// cpu: gekko
#define DOLRECOMP_CPU_GEKKO 1
#define DOLRECOMP_CPU_NAME "gekko"
#include <string.h>
#include <math.h>
#include "cpu/cpu.h"
static inline u32 dolrecomp_rotl32(u32, u32) { ... }
static inline f32 dolrecomp_f32_from_bits(u32) { ... }
static inline u32 dolrecomp_f32_to_bits(f32) { ... }
static inline f64 dolrecomp_f64_from_bits(u64) { ... }
static inline u64 dolrecomp_f64_to_bits(f64) { ... }
static inline f64 dolrecomp_ps_round(f64) { ... }
static inline f64 dolrecomp_ps_from_bits(u32) { ... }
static inline u32 dolrecomp_ps_to_bits(f64) { ... }
```

**Reconciliation rule:** Track A's generated C must be compiled with
`runtime/include` on its include path, so `"cpu/cpu.h"` resolves to
`runtime/include/cpu/cpu.h` and `"common/types.h"` (pulled in by it) to
`runtime/include/common/types.h`. Those two headers ARE the contract; the
`dolrecomp_*` inline helpers and `<string.h>/<math.h>` ride with the generated
TU and need nothing from us. The runtime provides everything else the generated
body names.

The GameCube target is **Gekko** (`emit_header` defaults to `DOLRECOMP_CPU_GEKKO`
**[BK]**); Broadway/Espresso are Wii/Wii-U and out of scope.

---

## 2. `CPUState` — the register-file struct

Generated code always takes `CPUState* ctx` and dereferences it by field name
(`ctx->gpr[..]`, `ctx->fpr[..]`, `ctx->ps1[..]`, `ctx->pc/lr/ctr/cr/xer/fpscr`,
`ctx->exception`, …) **[BK emitter.c throughout]**. The struct we expose is a
field-for-field mirror of **[CPU `struct CPUState`]** (cross-checked against
**[RS `struct rs_cpu`]**, which is byte-identical in the leading region and adds
its own trailing bookkeeping):

| Field | Type | Used by generated code as | Source |
|---|---|---|---|
| `gpr[32]` | `u32` | integer register file | [CPU]/[BK] |
| `fpr[32]` | `f64` | FP register file (ps0 lane) | [CPU]/[BK] |
| `ps1[32]` | `f64` | paired-single upper lane | [CPU]/[BK emit_fload sets `ps1`] |
| `pc,lr,ctr` | `u32` | branch/link/count | [CPU]/[BK emit_*branch] |
| `cr,xer,fpscr` | `u32` | condition/flags | [CPU]/[BK emit_compare*, emit_set_cr*] |
| `msr,srr0,srr1,dar,dsisr,ear,hid2` | `u32` | supervisor state | [CPU] |
| `timebase` | `u64` | mftb/mtspr 284/285 | [CPU] |
| `sr[16],gqr[8],spr[1024]` | `u32` | segment regs, quantizers, SPRs | [CPU] |
| `exception,program_exception` | `u32` | fault signalling (dispatch checks `exception`) | [CPU]/[BK psq `if (ctx->exception) return;`] |
| `tlb_last_*`, `tlb_invalidate_count` | `u32` | tlbie bookkeeping | [CPU] |
| `external_addr/value/rid`, `external_read_count/write_count` | | eciwx/ecowx | [CPU] |
| `reserve_addr` (`u32`), `reserve_valid` (`bool`) | | lwarx/stwcx reservation | [CPU] |
| `locked_cache_tag[512]` (`u32`), `locked_cache_valid[512]` (`bool`) | | dcbz_l locked cache | [CPU] |
| `external_read/write`, `external_read32/write32`, `instruction_fallback`, `host_call` | fn ptrs | device + host-intercept hooks | [CPU] |
| `external_user_data` | `void*` | host back-pointer | [CPU] |
| `ram` (`u8*`), `ram_size` (`u32`), `mem2` (`u8*`), `mem2_size` (`u32`) | | flat memory backing | [CPU]/[BK dispatch uses `ctx->ram_size`] |

**Trailing runtime-only fields** (`u64 cycles; bool halted; int halt_reason;`)
are APPENDED after `mem2_size`. This is safe: generated code only names fields,
never assumes `sizeof(CPUState)` or trailing layout. reshine does the same
(`cycles/halted/halt_reason`) **[RS cpu_state.h]**.

Callback signatures **[CPU]**:
```c
typedef u64 (*PPCExternalRead )(CPUState*, u32 ea, u8 size);
typedef void(*PPCExternalWrite)(CPUState*, u32 ea, u64 value, u8 size);
typedef u32 (*PPCExternalRead32)(CPUState*, u32 ea, u8 rid);
typedef void(*PPCExternalWrite32)(CPUState*, u32 ea, u32 value, u8 rid);
typedef void(*PPCInstructionFallback)(CPUState*, u32 raw, u32 cia);
typedef bool(*PPCHostCall)(CPUState*, u32 address);
```

---

## 3. Memory accessor signatures (bus primitives)

**Phase C (codegen speed campaign) update.** The ABI is now **cia-taking**:
every generated load/store passes the originating instruction's own address
("cia") into `mem_read*`/`mem_write*`. Two things changed together:

1. **Plain loads/stores are inlined.** LWZ/LBZ/LHZ/LHA (+U/X/UX forms),
   STW/STB/STH (+U/X/UX forms), and LFS/LFD/STFS/STFD (+U/X/UX forms) no
   longer call `mem_read*`/`mem_write*` directly. The emitter
   (`emit_load_fast`/`emit_store_fast`/`emit_fload_fast`/`emit_fstore_fast`
   and their `X`-form siblings) instead calls a `static inline` fast-path
   helper emitted per-TU by `emit_header_for_cpu`
   (`dolrecomp_mem_read8/16/32/64_fast`, `dolrecomp_mem_write8/16/32/64_fast`)
   that replicates `memory.c`'s RAM-hit fast path directly against
   `ctx->ram` — gcc inlines it (same translation unit) — and falls back to
   the real `mem_read*`/`mem_write*` bus primitive, passed cia explicitly,
   only for the non-RAM case. Opcodes deliberately kept off the inline path
   (reservation/string/multi-register semantics, or genuinely rare):
   LWARX/STWCX/STFIWX, LSWI/LSWX/STSWI/STSWX, LMW/STMW, DCBZ,
   LWBRX/LHBRX/STWBRX/STHBRX — these still call `mem_read*`/`mem_write*`
   unconditionally, but that call now also carries cia.
2. **The pre-instruction `ctx->pc` stamp is gone for all of the above.**
   Phase B's `ppc_op_is_pc_pure` whitelist (`recompiler/src/backend/emitter.c`)
   now includes every opcode in
   this list, because their `mem_read*`/`mem_write*` call (fast-path
   fallback or direct) carries cia explicitly — the stamp was the only
   reason `ctx->pc` needed to be accurate at that point, and it no longer
   is. PSQ_L/PSQ_LU/PSQ_LX/PSQ_LUX/PSQ_ST/PSQ_STU/PSQ_STX/PSQ_STUX are
   NOT included: `ppc_psq_load`/`ppc_psq_store` (`cpu_glue.c`) call
   `mem_read*`/`mem_write*` internally without a per-op cia of their own, so
   PSQ stays in the conservative "calls another CPUState helper" class and
   keeps its pre-stamp, exactly as Phase B left it.

Canonical (implemented in `runtime/src/memory.c`):

```c
u64  mem_read64_cia (CPUState*, u32 addr, u32 cia);
void mem_write64_cia(CPUState*, u32 addr, u64 value, u32 cia);
u32  mem_read32_cia (CPUState*, u32 addr, u32 cia);
void mem_write32_cia(CPUState*, u32 addr, u32 value, u32 cia);
u16  mem_read16_cia (CPUState*, u32 addr, u32 cia);
void mem_write16_cia(CPUState*, u32 addr, u16 value, u32 cia);
u8   mem_read8_cia  (CPUState*, u32 addr, u32 cia);
void mem_write8_cia (CPUState*, u32 addr, u8  value, u32 cia);
```

**Transitional macro dispatch.** `cpu.h` also declares `*_legacy` overloads
(`mem_read32_legacy(cpu, addr)`, `mem_write32_legacy(cpu, addr, value)`, ...)
that forward `cia = cpu->pc` — i.e. exactly the pre-Phase-C convention of the
emitter pre-stamping `ctx->pc` before every memory op. The bare identifiers
(`mem_read32`, `mem_write32`, ...) are function-like macros that dispatch on
**argument count** to either the `*_cia` or `*_legacy` function:

```c
#define GCN_MEM_PICK_R(_1,_2,_3,NAME,...) NAME
#define mem_read32(...) GCN_MEM_PICK_R(__VA_ARGS__, mem_read32_cia, mem_read32_legacy)(__VA_ARGS__)
```

This is what lets a bank emitted by the OLD (pre-Phase-C) recompiler — which
calls `mem_read32(ctx, ea)` (2 args) — keep compiling and running correctly
against the NEW headers without being regenerated: the 2-arg call resolves
to `mem_read32_legacy`, which reads `cpu->pc` for cia (accurate, because
those old banks still pre-stamp `ctx->pc` before every memory op — Phase C
never touched their `ppc_op_is_pc_pure` decision, since they were compiled
by the OLD emitter). The same mechanism covers hand-written runtime callers
that don't carry a per-call cia: `runtime/src/di.c`'s synthesized DMA writes
and `cpu_glue.c`'s `ppc_psq_load`/`ppc_psq_store`/`ppc_dcbz_l` internals.
**New code should always call the `*_cia` shape (3/4 args) explicitly or go
through the bare identifier with cia supplied; the `*_legacy` overload is a
transition aid, never a new call site's target.**

Semantics we guarantee (mirror of **[CPU]**, faithful/boring per PRINCIPLES):
- **Big-endian** value semantics against a little-endian host (swap on RAM
  access; `read_be*/write_be*` in `common/types.h`).
- MEM1 = 24 MB at `0x80000000` (`GC_RAM_BASE`); the **uncached mirror** at
  `0xC0000000` (`GC_RAM_UNCACHED`) addresses the SAME bytes; a **third,
  physical/real-mode mirror** at addresses `< GC_MAIN_RAM_SIZE` (used by
  exception handlers and BS1's pre-`MSR[DR]` bring-up, entered with
  `MSR[IR]=MSR[DR]=0`) addresses the SAME bytes too — `gcn_mem_resolve`
  checks all three, and so does every `dolrecomp_mem_read*_fast`/
  `dolrecomp_mem_write*_fast` inline helper.
- Any access NOT backed by flat RAM routes to `external_read/external_write`
  (the device layer). With no device installed we warn loudly and return 0 —
  never a silent fake. (We deliberately do **not** copy reshine's `mem.c`
  fake-the-answer forcing such as `0x80000044 -> 0xFFFF`.)
- The slow (non-RAM) branch of `mem_read*_cia`/`mem_write*_cia` does
  `cpu->pc = cia;` BEFORE dispatching to `external_read`/`external_write`/the
  unmapped-access warning, so every consumer of `cpu->pc` downstream (the
  always-on rings, `mmio.c`'s device dispatch, the card-traffic ring, the
  oracle trace) observes exactly the value the old unconditional
  pre-instruction pc-stamp guaranteed. The RAM-hit fast path (both the
  inline helper and `mem_read*_cia`/`mem_write*_cia`'s own RAM branch) never
  touches `cpu->pc`.
- Update-form (`...U`/`...UX`) writeback to `rA` is **unconditional** on
  every path — `mem_read*`/`mem_write*` never signal a fault back to the
  caller for this opcode class (an unmapped/MMIO access routes to the
  device layer or warns and returns 0; it never raises `ctx->exception` the
  way `psq_load`/`psq_store`/`dcbz_l` do), so there is no fault path to
  guard the writeback against. A faulting `lwzu`'s `rA` update happens
  exactly as it did before Phase C.
- Reservation-clear (`lwarx`/`stwcx.`) fires on **every** RAM-hit store,
  including ones reached through the inline fast path — the
  `dolrecomp_mem_write*_fast` helpers replicate `mem_write*`'s
  `clear_matching_reservation` check before writing, not just the slow path.

Lifecycle (also `memory.c`, mirror of **[CPU]**):
`bool cpu_init(CPUState*)`, `bool cpu_alloc_mem2(CPUState*, u32)`,
`void cpu_free(CPUState*)`, `void cpu_reset(CPUState*)`. `cpu_init` sets
`spr[287] = PPC_GEKKO_PVR (0x00083214)`; `cpu_reset` preserves RAM + callbacks
and re-asserts PVR.

---

## 4. `ppc_*` helper family

Out-of-line helpers the emitter references (implemented in
`runtime/src/cpu_glue.c`, ported from **[CPU]**). Prototypes are exactly **[CPU
cpu.h]**. Representative call sites in **[BK]**:

- overflow/carry: `ppc_add_overflowed`, `ppc_set_xer_ov` (emit_add/sub with OE).
- exceptions: `ppc_program_exception`, `ppc_take_exception`, `ppc_dsi_exception`,
  `ppc_alignment_exception`, `ppc_system_call_exception`, `ppc_fallback_instruction`.
- SPR/TB: `ppc_mfspr`, `ppc_mtspr`, `ppc_mftb`.
- paired single: `ppc_psq_load`, `ppc_psq_store` (emit_psq_* pass
  `frD, ea, w, gqr, indexed, cia` then `if (ctx->exception) return;`).
- FP math: `ppc_fres`, `ppc_frsqrte`, `ppc_fma`, `ppc_fctiw`, `ppc_ps_res`,
  `ppc_ps_rsqrte`, `ppc_approx_reciprocal`, `ppc_approx_rsqrt`, `ppc_fpscr_updated`.
- misc: `ppc_rfi`, `ppc_dcbz_l`, `ppc_eciwx`, `ppc_ecowx`, `ppc_tlbie`,
  `ppc_trap_condition`, `ppc_host_call`, `ppc_memory_fence`.

The frsqrte/fres estimate tables are adapted from Dolphin `FloatUtils.cpp`
(GPL-2.0-or-later) per the recompiler source — pure Gekko numeric tables, not
device/oracle logic.

---

## 5. Generated function + dispatch entry

- **Function signature [BK dispatch.c `emit_chunk_prototype`]:**
  `void func_XXXXXXXX(CPUState* ctx);` — one per recompiled function, named by
  its start address in uppercase hex.
- **Control-flow ABI [BK emitter.c]:** a function returns after updating
  `ctx->pc` at a non-local branch / call / return; `bl` sets `ctx->lr = addr+4`.
  Local forward branches use `goto label_XXXXXXXX;`; local backward branches set
  `ctx->pc` and return.
- **Dispatch table [BK dispatch.c `emit_dispatch_helpers`]** is emitted as
  `static inline` into the generated header:
  - `#define DOLRECOMP_ENTRY_POINT 0xXXXXXXXXu`
  - `typedef void (*DolRecompFunction)(CPUState*);`
  - `dolrecomp_find_original(u32) -> DolRecompFunction` (range table)
  - `dolrecomp_call(CPUState*, u32)` — tries `ppc_host_call`, then
    `find_original`, then the physical-address alias (`addr | GC_RAM_BASE`).
  - `dolrecomp_run_blocks(CPUState*, u32 max)` — loop: `dolrecomp_call(ctx,
    ctx->pc)`, stop on `ctx->exception`.

**Reconciliation for the driver:** because the dispatch helpers are
`static inline` in the generated header, the runtime provides a thin non-static
driver TU that `#include`s the generated header and re-exposes a callable
`run_blocks` — exactly reshine's `recomp_dispatch.c` pattern **[RS]**. That TU
is the M0 integration seam left as `TODO` in `runtime/src/main.c`
(`gcn_dispatch.c`); it is intentionally absent until Track A emits code, so the
M0 build carries no generated dependency.

---

## 6. Ambiguities / unknowns flagged for spot-check

1. **`bool` size in the struct.** `<stdbool.h>` `bool` is 1 byte on our
   toolchains, matching the recompiler and reshine. If Track A is ever built
   with a compiler where `CPUState` is compiled differently than the runtime
   TUs, layout would differ — but since BOTH compile the same
   `runtime/include/cpu/cpu.h`, they agree by construction. No action unless the
   include path is misconfigured.
2. **`func_*` vs. address aliases.** `dolrecomp_call` tries the physical alias
   `addr | GC_RAM_BASE` when a call misses. The IPL runs cached at
   `0x81300000`, so aliasing should not fire, but confirm once execution starts.
3. **Host-call table.** `ppc_host_call`/`ctx->host_call` is the host-intercept
   hook. At M0 we install none (pure LLE). Whether the IPL boot needs any host
   intercept is TBD — expected **none** for the LLE baseline.
4. **`instruction_fallback`.** For opcodes the emitter cannot lower it calls
   `ppc_fallback_instruction` -> `ctx->instruction_fallback`. Not installed at
   M0; a hit would raise an illegal-instruction program exception (loud), which
   is the desired signal.
