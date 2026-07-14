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

## E1 result + evidence-driven re-plan (2026-07-13, post-E1)

E1 landed exactness-clean (all four goldens bit-identical, oracle counts
unchanged, ctest 12/12) but measured FLAT: 3.373 s vs 3.332 s baseline
(overlapping min-of-6 distributions). Two follow-up experiments explain it:

1. Fresh `GCN_DISPATCH_STATS` attribution (the recorded 47.8/43.1 split was
   STALE — it predated the fused_pixel_E/F + efb_clear SSE2 commit):
   **block-exec 48.6 / gx 23.0 / dsp 19.7 / other 8.8**. The DSP is the #2
   bucket: `[dsp-stats]` shows 240k flush calls × ~62 interpreted ROM-idle
   steps (pc=0x0033 mail-wait, NOT halted so halt-skip can't help) ≈ 0.53 s.
2. Micro-model variant C (scratchpad alias_c.c): write-through GPR locals
   emit MORE instructions (154→166) for only 3 fewer memory refs. L1-hot
   ctx loads are ~free under OOO; the aliasing tax was never the bottleneck.

Consequences:
- **E3 (GPR promotion) demoted** — micro-evidence says don't pay the
  139-site emitter refactor for it. Block-exec is more plausibly I-cache/
  layout/branch bound (1.3 MB single-function TUs, dense entry switches),
  which is exactly PGO's domain → **E2 (PGO) is the next codegen move**,
  and it should also re-decide the E1 always_inline calls with real counts.
- **DSP worker thread promoted to #2** (it was demoted on the stale ~9%
  devices number): grant/drain design implemented behind GCN_DSP_THREAD=1 —
  batch-cap grants run async on a worker, every CPU observation path drains
  first; known risk = interrupt-latch lag ≤ one batch window, arbitrated by
  the standing gates. Expected recovery ≈ up to ~17% of wall.
- GX at 23% still gets the AVX2 pass (in flight), expected 1.3-1.8× on the
  fused loops ≈ 5-8% of wall.

## E2 (PGO) result — the win (2026-07-13)

Two-phase build (`-DGCN_PGO=GEN` → train on uniform 8M + derived 24M/56M +
one thread-off derived 12M → `-DGCN_PGO=USE`, 201 .gcda TU profiles) in a
separate `runtime/build-pgo` dir; the plain build stays the dev default.
Result: **all four golden hashes bit-identical, oracle counts unchanged, and
min-of-6 interleaved 12M derived 3.475 → 2.986 s (~14%; every PGO sample
beat every plain sample)**. vs the session-best plain 3.329 s it is still
~10%. This confirms the layout/branch-weight hypothesis for the block-exec
plateau (E1's memory-traffic theory measured flat; PGO's whole-program
reshaping is what moved it). Attribution on the PGO binary: block-exec 51.6 /
gx 20.8 / dsp 19.0 / other ~8.5 (same shape, smaller pie).

Campaign scoreboard (12M derived, HIGH-priority min): pre-campaign 3.332 s →
post E1+AVX2+DSP-thread+PGO ≈ 2.99 s (measured on a noisier machine — the
honest gain is ~10-14%), i.e. derived ≈ 0.50× real-time. Remaining distance
to 1.0× is architectural: the CPU/GX pipeline (G3, ceiling 1.76×) is the
next campaign phase; G2 tile tuning is the smaller follow-up.

## Rejected / deferred

- musttail chunk chaining: GCC 15 feature; dispatch is per-quantum (measure
  first — likely <3%). Deferred.
- computed-goto entry instead of switch: jump table already; once per entry.
- -O3 / -fno-jump-tables / crossjumping flags: measured honest negatives in
  Phase D.
- Whole-program `restrict ctx`: soundness (above).

## G3 finalized + G2 result (2026-07-13, `70e2b80` follow-up)

Final-source G3 A/B on the plain RelWithDebInfo build, 12M derived blocks,
HIGH-priority interleaved 8-run sequence:

