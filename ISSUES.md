# Known Issues and Validation Status

Status as of 2026-08-03 (Wind Waker 60-FPS burndown session; supersedes the
2026-07-15 checkpoint). Framework at `8a2e0d2` on `experiment/moderngekko-gpl`
(= `master`); title repo pin `a5a8937`.

## Where we are

Standing goal (`.claude/GOAL.md`): 60 FPS sustained with headroom. On the
fixed 110M-block headed WW route (true reset → IPL menu → main.dol → title
sailing), all byte-exact against the golden XFB chain `ed27f20acbdfe1d0`
(1338 publications / 1015 frames / poison=0):

| Metric | 2026-08-03 morning | now |
|---|---|---|
| Route wall | 94.7 s | 25.9 s (24.3 s with PGO opt-in) |
| Headed fps, route average | 11.7 | ~39–42 |
| Synchronized GX fallbacks | 538,977 | 535,166 (411,347 at `GCN_GX_GENERAL_TEV=1`) |
| Dispatch wall split | block-exec-dominated | balanced ~31/28/30 CPU/DSP/GX |

## Landed this session (each with its full gate matrix; see commit messages
## and ENHANCEMENTS.md exemplar entries for evidence)

- `00337d4` **Native-miss page-CRC memo** — the dominant hidden cost: a 4 KiB
  CRC32 per interpreted-fallback dispatch. Route wall −64.7%. Content-stale
  bitmap in the `gcn_native_code_invalidate` funnel + `content_dirty` for
  non-fence RAM writers (dcbz, gather pipe, GX copies, ARAM/memcard DMA).
  Identity is icache-coherent by contract.
- `3b58247` **Resident EFB→texture copies** (0x01023B RG8 / 0x010263 RGBA8),
  bit-exact tiled integer encode, `GCN_GX_EFB_COPY_VERIFY` compare knob;
  review-found fix: texture snapshots flush overlapping pending copies.
- `11ae141` / `c6af812` **General TEV program (program 31)** — full integer
  TEV pipeline in `gx_draw_f.comp`, ~1.25 B brute-force cases, per-triangle
  differential, corun, chain-golden at every knob level. Key discovery:
  Vulkan FDiv is ~2.5 ULP vs x86's correctly-rounded divss — `exact_rcp`/
  `exact_div` (Newton+Markstein on `precise fma()`) now back every GPU
  divide that mirrors a CPU divide. `GCN_GX_GENERAL_TEV`: 0=off, 1=full
  (fog+CMPR resident — exact but measured ~12–13% wall REGRESSION from tiny-
  draw dispatch, shipped opt-in), 2=phase-1a gate (default).
- **PGO retrained on the WW route** — `runtime/build-pgo-windwaker`
  (gitignored), select via `GCN_RUNTIME_BUILD_DIR`; ~5.8% median,
  chain-exact. Recipe in title `docs/BRINGUP.md`. Supersedes the old
  `runtime/build-pgo` mixed-path cache note from the July checkpoint.
- `72fd15a`..`8a2e0d2` **Snapshot/resume + BIOS skip** (docs/
  SNAPSHOT_RESUME.md) — PROVEN complete machine-state capture: save at the
  `0x80003140` park (block 37,532,856), resume seeds the XFB hash chain and
  completes the standing golden byte-exactly (341/341 overlapping
  publications identical; corun 456 checks/0; independently reproduced).
  Iteration workflow: save once (`GCN_CHECKPOINT_PC=0x80003140
  GCN_SNAPSHOT_SAVE=<p> GCN_SNAPSHOT_EXIT=1`), then every run
  `GCN_SNAPSHOT_LOAD=<p>` with budget 72,467,144 — skips ~1/3 of wall.

## Outstanding — performance (next levers, in order)

1. **Resident tiny-draw batching.** The motivating datum: fog/CMPR residency
   (level 1) moves ~124K tiny triangles onto the GPU and LOSES ~12–13% wall
   to per-draw dispatch overhead despite killing their syncs. Batching flips
   that lever positive and attacks the remaining ~535K synchronized
   fallbacks. Revisit the `GCN_GX_GENERAL_TEV` default when it lands.
2. **DSP AOT.** DSP-LLE is ~28% of dispatch wall and already flush-batched
   (4096-cycle cap); the interpreter core itself is the remaining cost.
   DOLPHIN_AUDIT.md CPU item 5.
3. **General TEV phases 2+**: mip sampling, z-texture, indirect stages /
   bump-alpha ras channels, >2 texgens — each still a loud per-draw
   fallback with a census bucket (2.4%/0.9%/~0% of pixels respectively).
4. Residual icbi-cluster interpreter cost (~6% of pc-cycles, uniform mode
   never fires the fa9c162 batcher — deadline pre-expired by design).

## Outstanding — correctness / validation

- **Intro→menu fly-in corun divergence** (pre-existing, IPL menu route
  only): frames ~879–914, planes B/D/K/L/N, gpu≈2×sw alpha-halving
  signature. Not seen on the WW route.
- **Latent PE-fence-poison hang note** in gx.c's drain loop (pre-existing
  handoff note; never reproduced this session).
- **Snapshot identity hash is provisional** — a MEM1 content hash, not a
  real disc/DOL identity (no DOL-load path exists to hash against yet).
  Flagged in `snapshot.h`. Required before the production BIOS-skip knob
  ships to users; `GCN_SNAPSHOT_FORCE=1` documents the iteration-tier
  bypass.
- **Production BIOS-skip UX** — mechanism proven; still needs the shipped
  packaging (a first-boot auto-capture flow or distributed per-title
  snapshot, plus the identity hash above).
- **Mid-game snapshot capture** untested (format deliberately doesn't
  preclude it; the drain-assert is the gate).
- **July checkpoint items still open**: WGL flicker final validation
  (clean unobstructed 60 s capture on the inner Game Play screen),
  audio-enabled endurance run (pacing + zero sustained WASAPI underruns
  together), window-resize stress test, in-menu memcard copy/delete
  interactive acceptance pass.

## Process / tooling notes (this session)

- Builds ALWAYS BelowNormal priority, max 2 jobs. Launch runs via bash
  with file redirection — a PowerShell dual-pipe synchronous redirect
  deadlocks `gcn_boot` (voluminous stderr fills the pipe; caused one real
  wedged run this session).
- Verify every headed run through the TCP debug server (`GCN_DEBUG_PORT`,
  `tools/gcn_debug_client.py ping/screenshot`) — exit-gate greps prove
  nothing about a wedged run.
- Recompiler ctest requires `C:\msys64\mingw64\bin` on PATH or 3 tests
  fail with spurious compile errors.
- The per-triangle differential knobs (`GCN_GX_VK_DRAW_VALIDATE`) are a
  silent no-op without `GCN_GX_BACKEND=vulkan-shadow`; corun requires
  `GCN_GX_BACKEND=vulkan`.
- `GCN_GX_GENERAL_DEBUG_XY="x,y"` dumps matched CPU/GPU per-pixel
  intermediates (bit patterns) — this pinned the FDiv ULP bug in one pass.
