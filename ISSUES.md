# Known Issues and Validation Status

Status as of 2026-08-10. Local framework HEAD `5e7c536` (docs) sits on the
COW experiment commit `a2a90cc`, on `experiment/moderngekko-gpl`.
WindWakerRecomp remains at title commit `e32ecef` and still pins framework
`8a2e0d2`, so the COW work is not integrated or pinned by the title. New this
session: framework commit `427735d`, opt-in `GCN_PRESENT_STATS`
presenter/VI cadence counters (posts/coalesced/distinct/latency, plus a TCP
`present_state` query) — validated 14/14 on both the Vulkan and non-Vulkan
suites, golden gates byte-identical, measured overhead ≤0.3% both disabled
and enabled.

## Where we are

Standing goal (`beads-b29`): 60 FPS sustained with headroom. The fixed
snapshot-resumed title-sailing suffix starts at cumulative frame 332 and ends
at 1015: 1015 cumulative minus 332 snapshot-seeded leaves **683 timed
DrawDone events**, the only valid denominator for this suffix. All accepted
runs retain the golden XFB chain `ed27f20acbdfe1d0` (1338 cumulative
publications), poison=0, and 539/539 native verifications.

`GXSetDrawDone` throughput is an unthrottled emulation-capacity proxy, not a
measurement of distinct host-presented frames. Never divide the cumulative
1015 count by suffix wall time. A 60-Hz presentation claim requires explicit
VI/presenter cadence and headed-output evidence — see "Presentation cadence"
below for the first measurement of that evidence.

| Exact level-1 metric | Fixed-slot baseline | Texture/TLUT COW opt-in |
|---|---:|---:|
| Unthrottled capacity wall (median of 3 ABBA rounds) | 9.153 s | 8.068 s |
| Capacity | 74.62 DrawDone/s | 84.66 DrawDone/s |
| On/off wall ratio | | 0.8815 |

The 3-round ABBA capacity re-measurement (`CAPACITY3.md`, 12/12 golden; a
port-squatter first pass was invalidated and preserved rather than
discarded) reconciles with the prior single-pair result (74.12/88.13
DrawDone/s): the COW speedup replicates at **11.85% wall**, below the
earlier 2-sample 15.9% figure, which was optimistic. Both arms clear the
66 DrawDone/s gate. This remains unthrottled capacity, not FPS.

These capacity runs use `GCN_AUDIO=0`, which detaches only the
non-architectural WASAPI PCM sink; DSP-LLE, AID DMA/timing, interrupts, and
firmware execution remain active. See "Audio" below for this session's
audio-on measurements. The older true-reset 110M-block route remains
historical context: 94.7 s at the morning baseline, 25.9 s after the
2026-08-03 burndown, and 24.3 s with the then-current PGO build.

## Landed this session

- **`GCN_PRESENT_STATS=1` presenter/VI cadence counters** (`427735d`) —
  opt-in posts/coalesced/distinct-present/latency
  counters in `host_window.c`, a field-tick counter in `vi.c`, a
  teardown print for both, and a TCP `present_state` debug-server query.
  14/14 on both Vulkan and non-Vulkan ctest suites, golden gates
  byte-identical with the flag on or off, overhead ≤0.3% either way. This
  is the instrumentation that produced the "Presentation cadence" numbers
  below — the first measured (not inferred) VI/presenter cadence data for
  this route.

## Post-COW attribution (`ATTRIBUTION.md`, cross-checked against an
## independently-built `COUNTER_INVENTORY.md`)

Exclusive submit-wait ranking on the 683-event suffix:

| Wait reason | Exclusive cycles | Share |
|---|---:|---:|
| sync-unsupported-triangle | 1.757 B | 57.7% |
| flush-drawdone | 1.098 B | 36.1% |
| pending-ram-overlap | — | 4.4% |
| texture-epoch-full | — | 1.3% |
| (rest) | — | <1% |