- synchronous: min 4.116 s, median 4.457 s, average 4.540 s;
- pipeline: min 3.723 s, median 3.854 s, average 3.839 s;
- average whole-boot improvement: **15.5%** (every pipeline sample beat every
  synchronous sample).

The host was under variable background load, so this establishes direction and
default-worthiness rather than a clean final scoreboard. G3 is now default on;
`GCN_GX_PIPELINE=0` retains the synchronous diagnostic path. Before the flip,
the default path held all four pinned XFB hashes (derived 24M/56M repeated),
both oracle counts, 10 repeated clean shutdowns, and interactive window/debug
capture. An adversarial thread audit found and fixed the publication gaps that
the initial opt-in gates did not cover: live VI field capture and debug RAM
access now drain, shutdown drains/quits/joins before RAM teardown, worker
failure cannot become an infinite wait, and the idle worker uses a changed
wake epoch so `WakeByAddressAll` cannot be lost.

G2 was measured and rejected rather than landed. All 8x8, 16x16, 32x8 and
32x16 tile shapes produced the exact 5M golden, but none beat the existing
dynamic full-width 2-pixel-row scheduler; 32-wide tiles were clearly worse.
Grouped row claims of 2/4/8 also lost to one-row claims as load imbalance grew.
Raster participant counts 4/6/7/8 did not justify reserving a core; 8 remained
best on average. The experimental tile/claim code was removed completely.

The next honest scoreboard is the current-source PGO retrain stacked with
default G3. Remaining class-level candidates, in measurement order, are:
profile-fed PPC hot-region/direct-exit emission, a guarded DSP basic-block
compiler with interpreter fallback, and integer-only SIMD within fully covered
fused raster spans. None is authorized to skip LLE work or alter publication
timing; each needs an env-gated micro A/B before a full gate campaign.

## Determinism correction + DSP interpreter work (2026-07-13)

A longer adversarial stress matrix found two rare framebuffer/CPU-state
outliers with the asynchronous DSP worker. The worker published a DSP-to-CPU
interrupt whenever the host worker happened to reach it, so architectural
visibility could move with host scheduling even though ordinary golden runs
usually passed. A deterministic publication experiment changed the pinned
24M derived framebuffer and was rejected rather than re-pinned. The result is
deliberate: synchronous DSP-LLE is again the default, and
`GCN_DSP_THREAD=1` is an explicitly experimental diagnostic/performance path.

The last complete current-source PGO cycle before the DSP source changed gave
the following noisy but fully interleaved 12M derived result:

- plain: min 3.643 s, median 4.272 s, average 4.236 s;
- PGO: min 3.295 s, median 3.771 s, average 3.720 s;
- average improvement: **12.2%**, with every paired PGO run winning.

PGO attribution was block-exec 51.2%, DSP 21.0%, GX 19.6%, and other devices
8.2%. That PGO executable is now stale and is evidence only; the final release
must be retrained after the current source and regenerated PPC output settle.

Two exact DSP interpreter optimizations followed. First, the main handler,
extension handler, and extension-presence flag were fused into one decoded
opcode-table lookup. Across three identical 12M derived runs, the interpreter
executed exactly 15,141,752 instructions each time while its cumulative core
time fell from about 580 ms to 503 ms (**13.2% inside DSP**). Second, GCC LTO
was scoped only to the small vendored DSP C++ library, never the giant generated
PPC chunks. The same instruction count then took 366.6 ms (**27.2% beyond the
fused table; 36.8% versus the original interpreter**). Whole-boot interleaved
A/B for DSP-only LTO was 3.533 s to 3.401 s by average (3.7%) and 3.552 s to
3.385 s by median (4.7%). The pinned 24M derived framebuffer remained
`bea0d67fb1dbba07be06c815c5c4d451`; no firmware work is skipped.

## Static fused GX row scanners rejected (2026-07-13)

