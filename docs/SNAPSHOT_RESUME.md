# Snapshot/resume and BIOS skip — implementation spec

Status: DESIGN (2026-08-03). User policy: LLE is a floor; HLE is an
opt-in optimization. Two tiers share one mechanism:

- **Iteration tier** (dev tooling): save/restore full machine state at an
  AOT-safe checkpoint to cut test time (~1/3 of the 110M route is
  boot+menu+apploader). Liberties allowed (baked RTC, determinism);
  documented, not validated beyond basic route equivalence.
- **Production tier** ("proper" BIOS skip): the shipped opt-in that boots
  a title without the IPL screen. Must restore state captured FROM the
  real LLE boot (never synthesize post-BS2 state, per PRINCIPLES.md) and
  pass the full validation gate below.

## Mechanism

One save/load format serves both tiers.

- **Capture**: `GCN_SNAPSHOT_SAVE=<path>` + the existing race-free
  checkpoint machinery (`GCN_CHECKPOINT_PC`, debug_server.c) — at the
  parked dispatcher boundary (default: the apploader→DOL handoff,
  `0x80003140` for WW), serialize and either continue or exit. Capture
  REQUIRES a fully-drained GX sync point (pipeline empty, resident work
  materialized to guest RAM/software EFB planes) so no GPU state is ever
  serialized — park first, drain, then dump.
- **Restore**: `GCN_SNAPSHOT_LOAD=<path>` — boot.c skips BS1/BS2/IPL,
  loads the blob, rebuilds host-side caches, enters the dispatch loop at
  the saved PC.
- **Format**: versioned binary; header carries magic, format version,
  framework commit id, and a content hash of the disc/DOL identity the
  snapshot was taken under (refuse restore on mismatch, loudly).

## What must be saved vs what may be reset

Saved (guest-visible machine state):
- CPUState complete (GPRs/FPRs/SPRs/MSR/exception/reservation/locked-L1
  contents and tags, cycles/timebase).
- MEM1 (24 MB), ARAM (16 MB, dsp_lle), DSP-LLE core state (all regs,
  mailboxes, control, pending IRQs, accelerator state).
- Every device model's POD state: EXI (channels, SRAM, RTC counter,
  memcard state incl. dirty flags + backing image position), VI, SI, DI
  (incl. pending/deferred command state), PI/MI/AI, GP/CP/PE and the GX
  command state (BP/XF/CP register files, TMEM, EFB color/depth planes,
  FIFO/gather state — which must be EMPTY at a proper sync point; assert
  it, don't serialize a live pipeline).

Reset at restore (host-side memos, exact by reconstruction):
- native-code invalidation bitmap + content-stale bitmap + page-CRC memo
  (all-stale reset is the existing `gcn_native_code_reset` semantics),
  draw-config cache, texture/TLUT shadow caches, rings (observability),
  interpreter capture tables.

## Validation gates

Production tier (all mandatory):
1. Full-boot route vs save-at-handoff+resume route: the XFB publication
   chain SUFFIX from the handoff onward must be byte-identical, same
   publication/frame counts from that point, poison=0.
2. Corun clean over the resumed route segment.
3. ctest suites green; golden route byte-identical when the feature is
   unused.
4. Restore-refusal paths tested (wrong disc hash, wrong version).

Iteration tier: gate 3 plus a one-time route-equivalence spot check;
liberties (fixed RTC epoch etc.) documented in the header flags.

## Non-goals

- No synthetic post-BS2 low-mem construction (gcrecomp/reshine pattern).
- No serialization of live Vulkan objects — always drain first.
- Savestates-during-gameplay are a natural extension but not this task's
  gate; design the format so mid-game capture isn't precluded.