`texture-epoch-full` corresponds to the COW arena sitting at 99.8% high-water
with 2 of 3 rollovers blocked. The fallback driver behind the dominant
`sync-unsupported-triangle` share is **not** arena pressure: 405,238 draws
fail `general_tev_eligible()` (`gx_raster.c:4106+`) outright, i.e. the
general-TEV path rejects them before arena state is ever consulted.

Main-thread shares from an INSTRUMENTED run (+24.6% overhead vs the
uninstrumented baseline; shares only, not absolute costs): block-exec 26.9%,
DSP 17.8%, gx-tick 35.9%. `gx-tick` is main-thread GX FIFO bookkeeping only —
the actual rasterization runs on the worker thread, which currently has **no
timing counter**, so its exclusive cost is unmeasured.

Known measurement artifacts (do not re-derive conclusions from these without
correcting them first):
- The resident-timing "wait%" line divides by a mismatched denominator
  (`gx_vulkan.c:2039` vs `2043`) — it is not a duty cycle as printed.
- The "xfb-memcpy" bucket includes EFB→texture copy time, not just the XFB
  memcpy.

Not available from any current counter: draw-arena drain cost, per-pixel
weight under Vulkan, and worker-thread GX exclusive time — closing these
gaps is a prerequisite for a complete attribution, not just a nice-to-have.

## Presentation cadence (`AUDIO_PRESENTATION.md` — first measurement via the
## new `GCN_PRESENT_STATS` counters)

Paced headed run (`GCN_THROTTLE=1`, vsync=1, audio=1): VI ticks at 59.7
fields/s (NTSC-correct); distinct presents ≈23.3/s; coalesced posts 3/352,
all during startup; post→present latency averages 2.11 ms, max 17.87 ms.

The key finding: **posts=352 / fields=857 is identical in both the
unthrottled and the paced run.** The ~24.6 new-frames/s guest publication
cadence is therefore route-intrinsic guest behavior — it is not presenter
loss and not an emulation-speed ceiling (the paced route runs ~47.6
DrawDone/s against a measured 84.7 DrawDone/s unthrottled capacity, so there
is large headroom left unused by this cadence). VI/field timing is
independently correct at 59.7/s. Whether 60 Hz presentation of 60 *unique*
frames per second is achievable on this route is **not established** — the
open question is whether this route section is inherently ~24 fps guest
content, not whether the presenter can keep up.

## Audio

Transport run: wall 23 s (host was under background load; recorded honestly
rather than re-run clean), wait ratio 0.230, live-sample underruns=0/drops=0.
Paced run: live-sample underruns=0/drops=0 throughout, out to 424,448+
frames.

The exit-line underrun counts printed at the end of parked runs (932/2348 in
one capture) are a **parked-window artifact, not a real underrun rate**:
once the guest reaches route end and stops producing, the sink keeps
draining, and `underrun_span` attributes every subsequent underrun back to
the final packet (436736). Any wall-time figure taken from a parked run must
use the parked marker, not the exit line, and any exit-line audio count
needs this caveat attached or it overstates underruns by orders of
magnitude. PCM fidelity itself remains **unvalidated** — there is no capture
knob in `host_audio.c` and no reference comparison has been done.

## Floors (`FLOORS.md`)

- Software-backend suffix is bit-exact golden (chain `ed27f20acbdfe1d0`),
  with no COW involvement in that path.
- DSP-LLE is synchronous by default (`dsp.c:123-135`) — confirmed by
  reading the source; there is no affirmative log line that announces this
  at runtime.
- CPU force-interpreter control is confirmed **ABSENT**. The force-floor
  gate is still not exercised — unchanged from the prior handoff.

## Outstanding — performance measurement and release integration

1. **Refresh the weighted fallback census next.** Per the attribution above,
   the dominant cost is the 405,238 `general_tev_eligible()` fallback
   rejections in `gx_raster.c:4106+`, not arena pressure. Before proposing
   batching or the next exact-TEV phase, attribute those failures by
   eligibility condition and by draw/triangle weight (a raw failure count
   does not tell you which condition to fix first).
