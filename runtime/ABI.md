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

Generated loads/stores call these free functions with the `ctx` pointer **[BK
emitter.c `emit_load/emit_store/emit_fload/...`, e.g. `mem_write32(ctx, ea,
(u32)ctx->gpr[%u])`]**. Implemented in `runtime/src/memory.c`:

```c
u64  mem_read64 (CPUState*, u32 addr);
void mem_write64(CPUState*, u32 addr, u64 value);
u32  mem_read32 (CPUState*, u32 addr);
void mem_write32(CPUState*, u32 addr, u32 value);
u16  mem_read16 (CPUState*, u32 addr);
void mem_write16(CPUState*, u32 addr, u16 value);
u8   mem_read8  (CPUState*, u32 addr);
void mem_write8 (CPUState*, u32 addr, u8  value);
```

Semantics we guarantee (mirror of **[CPU]**, faithful/boring per PRINCIPLES):
- **Big-endian** value semantics against a little-endian host (swap on RAM
  access; `read_be*/write_be*` in `common/types.h`).
- MEM1 = 24 MB at `0x80000000` (`GC_RAM_BASE`); the **uncached mirror** at
  `0xC0000000` (`GC_RAM_UNCACHED`) addresses the SAME bytes.
- Any access NOT backed by flat RAM routes to `external_read/external_write`
  (the device layer). With no device installed we warn loudly and return 0 —
  never a silent fake. (We deliberately do **not** copy reshine's `mem.c`
  fake-the-answer forcing such as `0x80000044 -> 0xFFFF`.)

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
