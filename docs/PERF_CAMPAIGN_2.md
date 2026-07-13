# Perf campaign 2 — derived mode 0.45× → ≥1.0× real-time

Baseline (2026-07-13, HEAD 240841b + GL presenter WIP): derived q=96 12M-block
boot, HIGH priority min-of-6 = **3.332 s** (matches the recorded 3.30 s).
Attribution: block-exec 47.8% / gx 43.1% / devices ~9%. Reaching 1.0× needs a
2.22× whole-program speedup — **both** halves must shrink (Amdahl: zeroing
block-exec alone = 1.92×).

Sources: local `-S` A/B experiments (scratchpad alias_a/b.c) + an external
consult (ChatGPT Pro, Extra High reasoning, 2026-07-13) that independently
verified the GCC aliasing behavior on GCC 14.2 and supplied the ranking below.
Key shared finding: in emitted code **every guest-register access is a memory
op** — gcc reloads `ctx->gpr[*]` after every guest store (u8 stores alias
everything), reloads `ctx->ram` per memory op, `reserve_valid/addr` per store,
and `cycles/cycle_deadline` (2 loads) per branch site.

## Why NOT `CPUState* __restrict ctx` (the tempting one-liner)

`restrict` on the chunk parameter licenses gcc to cache `ctx->` fields across
u8 stores (verified, GCC 14.2 — restrict on a hoisted `ram` local alone does
NOT). But it asserts no access to CPUState/RAM through pointers not based on
`ctx` for the whole invocation, and mid-chunk MMIO slow paths reach device
handlers holding **stored** CPU pointers (e.g. gx_raster's `s_cpu->ram` EFB
copy writes, debug ring writers). A same-invocation read of bytes a device
wrote through its stored pointer = UB. Locals-promotion (below) gets the same
win soundly, so restrict-on-ctx is rejected.

## Increment plan (each step: full regen w/ GCN_CYCLES_UNIFORM=1 pin, golden
## hashes uniform 5M/8M byte-identical, derived 24M/56M per re-pin protocol,
## oracle counts, ctest, then 12M bench)

- **E1 (emitter+header, one regen):**
  1. `__attribute__((always_inline))` on the `dolrecomp_mem_*_fast` helpers
     (deterministically closes the 391 cold-heuristic non-inlined sites that
     the Phase-D2 `--param` budgets could not).
  2. Hoist `u8* const dr_ram = ctx->ram;` at function entry; fast helpers take
     the RAM pointer as a parameter (slow fallback unchanged, still gets ctx).
     Kills the per-access `ctx->ram` reload (it's a plain local, unaliasable).
  3. Promote `ctx->cycles`/`ctx->cycle_deadline` to locals `dr_cycles` /
     `dr_deadline` (deadline is const per invocation — armed in the dispatch
     loop before the call). Spill `ctx->cycles = dr_cycles;` at every
     `return`. PRECONDITION (verified by grep before landing): nothing reached
     from inside a chunk invocation (MMIO handlers, ppc_* helpers, fallback)
     reads or writes `ctx->cycles`/`cycle_deadline`.
  4. Mark the extern bus slow paths (`mem_read*/mem_write*` _cia family) and
     `ppc_fallback_instruction` `__attribute__((cold))` at their declarations
     in the emitted header (layout: pushes cold paths out of hot I-cache).
- **E2: PGO.** CMake option: `-fprofile-generate` chunk TUs → deterministic
  training run (8M uniform + 24M derived boots) → `-fprofile-use`. Fixes
  layout + remaining branch-probability guesses. Fully deterministic training
  input (the boot is bit-reproducible).
- **E3: GPR promotion (the architecture rework).** Emitter tracks, per
  superblock (single-entry region: block leaders that only local fallthrough/
  goto edges enter — the deadline-yield loops qualify), which GPRs are
  accessed; loads them into `u32 rN` locals at region entry, rewrites body
  accesses, spills dirty regs at every region exit / opaque-helper call /
  exception guard. Mid-region switch-case entries get reload stubs (case
  label loads locals then `goto` the interior hot label). CR/XER can follow
  later (lazy materialization only for mfcr/exceptions).
- **G1 (gx, parallel with E-track — different files):** AVX2 widening of the
  fused SSE2 pixel paths + efb clear. Bit-exact rules: integer ops only, keep
  exact intermediate widths/saturation, in-lane `_mm256_pack*` ordering
  handled explicitly, scalar span edges, `_mm256_movemask_epi8` empty/full/
  mixed classification, no reassociation, vzeroupper hygiene (gcc emits it).
  Gate: all four golden hashes + GCN_GX_NO_SIMD=1 A/B still byte-identical.
- **G2: tile/cache ownership tuning.** Tile width multiples of 16px (4B
  plane), 64B row alignment, per-worker stat accumulation, multiple tiles per
  claim. Bench 8×8/16×16/32×8/32×16.
- **G3 (architecture, biggest GX item): CPU/GX pipeline.** GX command
  consumption on a worker, CPU produces ahead; hard joins where the sync
  model publishes observable state: PE token/finish, EFB copies feeding
  VI field capture, GX MMIO/status reads, FIFO watermark interrupts.
  Determinism argument: if the guest never mutates in-flight source data
  (vertex/texture/dlist memory) between submission and join, results are
  identical regardless of consumer timing; golden hashes ×4 + repeated-run
  flap detection are the gate. Ceiling if perfect: 1/(0.478+0.091) = 1.76×.
- **DSP thread: LAST.** Devices ≈9% total; ceiling 1.10×. Competes with 8
  raster workers for 8 cores. Deterministic design (publish points = the
  validated flush set) is understood — implement only if CPU/GX work leaves
  idle core capacity. (Goal item kept, sequenced last deliberately.)

## Rejected / deferred

- musttail chunk chaining: GCC 15 feature; dispatch is per-quantum (measure
  first — likely <3%). Deferred.
- computed-goto entry instead of switch: jump table already; once per entry.
- -O3 / -fno-jump-tables / crossjumping flags: measured honest negatives in
  Phase D.
- Whole-program `restrict ctx`: soundness (above).