2. **Confirm whether this route section is inherently low-fps guest
   content.** The posts=352/fields=857 identity above means the ~24.6
   frames/s cadence is not a presenter or emulation bottleneck. Compare
   against the Dolphin oracle or a different (gameplay) section to
   determine whether this is expected guest behavior for this specific
   route section.
3. **Keep the metrics separate.** Capacity clears the 66 DrawDone/s gate on
   this route (both arms). VI/field timing is correct at 59.7/s. Neither of
   those establishes 60 Hz presentation of unique frames, and audio
   endurance/quality is a distinct gate from both.
4. **Do not promote historical rankings that predate this attribution
   pass.** The level-1 TEV regression, old ~28% DSP share, residual icbi
   cost, and 5.8% PGO result all predate COW and this attribution refresh.
5. **Close the unmeasured gaps before calling attribution complete.**
   Draw-arena drains, per-pixel weight under Vulkan, and worker-thread GX
   exclusive time have no counters yet.
6. **Promote or retain COW deliberately.** Broader title/endurance, paced
   presentation, reset/snapshot/shutdown, and force-floor coverage must
   precede default promotion and title pinning.

Allowed claim (still accurate, carried forward verbatim): the measured route
clears the 66/s unthrottled emulation-capacity gate without removing the
retained software/interpreter/DSP-LLE paths. A complete exercised
force-floor gate, actual 60-Hz presentation, and release-quality audio are
not yet established.

## Outstanding — correctness / validation

- **Intro→menu fly-in corun divergence** (pre-existing, IPL menu route
  only): frames ~879–914, planes B/D/K/L/N, gpu≈2×sw alpha-halving
  signature. Not seen on the WW route.
- **Latent PE-fence-poison hang note** in gx.c's drain loop (pre-existing
  handoff note; never reproduced).
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
- **Audio-enabled endurance** (July checkpoint item, partial progress
  tonight): the paced route now passes with live-sample underruns=0/drops=0
  out to 424,448+ frames, and the exit-line underrun counts from parked runs
  are understood to be a parked-window artifact rather than real drops (see
  "Audio" above). Long-duration endurance beyond this route, and PCM
  fidelity validation (no capture/reference-comparison tooling exists yet),
  remain open.
- **July checkpoint items still open**: WGL flicker final validation (clean
  unobstructed 60 s capture on the inner Game Play screen); window-resize
  stress; in-menu memcard copy/delete acceptance.

## Process / tooling notes (this session)

- Builds ALWAYS BelowNormal priority, max 2 jobs. Launch runs via bash
  with file redirection — a PowerShell dual-pipe synchronous redirect
  deadlocks `gcn_boot` (voluminous stderr fills the pipe; caused one real
  wedged run previously).
- Verify every headed run through the TCP debug server (`GCN_DEBUG_PORT`,
  `tools/gcn_debug_client.py ping/screenshot`) — exit-gate greps prove
  nothing about a wedged run.
- `GCN_GX_XFB_HASH=1` is required for the chain gates to run at all — a
  prior handoff's recorded environment omitted it; don't reuse that env
  verbatim.
- The COW knob is `GCN_GX_VK_TEXTURE_COW=1`.
- Debug ports 4380/4381 can be squatted by unrelated sibling processes —
  check with `netstat` before picking a port for a new run.
- `GCN_DEBUG_PORT` parks the process at route end rather than exiting: wall
  time must be measured start→parked-marker, and the process must be quit
  via `tools/gcn_debug_client.py`, not by waiting on exit.
- Bash `$!` after `run.sh` is the wrong PID — `run.sh` execs, so `$!` is
  not the actual runtime process.
- Recompiler ctest requires `C:\msys64\mingw64\bin` on PATH or 3 tests
  fail with spurious compile errors.
- The per-triangle differential knobs (`GCN_GX_VK_DRAW_VALIDATE`) are a
  silent no-op without `GCN_GX_BACKEND=vulkan-shadow`; corun requires
  `GCN_GX_BACKEND=vulkan`.
- `GCN_GX_GENERAL_DEBUG_XY="x,y"` dumps matched CPU/GPU per-pixel
  intermediates (bit patterns) — this pinned the FDiv ULP bug in one pass.
