# GCN Performance Enhancement Routine

This is the handoff playbook for performance work in `gcnrecomp`. It applies
the shared recompilation rules in
[`OPTIMIZATION.md`](https://github.com/mstan/recomp-ai-rules/blob/main/OPTIMIZATION.md)
to the GameCube runtime and Wind Waker bring-up.

Here, “enhancement” means a faster implementation of the same guest-visible
machine unless a change is explicitly classified and approved as an
approximation. Performance work does not weaken the LLE floor.

## Non-negotiable floor

- The real reset, IPL, DI, apploader, and title path remains runnable.
- The recompiled Gekko executes the same instructions, cycles, exceptions, and
  hardware accesses as the retained interpreter/LLE implementation.
- Accelerated GX, DSP, device, or title paths enter and leave through a named
  LLE contract.
- The reference implementation remains forceable.
- Unsupported optimized states synchronize and fall back loudly.
- Content-derived HLE candidates are keyed by executable identity plus entry
  point, never by PC alone.
- Approximation requires an explicit user-visible policy and a separately
  retained exact mode. A screenshot that “looks close” is not an exactness
  proof.

Useful policy shape for a replaceable subsystem:

| Mode | Meaning |
|---|---|
| `off` | Force the faithful implementation |
| `on` | Use validated fast paths and fall back on every miss |
| `force` | Require the fast path; fail loudly if it cannot run |
| `verify` | Run/compare the fast path against the faithful path and publish the faithful result |
| `auto` | Select only previously promoted fast paths for the current host/content identity |

## The optimization loop

### 1. Establish a representative route

Name the exact scenario and acceptance boundary before editing:

- true-reset IPL menu;
- Wind Waker title screen;
- a deterministic gameplay scene;
- a bounded co-simulation interval.

Record the framework commit, title commit, executable hash, environment knobs,
host GPU/CPU, block or frame boundary, and output hashes. Preserve the baseline
executable before rebuilding.

### 2. Gate on host load

Builds use BelowNormal priority, at most two jobs, and no short wall timeout.
Game runs use normal priority.

Before a performance run, sample
`\Processor Information(_Total)\% Processor Utility` for at least eight
seconds. At 40% or more sustained background utility, pause performance
measurement. Correctness runs may continue if they do not interfere with the
foreground workload, but their wall time is not evidence.

### 3. Measure attribution before choosing a change

Use opt-in counters that answer a specific question without taxing normal
play. Keep source attribution where multiple callers share a dispatcher.
Current instruments include:

- `GCN_DISPATCH_STATS=1` for CPU/device attribution;
- `GCN_GX_STATS=1` and `GCN_GX_PIXEL_STATS=1` for software GX cost;
- `GCN_GX_GPU_STATS=1` for resident Vulkan submission/wait/copy timing;
- resident Vulkan triangle/program/fallback summaries;
- AOT/interpreter instruction and identity summaries;
- `GCN_GX_PIPE_STATS=1` for GX pipeline sync/resume/poison transitions.

Prefer whole-route counters first, then a narrower microbenchmark. A locally
faster helper is rejected when its route-level dynamic coverage is too small.

### 4. Classify the candidate

1. **Exact host-cost removal:** caching, inlining, sharding, eliminating dead
   diagnostics, or removing artificial dispatch boundaries.
2. **Exact replacement behind an LLE API:** resident Vulkan, compiled DSP
   blocks, or a content-validated routine shim with exact state/timing
   publication.
3. **Approved approximation:** a disclosed enhancement which may differ from
   hardware and can be disabled.

Classes 1 and 2 are zero-diff candidates. Class 3 is never silently promoted
as an optimization.

### 5. Make the smallest reversible increment

- Add a same-binary disable/verification knob when practical.
- Preserve a slow path with the old semantics.
- Put invalidation at the exact guest event which makes cached state stale.
- Keep unsupported cases loud.
- Avoid title PCs in the generic runtime when decoded shape, device state, or
  content identity can describe the condition.

### 6. Prove correctness before timing

Run the narrow unit test first, then the project matrix appropriate to the
changed boundary:

- all runtime/recompiler tests;
- pinned uniform and derived XFB hashes;
- ordered MMIO/PE token/finish counts;
- interpreter-versus-native state parity;
- GPU/software co-run at EFB/XFB publication boundaries;
- deterministic TCP co-simulation for registers and RAM;
- headed screenshots and audio when user-visible output is affected.

Headless success is necessary but not sufficient. A renderer or presentation
change is not complete until headed execution reaches the same acceptance
boundary without new visible defects.

### 7. Measure fairly

Use a preserved baseline executable and the same title inputs. Run interleaved
`BASE,CAND,CAND,BASE` rounds, discard warm-up explicitly, and report medians
plus the individual samples. Do not compare unrelated rebuilds, different PGO
profiles, different background load, or different scene coverage.

Report both:

- route throughput (frames, `GXSetDrawDone` completions, blocks, or guest time
  per wall second);
- remaining headroom attribution and fallback coverage.

### 8. Promote or reject

An exact candidate is promoted only when:

- every required correctness gate passes;
- the headed path passes when applicable;
- it improves the representative route, not only a microbenchmark;
- the LLE/reference path remains forceable;
- the measurement and rejected alternatives are documented.

Keep negative results. They prevent the next agent from repeating plausible
but unproductive work.

## Exercised example: GX scanner resume boundary

### Baseline observation

The archived headed Wind Waker run in
`captures/checkin-headed/runtime.err.log` reported:

```text
gx-pipe: scanner carry overflow (97+32) — BUG; permanent synchronous fallback
```

The scanner owns a 128-byte carry and receives 32-byte gather-pipe chunks.
After an unsizable top-level primitive moves decode temporarily to the
synchronous path, the old resume test accepted any staging remainder at or
below 128 bytes. A 97-byte remainder therefore resumed even though the next
gather required 129 bytes. The following push poisoned the pipeline for the
rest of the run, removing CPU/GX overlap.

### Exact change

Resume is now allowed only when:

```text
carry_len <= carry_capacity - next_gather_size
```

With the current sizes, 96 bytes resumes and 97 bytes remains synchronous for
another tick. The same FIFO decoder consumes the same bytes in the same order;
the change only avoids selecting asynchronous hosting before its buffer
precondition is true.

`gcn_gx_pipeline_carry_can_resume()` centralizes that precondition.
`test_gx_pipeline` proves the 96/97 boundary and undersized-capacity case.

`GCN_GX_PIPE_STATS=1` emits:

```text
[gx-pipe-stats] first resume deferral carry=... next-gather=... capacity=...
[gx-pipe-stats] sync=... defer=... resume=... poison=... max-deferred-carry=...
```

Acceptance for the title route is:

- the historical 97-byte case is observed as a deferral;
- execution later resumes;
- `poison=0`;
- no `scanner carry overflow` appears;
- the same title boundary is reached;
- a quiet-host interleaved timing run shows whether restored overlap produces
  a measurable route gain.

### Baseline-reproduction instrument

`GCN_GX_PIPE_UNSAFE_RESUME=1` reproduces the old, known-buggy resume policy
(`carry_len <= carry_capacity`, no headroom reserved for the next gather)
from the same binary, so the two policies can be A/B measured without a
separate build. It defaults off. Enabling it can poison the pipeline exactly
as the archived baseline did — that is its purpose, not a defect; it exists
solely for comparison, never for normal use.

The unit-level build and all 12 runtime tests pass.

### Route evidence and timing (2026-08-02/03)

Same-binary interleaved A/B (`A` = `GCN_GX_PIPE_UNSAFE_RESUME=1` old gate,
`B` = corrected gate), headless true-reset Wind Waker, `GCN_WINDOW=0`,
`GCN_THROTTLE=0`, fixed `GCN_MAX_BLOCKS=110000000`, `GCN_GX_PIPE_STATS=1`,
order `A,B,B,A`, host-load gate sampled >=8 s below 40% before every arm
(observed 27.6-35.3%). Binary
`runtime/build-windwaker/gcn_boot.exe` SHA256
`68dceda9acd691df18fe8b26a33bd40b3c54f039587a570309829b92e35229e7`.
Raw logs: `WindWakerRecomp/captures/perf-pipe-ab-20260802/`.

| run | arm | wall (s) | frames | poison | overflow lines |
|---|---|---|---|---|---|
| 1 | A | 87.85 | 1015 | 1 | 1 |
| 2 | B | 84.72 | 1015 | 0 | 0 |
| 3 | B | 85.62 | 1015 | 0 | 0 |
| 4 | A | 87.23 | 1015 | 1 | 1 |

- A reproduced the archived failure both times: one `scanner carry overflow
  (97+32)`, `poison=1`, pipeline permanently synchronous
  (`sync=15175 defer=0 resume=15174`).
- B held `poison=0`, zero overflow, and identical deterministic stats both
  times (`sync=31906 defer=76667 resume=31906 max-deferred-carry=1302`).
- Behavioral equivalence across all four runs: identical frame count (1015
  `GXSetDrawDone`), DL tear census (0/70468), AOT summary
  (`native=69217855 verifications=539 verified=539 failed=0`), and
  interpreter summary (`instructions=5892966 unique-misses=22790`).
- Wall time: A mean 87.54 s, B mean 85.17 s — **B improves the whole route
  by ~2.7%**, with non-overlapping samples (both B runs faster than both A
  runs). The gain is modest because on this headless route the poisoned
  synchronous pipeline still keeps the GX worker fed most of the time; the
  fix's primary value is correctness of the overlap mechanism plus the
  measured ~2.7%, not a large throughput jump.

Headed confirmation (candidate gate, knob off, `GCN_WINDOW=1`,
`GCN_THROTTLE=1`): launched on the interactive desktop
(`winsta0\default`), reached and rendered the Wind Waker title sailing
sequence (debug-surface screenshot and a `CopyFromScreen` desktop capture
both show the scene), observed the historical event exactly once as
`first resume deferral carry=97 next-gather=32 capacity=128`, completed
1353 `GXSetDrawDone` frames with
`sync=150164 defer=749420 resume=150164 poison=0 max-deferred-carry=1302`,
zero overflow lines, clean TCP `quit`.

Promoted: correctness gates plus a consistent (if small) whole-route win.

## Exercised example: fused/GPU programs Y–AD for dominant WW general shapes (2026-08-03)

`GCN_GX_TEV_CENSUS` (now printing `prog=` per bucket plus overflow
accounting) attributed 73.3% of the route's shaded pixels to 60
program-0 ("general") shapes; the top handful were single-stage
untextured fills. Six programs were added following the T–X template:
Y(25)/Z(26)/AA(27) as fused CPU+GPU folds (zt off), AB(28)/AC(29)/AD(30)
GPU-only (zt on; software stays the differential authority). Y and Z
deliberately do not pin `alpha_update` — the finish helper branches on
the live flag, absorbing two census-duplicate shape pairs each. All folds
brute-force verified against the general combiner/blend transcriptions
(529,153 + 16,777,216 cases, 0 mismatches).

New instrument: `GCN_GX_XFB_HASH=1` chains an FNV-1a-64 over every XFB
publication (backend-agnostic boundary shared by the software copy and
the resident materialize path) — the route-level byte-exactness gate this
file previously lacked.

Runtime fix that fell out of gating: `boot.c` silently discarded a
nonzero `GCN_MAX_BLOCKS` whenever a debug port or window was active,
making headed runs unbounded; an explicit budget is now honored
everywhere (0 still means unbounded).

Corun caught a real GPU bug the offline harness could not: program AB
(blend disabled, combiner alpha unconditionally 0) was missing from the
shader's blend-passthrough exception list, so the general alpha-blend
branch computed `sa=0, da=256` → draw silently became a no-op
(`gpu=0` vs software color; 34 divergences). Fixed by adding AB to the
passthrough set. A second real defect fixed en route: corun tile blame
was recorded before the resident path knew whether the draw actually
stayed on the GPU. After both fixes: corun over the full bounded route =
0 divergences in 1005 plane checks.

Evidence (route = true-reset Wind Waker, `GCN_MAX_BLOCKS=110000000`,
logs in `WindWakerRecomp/captures/perf-fused-20260803/`):

- Census delta: general pixel share 73.3% → 18.3% (139.7M → 34.9M of
  190.5M px); fused share 23.3% → 63.6%.
- Byte-exactness: fused vs `GCN_GX_NO_FUSED` XFB chains identical
  (`ed27f20acbdfe1d0`, 1338 publications), before and after the AB fix.
- Headless timing (interleaved BASE,CAND,CAND,BASE, true baseline binary
  rebuilt from HEAD): **neutral** — 80.21s vs 80.15s (0.07%, inside
  noise). Recorded as a rejection of the headless-throughput claim: that
  route is CPU-bound and the GX worker savings hide in overlap slack.
- Headed timing (Vulkan resident active, same interleave, both arms
  carrying the identical boot.c fix): BASE 94/98s vs CAND 85/88s —
  **9.9% faster (10.57 → 11.73 fps)**, identical frames/pipe-stats/
  poison=0 across arms. The win comes from the giant-area draws going
  GPU-resident (117,273 → 119,550 queued triangles — few draws, ~100M
  pixels) plus the fused CPU folds accelerating the remaining software
  raster.

Promoted on: full gate matrix (ctest 12/12, XFB chain equality, corun
zero-divergence, headed route win) with the headless-neutral result
retained as a negative datum.

## Exercised example: native-miss page-CRC memo (2026-08-03)

`GCN_DISPATCH_TOPPC` on the uniform 110M-block WW route attributed 27.2%
of all pc-attributed cycles to the three blocks of one dynamically-written
icbi flush loop at `0x812FFF80` (~35K host cycles per block entry, 622,592
entries per PC). The interpreter batching shipped at fa9c162 never fires in
uniform mode (the pre-expired `cycle_deadline` zeroes its budget), but the
cost wasn't the loop at all: `gcn_interpreter_note_native_miss` recomputed
a full 4 KiB page CRC32 on EVERY native miss — the dedup key is (pc, crc) —
so every interpreted-fallback block on the route paid ~33K cycles of
hashing before executing one instruction.

Fix: `native_code.c` keeps a read-and-clear per-page content-staleness
bitmap fed by the existing invalidation funnel plus a new
`gcn_native_code_content_dirty` entry point for writers that change bytes
without touching the icache fence (dcbz's zeroing store, gather-pipe
redirect bursts, GX XFB/EFB→RAM copies, ARAM→MEM1 and EXI memcard DMA).
`interpreter.c` memoizes the page CRC and rehashes only on staleness;
ROM-window pages are seen-once. Identity semantics are deliberately
icache-coherent: plain inlined guest stores refresh neither the native
fence nor the identity (see the `miss_page_crc` comment for the full
contract).

Evidence (same env both arms, uniform route, stats+toppc+journal on):

- Wall: **94.66s → 33.39s (−64.7%)**; block-exec dispatch share
  73.5% → 27.4%; the icbi cluster 27.2% → 5.0% of pc-attributed cycles.
- Byte-exactness: golden XFB chain `ed27f20acbdfe1d0`, 1338 publications,
  1015 frames, poison=0 — identical before/after.
- Identity: distinct missed-pc sets bit-identical; ~3,900 baseline-only
  (pc, crc) variants were pure low-mem/heap data churn between misses
  (junk identities the memo intentionally stops minting).
- Tests: runtime ctest 14/14 (new `page_crc_memo_identity` covers memo
  reuse, store/dcbz/icbi/reset staleness, neighbor-page isolation, and
  that dcbz dirties identity without touching the native fence).

## Exercised example: general TEV program phases 1a/1b (2026-08-03)

Five audited passes (docs/GX_GENERAL_TEV.md is the spec; 0f20e67,
ecd1e10, 11ae141, c6af812): draw packet 128→272 words packing raw BP
windows; a full general TEV pixel pipeline in gx_draw_f.comp (program
31) transcribed function-for-function from gx_raster.c and brute-force
verified (~1.05B combiner/blend cases + 199M fog cases, 0 mismatches);
eligibility gate + 3-level GCN_GX_GENERAL_TEV lever; two texture slots,
8 formats + TLUT arena + CMPR.

Hard-won exactness facts (now standing rules):
- **Vulkan FDiv is ~2.5 ULP; x86 divss is correctly rounded.** A 1-ULP
  reciprocal difference flipped an s17.7 texel index and moved the XFB
  chain. exact_rcp/exact_div (Newton+Markstein on `precise fma()`,
  correctly rounded by construction) back EVERY GPU divide that mirrors
  a CPU divide, including long-shipped programs' texture_uv(). Use them
  for any future GPU float path.
- Permanent GCN_GX_GENERAL_DEBUG_XY="x,y" instrumentation dumps matched
  CPU/GPU per-pixel intermediates (bit patterns) — this is how the ULP
  bug was pinned in one pass instead of guessed at.
- The differential/validate knobs are a no-op in resident mode without
  GCN_GX_BACKEND=vulkan-shadow.

Measured outcomes (fixed 110M-block headed route):
- Phase 1a (default): fallbacks 538,901 → 535,166, wall 30.5s → 25.9s,
  chain golden at every knob level, corun 1118 plane checks / 0.
- Phase 1b (fog+CMPR): exact (all gates green) but a consistent
  ~12-13% wall REGRESSION when resident (interleaved 6-run A/B, median
  29.44s vs 25.69s): ~124K extra tiny GPU triangles cost more dispatch
  than their saved synchronizations recover. Shipped opt-in (level 1),
  default stays at the phase-1a gate (level 2). Recorded as the
  motivating datum for a resident tiny-draw batching pass — the next
  designed lever; revisit the default when it lands.

## Exercised example: immutable texture/TLUT staging epochs (2026-08-09)

`GCN_GX_VK_SUBMIT_STATS=1` first partitioned every resident submit by its
immediate trigger. On the fixed snapshot-resumed Wind Waker suffix, exact
general-TEV level 1 made 1,881 command-buffer submissions: 929 texture
overwrites, 682 `GXSetDrawDone` boundaries, 225 unsupported-triangle syncs,
38 pending-RAM overlaps, five CPU EFB reads, and two pipeline drains. The
fixed texture slots were therefore a real batching barrier, but not the only
one.

`GCN_GX_VK_TEXTURE_COW=1` makes texture and TLUT uploads immutable within
bounded staging epochs (4 MiB and 64 KiB by default). A changed cache entry
gets a new aligned offset, while every already-recorded draw packet retains
the old scalar offset it captured. Arena exhaustion submits, fences, and
materializes through the existing exact boundary before advancing the epoch
and reusing offset zero. A bounded binding-stability retry prevents a later
texture/TLUT rollover from invalidating an earlier binding for the same
not-yet-recorded draw. Unsupported or impossible binding sets still fall back
to the faithful software path.

`GCN_GX_VK_TEXTURE_COW_BYTES` and `GCN_GX_VK_TLUT_COW_BYTES` are diagnostic
arena caps used to force rollover coverage; they apply only when COW is on.
The optimization remains opt-in pending broader release integration.

Evidence (72,467,144-block suffix, 683 new `GXSetDrawDone` events, title pin
`a5a8937`, golden chain `1338/ed27f20acbdfe1d0`):

- Same-binary unthrottled capacity, with only the non-architectural WASAPI
  sink detached (`GCN_AUDIO=0`; DSP-LLE/AID still execute): OFF 9.17/9.26 s,
  ON 7.79/7.71 s. Median-equivalent capacity rises from 74.12 to 88.13
  DrawDone/s (13.49 to 11.35 ms/event), about 19% more throughput.
- Resident work rises from 129,823 to 135,940 triangles while submissions fall
  from 1,881 to 895 and synchronized fallbacks fall from 411,355 to 405,238.
- Audio-on runs remain intentionally paced by 436,840 samples / 32 kHz =
  13.651 s of guest PCM; accepted COW samples had zero underruns and zero
  drops. Raw audio-on wall FPS is therefore not a headroom metric.
- Full corun: 522 plane checks, zero divergences. Resident EFB-to-texture
  verification: 76 comparisons, zero mismatches. Runtime tests: 14/14 in both
  Vulkan and `GCN_VULKAN=OFF` builds.
- A forced 512 KiB texture epoch exercised 68 rollovers, including 34 with
  live work requiring a command submission, and retained the golden chain,
  frame/draw/native/interpreter counts, and clean shutdown.

## Wind Waker performance burndown after this exemplar

The most recent headed title-screen log attributes the urgent work as follows:

1. **Restore GX overlap.** Eliminate the `97+32` permanent pipeline poison and
   verify that recoverable top-level primitives continue to re-seed safely.
2. **Expand exact resident GX coverage.** Roughly 5.4 million draws fell back
   synchronously, dominated by general/program-0 triangle state. Implement the
   observed state behind `gx_render`, co-run against software, and retain loud
   fallback.
3. **Implement observed EFB-copy states.** The title repeatedly uses copy
   states `0x01023B` and `0x010263`; synchronize/corun before promotion.
4. **Reduce GPU publication joins.** Keep EFB/XFB data resident until a real
   guest observation boundary; never delay PE/token/CPU-read visibility.
5. **Close interpreter coverage.** The headed checkpoint still executed about
   27.0 million interpreter instructions with 24,600 unique misses versus
   about 298.6 million native title dispatches.
6. **Re-profile CPU/dispatch only after GX fallback collapses.** Then apply the
   proven `ndsrecomp` sequence: exact RAM fast paths, generation-aware dispatch
   caches, source-aware fallthrough attribution, validated superblocks, and
   scoped PGO.
7. **Consider title HLE last.** Use content identity, declared live state,
   `verify` mode, and exact LLE fallback. Prefer platform-wide exact work while
   it dominates.

## Clean handoff checklist

Before another agent takes over:

- stop any `gcn_boot` process cleanly with the TCP `quit` command;
- record current system load and whether timing is admissible;
- include the exact build/test/run commands and log paths;
- commit framework changes to `master` and push `origin/master`;
- update `WindWakerRecomp/gcnrecomp.lock`, its handoff note, and push its
  `master`;
- leave generated code, ISO/firmware, captures, Ghidra databases, and build
  products untracked;
- state which checks are complete and which remain pending.

Do not erase a failed experiment or silently substitute a narrower route. The
next agent should be able to reproduce the evidence, reject the candidate, or
continue from the exact stopping point.
