# Known Issues and Validation Status

Status as of 2026-08-09. Local framework commit `a2a90cc` on
`experiment/moderngekko-gpl` contains the current COW experiment; it descends
from framework checkpoint `5a7a972`. WindWakerRecomp remains at title commit
`a5a8937` and still pins framework `8a2e0d2`, so the COW work is not integrated.

## Where we are

Standing goal (`beads-b29`): 60 FPS sustained with headroom. The fixed
snapshot-resumed title-sailing suffix starts at cumulative frame 332 and ends
at 1015, so its timed interval contains **683 new DrawDone events**. All
accepted runs retain the golden XFB chain `ed27f20acbdfe1d0` (1338 cumulative
publications), poison=0, and 539/539 native verifications.

`GXSetDrawDone` throughput is an unthrottled emulation-capacity proxy, not a
measurement of distinct host-presented frames. Never divide the cumulative
1015 count by suffix wall time. A 60-Hz presentation claim requires explicit
VI/presenter cadence and headed-output evidence.

| Exact level-1 metric | Fixed-slot baseline | Texture/TLUT COW opt-in |
|---|---:|---:|
| Unthrottled capacity wall (median of interleaved pair) | 9.215 s | 7.750 s |
| Capacity | 74.12 DrawDone/s | 88.13 DrawDone/s |
| Emulation work per DrawDone | 13.49 ms | 11.35 ms |
| Resident triangles / submissions | 129,823 / 1,881 | 135,940 / 895 |
| Synchronized fallbacks | 411,355 | 405,238 |

These capacity runs use `GCN_AUDIO=0`, which detaches only the
non-architectural WASAPI PCM sink; DSP-LLE, AID DMA/timing, interrupts, and
firmware execution remain active. Audio-on runs emit 436,840 samples at
32 kHz (13.651 s of guest audio) and intentionally block on the full host
ring, so their roughly 14 s wall time / 49 DrawDone/s is a realtime-audio
quality measurement, not an emulation-headroom metric. Accepted audio-on COW
samples had zero underruns and zero drops. The older true-reset 110M-block
route remains historical context: 94.7 s at the morning baseline, 25.9 s after
the 2026-08-03 burndown, and 24.3 s with the then-current PGO build.

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
- `a2a90cc` **Immutable resident texture/TLUT staging epochs** — opt-in,
  default-off COW reduced exact level-1 submissions from 1,881 to 895 and
  raised the fixed-suffix capacity result from 74.12 to 88.13 DrawDone/s.
  Golden XFB, 522/0 GPU/software plane checks, 76/0 EFB-texture checks,
  forced-small-arena rollover, and both Vulkan/non-Vulkan 14-test suites pass.
  The two-sample ABBA result is strong candidate evidence, not broad title or
  presentation acceptance.

## Outstanding — performance measurement and release integration

1. **Post-COW attribution first.** Measure current submit reasons and exclusive
   waits, epoch high-water/rollover, fallback weight, CPU/DSP/GX shares, and
   VI/presenter cadence on the same 683-event suffix. No next optimization has
   current ranking evidence.
2. **Keep the metrics separate.** Capacity clears the 66 DrawDone/s gate on
   this route. Audio-on endurance and VI/presenter-specific 60-Hz behavior are
   distinct release gates; the route produces about 50 DrawDone events per
   guest-audio second.
3. **Do not promote historical rankings.** The level-1 TEV regression, old
   ~28% DSP share, residual icbi cost, and 5.8% PGO result all predate COW.
   They remain hypotheses until a post-COW profile refreshes them.
4. **Choose the next lever from exclusive cost.** Only after attribution,
   optimize the largest measured remaining cost while preserving forceable
   software/interpreter/DSP-LLE floors and loud fallback.
5. **Promote or retain COW deliberately.** Broader title/endurance, paced
   presentation, reset/snapshot/shutdown, and force-floor coverage must precede
   default promotion and title pinning.

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
  audio-enabled endurance with guest-audio duration, host-ring full wait,
  DrawDone count, VI/presenter cadence, and zero sustained underruns/drops;
  window-resize stress; and in-menu memcard copy/delete acceptance. Audio
  endurance is a pacing/quality gate, not proof of 60-Hz presentation.

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