The next measured GX experiment removed the remaining per-pixel indirect call
through `s_cfg.fused`. Six statically-bound A--F row scanners were selected once
per triangle; object-code audit confirmed direct calls to the matching fused
shader and zero indirect calls inside those pixel loops. The complete generic
path remained available in the same binary behind `GCN_GX_STATIC_FUSED=0`.

An adversarial thread audit caught and prevented a stale-worker design error
before benchmarking: a worker must claim a row from the packed, versioned fork
word before reading the job's scanner, because a late worker may help a newer
fork. The corrected experiment observed that publication rule and passed exact
same-binary gates:

- uniform 5M, feature off and on: `fee16a8b6143e82698169b9bfba77801`;
- derived 24M, feature on twice: `bea0d67fb1dbba07be06c815c5c4d451`.

The performance result was negative. Twelve normal-priority 12M-derived boots
used an `off,on,on,off` pattern repeated three times (six samples per mode):

- off: min 3.364487 s, median 3.480585 s, average 3.519462 s;
- on: min 3.434695 s, median 3.668312 s, average 3.649723 s;
- enabled was 3.701% slower by average and 5.394% slower by median; off won
  4/6 adjacent comparisons and 2/3 ABBA blocks.

The cloned scanner hot text/i-cache cost outweighed removal of the indirect
pixel call. The entire experiment and knob were removed; no dormant branch,
row indirection, or specialized scanner remains in the runtime.

## DSP PC-indexed decoded-op cache rejected (2026-07-14)

A compact PC-indexed DSP dispatch cache was measured and rejected. The
existing fused decoded-op table occupies 2 MiB because each decoded opcode
contains two 16-byte C++ member-function pointers. The experiment copied all
4,096 IRAM and 4,096 IROM entries into a 256 KiB per-core table, rebuilt it on
initialization and every existing `CodeLoaded` invalidation, and selected it
behind `GCN_DSP_PC_DECODE=1`. Every firmware instruction, operand fetch, cycle,
memory access, and interrupt remained present.

Adversarial review caught a re-entrant invalidation hazard before gating: a DSP
handler can synchronously start IDMA through an IFX write, rebuilding the cache
while the current instruction is executing. The gated prototype first
snapshotted both handlers by value, and object-code review also forced the helper
back inline so the A/B did not add a host call to every DSP instruction.

Same-binary exactness held: uniform 5M OFF and ON both produced
`fee16a8b6143e82698169b9bfba77801`; derived 24M ON repeated produced
`bea0d67fb1dbba07be06c815c5c4d451`. Twelve normal-priority 12M-derived boots
used OFF,ON,ON,OFF repeated three times:

- OFF: min 3.904915 s, median 4.032773 s, average 4.013476 s;
- ON: min 3.933949 s, median 4.503864 s, average 4.394414 s;
- enabled was 9.491% slower by average and 11.682% slower by median;
- OFF won 5/6 adjacent comparisons and all three ABBA blocks.

The smaller table did not offset its larger hot dispatch path and extra cache
footprint. The implementation and environment knob were removed completely.

## Current-source PGO acceptance (2026-07-14)

PGO was retrained after the final emitter, GX-pipeline, and DSP-LLE source
changes. The GEN build was trained on uniform 8M plus derived 24M/56M and a
derived 12M synchronous-DSP run, producing 201 non-empty profiles. The USE
build consumed 203 profile-guided translation units without missing-profile or
coverage-mismatch warnings.

The retrained binary passed the complete acceptance matrix without re-pinning:

- uniform 5M: `fee16a8b6143e82698169b9bfba77801`;
- uniform 8M: `a94db4e05555e87a03704c79def96005`;
- derived 24M twice: `bea0d67fb1dbba07be06c815c5c4d451`;
- derived 56M twice: `5227ee9c3f4ddd0f7486c2ad508b3e7a`;
- uniform oracle: 19,355 matches / 3 resyncs;
- derived oracle: 19,354 matches / 4 resyncs, with only the standing terminal
  PI-order divergence.

Normal-priority interleaved ABBA testing at 12M derived blocks measured:

- plain: min 3.179626 s, median 3.234967 s, average 3.246550 s;
- PGO: min 2.757948 s, median 2.844289 s, average 2.960318 s;
- PGO improved the median by **12.077%** and the average by **8.816%**;
- PGO won 5/6 adjacent comparisons and all three ABBA blocks.

This is a real, exact win and is the accepted release build configuration.
It does not close the strict derived-timing target: that mode remains roughly
0.50--0.55x real time by the earlier block-budget estimate. That estimate is
not a frame-rate measurement and must not be used to claim a real-time UI.

A subsequent current-PGO windowed acceptance measured 5,787 firmware
`GXSetDrawDone` completions over 605.769 wall seconds: **9.55 FPS** in default
uniform mode. A separate 60.008-second unthrottled, headless derived-mode run
measured 1,219 completions: **20.31 FPS**. The throttle does not sleep when the
runtime is behind, so neither result is a pacing artifact. Calendar time in the
uniform run advanced only 9 displayed seconds across 60 wall seconds, an
independent visible confirmation of the shortfall.

Current uniform attribution at 12M blocks is GX 55.2%, DSP 19.4%, recompiled
CPU 15.1%, VI 4.4%, AI 3.0%, and DI 2.9%. Therefore the 60 FPS goal is **not
met**, and GX cadence/software raster work is the first performance priority.
An event-deadline feasibility audit found that active GX, DI, DSP/AID,
timebase, and in-call MMIO are fixed fences today; only measurement counters
are justified before any deadline change. Idle skipping and result skipping
remain forbidden.

## GX characterization and bilinear reuse rejected (2026-07-14)

Fresh current-binary profiling resolved the measured UI bottleneck. At a
matched 10,485,760 synchronous GX ticks, uniform and derived modes respectively
spent 94.1% and 93.7% of GX time drawing; FIFO plus decode was only 0.8% and
1.0%, and EFB copy/clear was 5.1% and 5.4%. Triangle scan/pixel work was
93.3--94.2% of draw time, with the largest `2^14` bounding-box bucket alone
accounting for roughly 74% of triangle time. At least 99.2% of enabled GX ticks
drained no FIFO chunk, but empty-call removal cannot solve the derived-mode
raster ceiling and changing cadence would alter current CP/PE timing.

The first exact raster candidate added an opt-in, per-worker memo of the
immediately previous bilinear 2x2 texel footprint. It retained draw generation,
texmap, all four wrapped coordinates, and exact RGBA bytes; identical footprints
avoided four probes into the existing per-draw texel cache, and a one-texel
right shift reused the old right column while obtaining the new column through
the original cache path. Pixel, TEV, blend, EFB-write, command, and first-use
decode order stayed unchanged.

The overlap was real: a derived 12M diagnostic observed 14,656,205 bilinear
samples, with 36.350% identical-footprint reuse and 27.751% right-shift reuse.
It avoided 29,444,406 of 58,624,820 logical large-cache probes (50.225%). The
wall result was nevertheless negative. A same-binary `OFF,ON,ON,OFF` sequence
repeated three times measured:

- OFF: min 4.035304 s, median 4.314476 s, average 4.298559 s;
- ON: min 4.286930 s, median 4.477838 s, average 4.471011 s;
- ON was 3.786% slower by median and 4.012% slower by average;
- OFF won 4/6 adjacent comparisons and all three ABBA blocks.

The extra hot-path comparisons and footprint copies cost more than the avoided
cache probes. The entire implementation, knob, counters, ABI fields, and object
growth were removed before the baseline rebuild; no dormant branch remains.
Future raster work must reduce arithmetic across multiple pixels (compact SIMD)
or amortize existing fork/join work without cloning scanners, not add another
scalar memo layer.
