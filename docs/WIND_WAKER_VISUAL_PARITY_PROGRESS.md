# Wind Waker Visual Parity Progress

Last updated: 2026-08-13

## Current Focus

The focus is visual correctness. Frame rate matters for iteration speed, but it is not the current acceptance target.

The active problem is no longer a broad "GX might be wrong" guess. Current evidence now pins the title-attract director state closely enough to compare matching beats: runtime and Dolphin share the same active `dDemo_manager` pointer, and the earlier strip mismatch was mostly a publication-to-demo-frame alignment problem. The runtime now draws the title/logo and some water strokes, but at the same demo timeline it still differs in camera framing, water clarity, wave lines, and lighting.

Latest update: the first major compressed wait was fixed in the runtime DI model. Runtime now uses a Dolphin-style delayed DVD read completion with read-buffer/seek/raw-read timing instead of completing every command on the next dispatch tick. The key phase gate at `0x802356E0` now sees `0x802916C0 -> 1` for 74 publications before clearing, matching Dolphin's 73-publication busy window. The effect loop now processes the previously skipped slot-2 emitter `0x80912470` in the first pass. This is a correctness improvement, but not final visual parity: the first effect loop now reaches global word `0x1F2`, four ticks past the earlier Dolphin reference `0x1EE`, and later title-attract screenshots still diverge in camera timing, scene placement, and water/lighting clarity.

New oracle tooling: `tools/gcn_cosim.py dolphin-ipl-sweep` now captures multiple Dolphin TCP screenshots from one NoGUI oracle process. `runtime-pub-sweep`, `runtime-capture`, `dolphin-ipl-capture`, and `dolphin-ipl-sweep` also support `--ram-dump ADDRESS:LENGTH` and `--ram-dump-deref POINTER_ADDRESS:READ_OFFSET:LENGTH`, so screenshot samples can persist absolute guest RAM windows over the TCP `read_ram` path without relying on a block-entry GPR hook.

Latest draw-state boundary:

- Runtime draw-state records now include `xfb_pub_count` and direct `genmode` fields for `recent`/`large` rings. `tools/gcn_cosim.py runtime-pub-sweep` now requests draw-state before the screenshot, so the diagnostic state is tied to the publication being sampled instead of drifting two publications later during TCP round trips.
- Matched runtime prescreenshot draw-state: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-matched-frame-drawpub-prescreen-pub2200-20260813-codex\runtime-drawpub-prescreen-pub2200.draw-state.json`
- Post-indexed-XF visual validation: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-indexedxf-visual2-pub2200-20260813-codex\runtime-indexedxf2-pub2200.png`
- Dolphin matched software draw-state: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-dolphin-matched-frame-efbdraw-pub3200-20260813-codex\dolphin-matched-efbdraw-pub3200.sw-draw-state.json`

Current finding: the major remaining water placement mismatch is an XF position-matrix mismatch before rasterization. Runtime's leading water pass at pub 2200 is backed by draw records stamped `xfb_pub_count=2199`; Dolphin's pub 3200 water pass uses identical projection and viewport, but a different position matrix. Runtime uploads a yaw-only matrix `[0.859899, 0, -0.510464, 0; 0, 1, 0, 0; 0.510464, 0, 0.859899, 0]`, while Dolphin uses the camera-pitched/translated matrix `[0.859899, 0, -0.510464, 0; 0.0928797, 0.983308, 0.15646, -26.5527; 0.501943, -0.181951, 0.845545, 4.921875]`. That explains the large shifted water/horizon bboxes before TEV or EFB-copy encoding.

Indexed XF loads (`LOAD_INDX_A..D`) were implemented in `runtime/src/gx.c` from Dolphin's opcode decoder semantics and validated with a pub 2200 screenshot. This closed a real LLE gap, but it did not change the Wind Waker attract screenshot or the leading water matrix, so the active boundary is now CPU-side matrix generation/selection around main-DOL `0x802DC184..0x802DC240`, which calls the FIFO matrix uploader at `0x802D8C58`.

Runtime matrix-uploader probe:

- Artifact: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-matrix-uploader-probe-pub2200-20260813-codex\runtime-matrix-uploader-pub-pub2200.gpr-probe.json`
- Probe PCs: `0x802D8C58` (`GXLoadPosMtxImm`-style direct uploader) and `0x802D8CC4` (composed uploader), with inline 48-byte snapshots from the matrix pointer in `r3`.
- The exact yaw-only matrix appears once per publication from pub 2186 through pub 2202 with `pc=0x802D8C58`, `lr=0x802DC1C0`, `r3/r6=0x8040CC18`, `r4=0`, and caller argument `r5=3`.
- That confirms the bad water matrix is already present in the CPU matrix argument passed to the direct upload path. It is not introduced by FIFO byte decode, indexed XF handling, XF register storage, or the rasterizer transform.
- The wrapper branch condition can now be narrowed to its state inputs: `r13=0x803FE0E0`, table pointer global at `0x803F794C`, and compose-enable/global flag at `0x803F7950`. The next state probe should compare those bytes and the `r6` matrix source against Dolphin at the matched demo frame to decide whether runtime is taking a different wrapper path or feeding a different source matrix into the same path.
- Generated-code inspection found the initializer for those globals at `0x802DD7CC..0x802DD858`. It writes `r13-26516` from object offset `0x54`, and writes `r13-26512` from object byte offset `0x34`. Runtime's direct path is therefore consistent with the current model/context object state; do not patch the renderer to compensate unless a Dolphin-side state probe proves the object state matches and the composition is still different.

Runtime same-instant wrapper probe:

- Artifact: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-wrapper-inline-probe2-pub2200-20260813-codex\runtime-wrapper-inline2-pub-pub2200.gpr-probe.json`
- The bad upload at pub 2200 is immediately preceded by wrapper entry `0x802DC184` with `r5=3` and `r6=0x8040CC18`.
- At that exact wrapper entry, the global table pointer/compose flag bytes at `0x803F794C` are `815ed27800000000`: table pointer `0x815ED278`, compose flag `0`.
- Dereferencing `0x815ED278` at the same instant shows first bytes `01 01 01 01 ...`, so table entry `3` is `1`. That selects the direct `0x802D8C58` upload path.
- The same-instant source matrix at `0x8040CC18` is already the yaw-only water matrix. This is the strongest current proof that the runtime side is not losing camera pitch/translation in GX decode or rasterization.

Dolphin wrapper-state status:

- Coarse fast-core Dolphin RAM dump at the matched frame exists: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-dolphin-wrapper-ram-pub3200b-20260813-codex\dolphin-ipl-capture.ram-dump.json`
- In that after-screenshot dump, `0x803F794C` points to `0x815EFA08`, compose flag is `0`, and table entry `3` is `0`. That is a useful lead, but it is not the same instant as the water matrix upload, and these wrapper globals are per-object enough that after-screenshot RAM can mislead.
- Interpreter-mode Dolphin GPR probing for the wrapper/upload path is not practical to pub 3200: the attempt timed out around publication 2520 after roughly 15 minutes without producing the per-call sidecar.
- The next oracle/tooling step is therefore a fast Dolphin trace hook at XF position-matrix write time. It should record the current XF matrix words plus sampled guest RAM for `0x803F794C`, the pointed table bytes, `0x803F7950`, and `0x8040CC18`. That will tell us whether Dolphin has table entry `3 == 0` at the actual water upload, or whether both sides take the same branch and the upstream source matrix generation differs.

Fast Dolphin XF/write-time oracle:

- Dolphin source patch: `F:\Projects\gcnrecomp\oracle\dolphin\Source\Core\Core\GcnTrace.h` now supports env gate `GCN_TRACE_XF_CONTEXT_STATE` and TCP command `xf_context_state`. The first implementation recorded all XF memory writes; it worked but the relevant water writes fell out of the 16,384-record ring before screenshot acceptance. The hook was then narrowed to slot-0 position matrix writes (`0..11`, plus `0x400..0x40B` form), rebuilt into `F:\Projects\gcnrecomp\oracle\dolphin\Binary\x64\DolphinNoGUI.exe`, and re-run.
- Capture artifact: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-dolphin-xf-context-pub3200b-20260813-codex`
- Leading Dolphin water draw at screenshot pub `3199`, draw `seq=11019215`, uses matrix provenance XF write seqs `31588985..31588996`.
- The XF context ring now contains those exact writes. At all 12 writes, sampled wrapper globals are `0x803F794C = 0x815ED278`, compose flag `0`, pointed table first bytes `01 01 01 01 ...`, so `table[3] == 1`.
- This matches the runtime same-instant wrapper state for the bad upload: runtime also has `0x803F794C -> 0x815ED278`, compose flag `0`, and `table[3] == 1`.
- Therefore the active mismatch is not that runtime chooses a different wrapper branch. Both sides are in the direct matrix-upload class for this water pass, but Dolphin's XF payload is already camera-composed while runtime's CPU source argument is yaw-only.
- Caveat: this Dolphin hook runs at GPU/XF decode time under the fast core. It cannot see the JIT CPU GPRs or the original `r6` source pointer. The absolute `0x8040CC18` sample is zero in Dolphin at XF decode time, which proves only that GPU-time RAM is not a reliable substitute for CPU-call-time source memory. The next probe must move one step earlier to CPU FIFO/gather-pipe timing or add a fast-core/JIT-side PC/source sampler.

Fast CPU-side Dolphin source probe:

- Dolphin source patch: `F:\Projects\gcnrecomp\oracle\dolphin\Source\Core\Core\PowerPC\CachedInterpreter\CachedInterpreter.cpp` now calls the same `GcnTrace::SetPc` / `GcnTrace::MaybeProbePc` hook as the plain interpreter. This allows GPR probes under `Dolphin.Core.CPUCore=5` (Cached Interpreter), which is much faster than the plain interpreter while still exposing `PowerPCState`.
- Build validation: rebuilt `DolphinLib.vcxproj` and `DolphinNoGUI.vcxproj`; the patched executable was copied to `F:\Projects\gcnrecomp\oracle\dolphin\Binary\x64\DolphinNoGUI.exe`.
- Capture artifact: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-dolphin-cachedinterp-wrapper-probe-pub3200-20260813-codex`
- Runtime of the capture was practical: about 12.7 minutes to pub 3200, compared with the earlier plain-interpreter attempt that timed out around pub 2520 after about 15 minutes.
- Dolphin wrapper record at pub `3199`, probe seq `18509035`, `pc=0x802DC184`, `lr=0x802DBD2C`, `r5=3`, `r6=0x8040CC18`, has same wrapper globals as runtime: `0x803F794C = 0x815ED278`, compose flag `0`, pointed table first bytes `01 01 01 01 ...`.
- At that same CPU instruction, Dolphin memory at `0x8040CC18` is already the camera-composed matrix `[0.859899, 0, -0.510464, 0; 0.0928797, 0.983308, 0.15646, -26.5527; 0.501943, -0.181951, 0.845545, 4.921875]`.
- Dolphin direct uploader record at pub `3199`, probe seq `18509043`, `pc=0x802D8C58`, `lr=0x802DC1C0`, `r3=0x8040CC18`, `r4=0`, `r5=3`, carries the same camera-composed source matrix.
- This is now the sharpest known divergence: runtime and Dolphin use the same wrapper globals, same table entry, same direct upload call, and same source address (`0x8040CC18`) for this water pass. Dolphin's source bytes are camera-composed; runtime's source bytes are yaw-only. The bug is upstream of `0x802DC184` / `0x802D8C58`, in whoever writes or fails to update the scratch matrix at `0x8040CC18`.

Latest visual strip evidence:

- Runtime post-DI sweep `900..1600`: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-title-sweep-900-1600-20260813-codex`
- Runtime later sweep `1700..2400`: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-title-sweep-1700-2400-20260813-codex`
- Dolphin no-card sweep `2250,2355,2450,2550,2650`: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-dolphin-title-sweep-2250-2650-20260813-codex`
- Contact sheets:
  - `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-dolphin-title-sweep-compare-20260813-codex\runtime-dolphin-title-sweep-contact.png`
  - `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-late-dolphin-title-sweep-compare-20260813-codex\runtime-late-dolphin-title-sweep-contact.png`

Interpretation: runtime reaches a stable logo/title route and continues to draw ocean/cloud material, but it lingers through a long ocean/cloud beat while Dolphin advances through Link/cliff/tree title beats in the same publication range. That points back to an attract-demo state/cadence boundary before treating residual water differences as a pure GX feature bug.

State-aligned title-attract evidence:

- Runtime pub 2203 RAM dump: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-demo-ramdump-pub2200-20260813-codex\runtime-demo-ramdump-pub2200.ram-dump.json`
- Dolphin pub 2250/2355 RAM dumps: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-dolphin-demo-ramdump-2250-2355-20260813-codex`
- Dolphin pub 3200 RAM dump: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-dolphin-demo-ramdump-pub3200-20260813-codex\dolphin-demo-ramdump-pub3200.ram-dump.json`
- Matched contact sheet: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-demo-frame-matched-compare-20260813-codex\demo-frame-contact.png`

Key finding: `dComIfG_play + 0x5AC8` and absolute `0x803CA6D0` both point to the same active demo manager, `0x80AC11EC`, on runtime and Dolphin. The manager snapshot is identical except for frame counters at `+0xD4/+0xD8`: runtime pub 2203 is demo frame `1710`; Dolphin pub 2250 is frame `763`; Dolphin pub 2355 is frame `868`; Dolphin pub 3200 is frame `1713`. Therefore the older Dolphin pub 2250/2355 comparisons were not beat-aligned. The correct near-oracle pair is runtime pub 2203/frame 1710 vs Dolphin pub 3200/frame 1713. Image RMSE for that pair is still high (`16003.8`, normalized `0.244202`), and the contact sheet shows remaining visual defects at matched demo time.

The current narrowed boundary is no longer the tag/list helper itself. Runtime and Dolphin take the same tag-helper sequence and the same helper flag values after the id-3 timer sequence. The previously compressed phase was the object at `0x80AC0F28+0x1C4 == 0x80AC10EC`, using phase table `0x80394B88`: before the DI timing fix, runtime advanced phase index `0x80AC10F0` from `2` to `3` at pub 276 while Dolphin held that phase from pub 1328 until pub 1401. After the DI timing fix, runtime holds the equivalent `0x802916C0` busy gate from pub 339 through pub 411 and clears at pub 412, which matches the Dolphin hold count. The remaining boundary is now smaller: a four-tick effect/global counter offset plus the still-missing water surface effects.

Counter-normalization experiment: patching runtime `0x803E8140` from `0x12E` to Dolphin's `0x1EE` at the first `0x8008B6E0` checkpoint makes the first effect pass process the same six slots in the same order as Dolphin. A longer bounded sample then shows target emitter `0x80912AB8+408` becomes `0x815739C8`, the Dolphin good table, by runtime pub 1303 / block 620494849. In the unpatched run, the same target field had been overwritten with bad/secondary table `0x81573A48` by this range.

Headless validation is useful and should remain the default for iteration:

- Runtime headless sweeps are fast enough for repeated probes.
- Dolphin headless/interpreter captures are slower, but they provide the oracle state and screenshots.
- Headed runs are still needed for final human review of motion, lighting, and water feel.

## Correctness State

High-level scene composition is now close enough that broad missing-layer bugs are mostly gone, but the remaining issues are still visible:

- Ocean/title water has some strokes now, but still lacks Dolphin-matching clarity, tonal variation, and cadence in matched-looking frames.
- Attract-demo publication cadence differs from Dolphin, but the active demo manager frame counter now gives a reliable alignment key. Runtime pub 2203/frame 1710 should be compared against Dolphin pub 3200/frame 1713, not against Dolphin pub 2250/2355.
- The problem tracks to JPADraw emitter/resource scheduling before final GX writes, not missing table bytes or a simple TEV helper mismatch.
- The title/logo layer is now present after the DI timing fix, which is visible progress versus the old missing-layer state.

Latest EFB-copy/source comparison:

- Runtime pub1600 EFB copy/source dump: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-title-pub1600-efbcopy-20260813-codex`
- Dolphin pub2355 EFB copy/source dump: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-dolphin-title-pub2355-efbcopy-20260813-codex`
- Source montage: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-efb-source-compare-runtime1600-dolphin2355-20260813-codex\efb-source-last-nonxfb-contact.png`
- Copy stream facts: both sides emit the same important non-XFB destinations/formats, including `0x0065FF20` format 11 and `0x00614F20` format 6. The source images already differ before encoding, so the current evidence does not support "EFB texture copy is missing entirely" as the primary cause.

Known good/bad pairing:

- Dolphin assigns good table `0x815739C8` to target emitter `0x80912AB8`.
- Runtime assigns that same good table to emitter `0x80912EE8`.
- Runtime later consumes target emitter `0x80912AB8` with old/secondary table state, producing the visible mismatch.

The latest effect slot loop around `0x8008B6E0` is improved:

- Dolphin first pass processes slots `0,1,2,3,4,5`.
- Before the DI timing fix, runtime first pass processed slots `0,1,3,4,5`, then ran the rebuilder, then later visited slot `2`.
- After the DI timing fix, runtime first pass returns six emitters, including slot-2 emitter `0x80912470`.
- Dolphin slot 2 returns emitter `0x80912470`.
- Before the fix, runtime delayed slot 2 later returned target emitter `0x80912AB8`.

This means the large effect phase/timing state bug feeding JPADraw has been corrected, but later draw/rendering issues remain.

Validated counter-normalized behavior:

- Runtime first pass after patch: slots `0,1,2,3,4,5`, matching Dolphin.
- Runtime target emitter `0x80912AB8+408` after bounded run: `0x815739C8`, matching Dolphin.
- Runtime writer-entry ring during the bounded run has 92 hits and no late target-emitter writer hit with `r30 == 0x80912AB8`; the old bad target overwrite path appears suppressed by the corrected phase.
- Screenshot comparison is still not visually perfect: normalized runtime vs Dolphin reports 303,784 differing pixels, with a visible large black artifact and remaining composition/timing differences. The phase fix is a major state-level narrowing, not a complete visual fix.

## Current Technical Boundary

Important guest locations:

- Draw/resource consume: `0x8025F0E4`
- Rebuilder scan/move: `0x8025F2F4`
- Writer entry: `0x8025CB54`
- Writer store to `emitter+408`: `0x8025CB78`
- Effect loop entry: `0x8008B6E0`
- Draw wrapper callsite/return: `0x8008BB30` / `0x8008BB34`
- Rebuilder wrapper path: `0x80234EB8..0x80234ED4`
- Effect/global counter increment: `0x802449EC`
- Effect/global counter word: `0x803E8140`

Important runtime slot array facts:

- First runtime loop base `r24 = 0x80A719E0`.
- Slot size is `0x34`.
- Slot 2 pointer is `0x80A71A48`.
- Slot 2 state field is `0x80A71A6C` (`slot+36`).

Latest watch finding to confirm:

- Runtime has an early write at `0x8003FFEC` writing `0xFFFFFFFD` to slot 2 state `0x80A71A6C`.
- Runtime later clears slot 2 at `0x80088390/0x80088398` immediately before the first effect loop, so stale negative slot state is not the reason for the skip.
- Runtime writes flag byte `r24+1880` to zero at `0x8008B478`.
- Runtime reaches first pass with global word `0x12E`, phase `2`; Dolphin reaches the matching first pass with global word `0x1EE`, phase `6`.

The previous target was why the global counter/phase is 192 ticks behind in runtime at the same effect/emitter sequence. The counter is incremented at `0x802449EC`, inside `cCt_Counter__Fi`, called from the main game loop at `0x80023204`.

Counter-cadence comparison now shows the counter itself is not dropping ticks. Runtime and Dolphin both increment once per game-loop publication once their game counter starts. The `0xC0` gap is earlier scene/overlap scheduling:

- Before the first `0x8008B6E0` effect loop, runtime has 302 `cCt_Counter` calls.
- Before Dolphin's first matching `0x8008B6E0`, Dolphin has 494 `cCt_Counter` calls.
- Delta: 192 calls (`0xC0`).
- The delta decomposes exactly into 56 fewer runtime ticks in `fpcCtRq_isCreatingByID` (`0x8003CD0C`) and 136 fewer runtime ticks in `fopOvlpReq_phase_WaitOfFadeout` (`0x80029964`).

The overlap request pointer for `WaitOfFadeout` is the same on both sides: `0x803B9E30`. Its first sampled header differs:

- Runtime first 12 bytes at `0x803B9E30`: `40 00 00 00 00 01 00 00 00 00 00 00`
- Dolphin first 12 bytes at `0x803B9E30`: `82 00 00 01 00 01 00 00 00 00 00 01`

A blunt patch experiment forcing those first 12 runtime request bytes to Dolphin's first sampled header at the first `0x80029964` hit prevented the runtime from reaching the first effect loop before timeout/block 671350785. That is diagnostic only; it proves the state is phase-critical but is not a candidate fix.

Runtime header-watch at the first `WaitOfFadeout` found the local writer sequence for `0x803B9E30`:

- Initial zeroing: `0x80003448..0x80003454`
- `cReq_Create` writes byte 0 through `0x802453EC`, `0x802453FC`, `0x80245408`
- Overlap setup writes timer/fields: `0x80029C50` writes halfword `0x001E` at `+6`, `0x80029B34` writes `+4`, `0x80029B38` writes `+2`, `0x80029B44/48` zero `+8/+C`
- `cReq_Done` writes byte 0 through `0x8024539C`, `0x802453AC`, `0x802453B8` immediately before the first runtime `WaitOfFadeout`, leaving byte 0 as `0x40`

Request-helper probes corrected the previous interpretation: runtime is not missing the command-2 transition. Runtime also calls `cReq_Create(0x803B9E30, 2)` from `lr=0x80029C9C/ctr=0x80029FF8` and `cReq_Command(0x80AC11CC, 2)` from `lr=0x800299B0/ctr=0x80029964`, but it does so much earlier in local cadence. Dolphin makes the analogous transition at pub 1485, just before the first matching effect loop at pub 1487.

The current upstream boundary is therefore scene-request/node-request completion, not a skipped overlap command. In the latest scene request captures:

- Runtime reaches `fopScnRq_phase_Execute` (`0x8002A028`) only 31 times before moving on.
- Dolphin reaches `fopScnRq_phase_Execute` 272 times before moving on.
- Runtime `fopScnRq_Execute` (`0x8002A0E8`) count is 99; Dolphin count is 309.
- Runtime `fopScnRq_phase_ClearOverlap` (`0x80029FF8`) count is 14; Dolphin count is 1.

`fopScnRq_phase_Execute` calls `fpcNdRq_Execute__FP19node_create_request` (`0x8003F4B0`), which calls `fpcNdRq_DoPhase__FP19node_create_request` (`0x8003F468`). `fpcNdRq_DoPhase` dispatches the embedded phase process at request `+48` through `cPhs_Handler__FP30request_of_phase_process_classPPFPv_iPv` (`0x8024533C`) using the method table pointer at request `+56`. The next comparison should determine why this phase handler completes far earlier in runtime.

Latest node-request phase probe:

- Runtime artifact: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-node-request-phase-pub350-20260813-codex`
- Dolphin artifact: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-dolphin-node-request-phase-pub1490-20260813-codex`
- For long scene request `0x80AC146C`, runtime enters `cPhs_Handler` 26 times total.
- Dolphin enters `cPhs_Handler` for the same request 231 times total.
- Runtime spends 5 passes with method table `0x803727D0`, then 21 passes with method table `0x803727A8`.
- Dolphin spends 61 passes with method table `0x803727D0`, then 170 passes with method table `0x803727A8`.
- The first 56-pass delta exactly matches the previously measured `fpcCtRq_isCreatingByID` (`0x8003CD0C`) gap.
- The long request's embedded phase bytes at `0x80AC149C` and the method table contents match at first sample. The request header/queue fields differ: runtime has zeros in the early queue/link fields where Dolphin has `0x80372738`, `0x80A8C0D4`, `0x01000000`, and related pointers/flags. Treat that as a strong lead, but not yet as a fix, because runtime is currently starting from the handoff snapshot while Dolphin is a cold no-card oracle.

Probe-time memory snapshots are now available in both the runtime and Dolphin oracle GPR rings. This fixes the earlier weakness where `--gpr-probe-memory` read reused request/tag structs only after the run had ended. The new inline specs are passed through `--gpr-probe-inline-memory` and support direct and dereferenced reads at the same instant as the PC/GPR sample.

Latest inline create-tag comparison:

- Runtime artifact: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-inline-probe-smoke3-20260813-codex`
- Dolphin artifact: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-dolphin-inline-probe-pub1490-20260813-codex`
- Both sides now snapshot `fpcCtRq_isCreatingByID` (`0x8003CD0C`) at probe time for `gpr3`.
- For id 3, both sides use tag `0x80AC138C`, and the first 64 tag bytes match at-time:
  `00000000 80372690 00000000 80AC138C 01000000 00000000 80AC1480 803BCE3C ...`
- Runtime `fpcCtRq_IsCreatingByID` (`0x8003CD28`) counts before the sampled effect boundary: id 1 = 4, id 3 = 15, id 13 = 6, id 200 = 4.
- Dolphin counts in the comparable oracle window: id 1 = 60, id 2 = 2, id 3 = 164, id 13 = 44, id 200 = 2.
- Paired `0x8003CD28 -> 0x8003CD0C` samples with live tags: runtime id 3 = 14, Dolphin id 3 = 163.
- The id-3 tag state progression is structurally the same on both sides, but Dolphin holds each state for many more publications: runtime saw snapshot counts 9/1/3/1 across the four observed id-3 states; Dolphin saw 120/1/8/34.

This makes the current boundary sharper: runtime is not using a different id-3 tag or stale post-run memory. It is completing the same create-request/tag state sequence too quickly.

Latest return-point node/create comparison:

- Runtime artifact: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-node-create-return-mem-pub350-20260813-codex`
- Dolphin artifact: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-dolphin-node-create-return-mem-pub1490-20260813-codex`
- Runtime records: 171. Dolphin records: 1888.
- For target long request `0x80AC146C`, runtime reaches `fpcNdRq_DoPhase` return point `0x8003F48C` 26 times; Dolphin reaches it 231 times.
- The target return distribution is structurally identical:
  - Runtime: return `2` x5, return `0` x19, return `4` x2.
  - Dolphin: return `2` x5, return `0` x224, return `4` x2.
- First phase table `0x803727D0`, request id field `+84 == 1`: runtime wait-return `0` count is 3; Dolphin wait-return `0` count is 59.
- Transitional phase table `0x803727A8`, request id field `+84 == 0xFFFFFFFE`: both sides match at return `2` x3 and return `0` x2.
- Second phase table `0x803727A8`, request id field `+84 == 3`: runtime wait-return `0` count is 14; Dolphin wait-return `0` count is 163.
- At `fpcNdRq_phase_IsCreated` after `fpcCtRq_IsCreatingByID`, target runtime returns `{1:17, 0:2}`; Dolphin returns `{1:222, 0:2}`.
- At `fpcNdRq_phase_Create` after `fpcSCtRq_Request`, both sides create ids `1` then `3` with matching args:
  - first request args `(0x803BCE20, 5, 0x8002A130, 0x80AC146C, 0)`
  - second request args `(0x803BCE20, 8, 0x8002A130, 0x80AC146C, 0)`

This removes another class of theories: runtime is not taking a different phase path or creating different request ids. It is retiring the same wait states far too early.

Latest runtime id-3 tag watch:

- Runtime artifact: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-id3-tag-watch-pub350-20260813-codex`
- Watched tag range: `0x80AC138C:0x80AC13EC`.
- Tag/list setup for id 3 is visible through `0x800402F8`, `0x800402FC`, `0x8003D038`, `0x8003D03C`, `0x8003D044`, `0x8003D04C`, `0x800408D4`, `0x800408D8`, `0x800408DC`, and `0x800408E0`.
- The candidate state byte/timer pair is now `0x80AC13D4` / `0x80AC13D8`.
- Runtime writes timer `0x005A`, counts it down through `0x800FD7E8`, transitions byte `0x80AC13D4` to `1` at `0x8022C190`, resets timer to `0x001E` at `0x8022C198`, transitions to `3` at `0x8022C260`, resets to `0x005A` at `0x8022C268`, then transitions to `4` at `0x8022CEA0`.
- Near the shortened liveness window, runtime mutates list pointer `0x80AC1394` through `0x80244C60` / `0x80244D5C`.

Resolved follow-up: the id-3 tag timer is not the shortened-liveness bug, but id-3 tag/list liveness after the timer remains the active boundary.

- Runtime watch entries now include XFB publication stamps, so runtime watch logs can be compared directly to Dolphin GPR probe records.
- Runtime artifact with publication-stamped watch: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-id3-tag-watch-pubstamp-pub350-20260813-codex`
- Dolphin artifact: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-dolphin-id3-tag-timer-probe-pub1490-20260813-codex`
- Runtime timer segments at `0x80AC13D8`:
  - pub 3..92: `0x0059 -> 0`
  - pub 93..121: `0x001D -> 0`
  - pub 122..210: `0x0059 -> 0`
  - pub 211..240: `0x001D -> 0`
- Dolphin timer segments at `0x80AC13D8`:
  - pub 1056..1145: `0x0059 -> 0`
  - pub 1146..1173: `0x001D -> 0`
  - pub 1174..1262: `0x0059 -> 0`
  - pub 1263..1292: `0x001D -> 0`
- Runtime transitions at pubs 92, 121, and 210 match Dolphin transitions at pubs 1145, 1173, and 1262 after accounting for the handoff snapshot's cold-oracle publication offset.

This corrects the previous interpretation. The request/tag timer state machine and countdown cadence are structurally oracle-matching. The raw runtime-vs-Dolphin wait-count gap was partly an origin mismatch: runtime is resumed from `ww-handoff-passC.gcnsnap`, while Dolphin was measured from a cold no-card boot. The remaining gap now re-anchors to the list/liveness window after the timer sequence.

Latest re-anchored node/list findings:

- Runtime artifact: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-node-clearoverlap-pubstamp-pub500-20260813-codex`
- Dolphin artifact: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-dolphin-id3-tag-list-removal-pub1490-20260813-codex`
- Runtime target scene request `0x80AC146C` enters method table `0x803727A8` at pub 268 and returns `4` at pub 284.
- Dolphin target scene request enters the same method table at pub 1320 and returns `4` at pub 1485.
- Runtime creates/inserts the id-3 tag around pub 270; Dolphin creates/inserts the matching id-3 tag around pub 1322.
- Runtime mutates/removes the relevant id-3 list links around pubs 278..284. Dolphin keeps `fpcCtRq_IsCreatingByID(3)` returning live through pub 1484 and returns dead at pub 1485.
- The overlap request command-2 write is downstream of this: runtime issues `cReq_Command(0x803B9E30, 2)` at pub 297 from `lr=0x80029C9C/ctr=0x80029FF8`, while Dolphin issues the analogous command at pub 1485.

Current interpretation: the remaining visual mismatch is caused by early retirement of the id-3 create/list state feeding the scene-request phase. The overlap fade and effect phase errors are consequences of that earlier liveness divergence.

Latest phase/resource re-anchor:

- Runtime tag-helper entry/flag artifact: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-tag-helper-entry-flags-pub500-20260813-codex`
- Dolphin tag-helper entry/flag artifact: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-dolphin-tag-helper-entry-flags-pub1490-20260813-codex`
- Runtime `0x80AC10D8` watch artifact: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-80ac10d8-watch-pub330-20260813-codex`
- Dolphin `0x80AC10D8` phase artifact: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-dolphin-80ac10d8-phase-pub1490-20260813-codex`
- Runtime `0x80AC10D8` phase-return artifact: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-80ac10d8-phase-return-pub330-20260813-codex`
- Runtime `0x802356E0` internals artifact: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-802356e0-internals-pub330-20260813-codex`
- Dolphin `0x802356E0` internals artifact: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-dolphin-802356e0-internals-pub1490-20260813-codex`

The tag/list helper sequence itself now matches structurally after normalizing by the handoff offset:

- Runtime inserts the `0x80AC10D8` tag/helper object at pub 271; Dolphin inserts the same object at pub 1323.
- Runtime enters the lower-list helper sequence at pub 278; Dolphin enters the corresponding sequence at pub 1441.
- At helper entry `0x80245574`, the sampled flag byte is `0` on both sides before insertion.
- At helper entry `0x80245534`, the sampled flag byte is `1` on both sides before removal.

The missing wait is inside the embedded phase process at `0x80AC10EC`:

- Runtime phase-index writes at `0x80AC10F0`: `0 -> 1` pub 271, `1 -> 2` pub 276, `2 -> 3` pub 276, `3 -> 4` pub 278, `4 -> 5` pub 278, `5 -> 6` pub 278, then clear.
- Dolphin phase-index writes at `0x80AC10F0`: `0 -> 1` pub 1323, `1 -> 2` pub 1328, `2 -> 3` pub 1401, `3 -> 4` pub 1431, `4 -> 5` pub 1437, `5 -> 6` pub 1441, then clear.
- The key shortened phase is table entry `0x802356E0`: runtime returns advance (`2`) at pub 276; Dolphin keeps returning hold (`0`) until pub 1401.
- `0x802356E0` first calls `0x802916C0`. Runtime sees `0x802916C0 -> 0` at pub 276 and continues into `0x8005CB34`; Dolphin sees `0x802916C0 -> 1` every pub from 1328 through 1400 and returns hold without calling `0x8005CB34`.
- At pub 1401 Dolphin finally sees `0x802916C0 -> 0`, then follows the same downstream path runtime took at pub 276: `0x8005CB34 -> 0`, state byte `0xFF`, store `r13-28172 = 0x80AD249C`, then phase advance.
- `0x802916C0` is a global async-work scan: it loads arrays from `r13-27476` and `r13-27484`, walks active slots, and returns `1` if any active slot's status word is `1`; otherwise it returns `0`.

Resolved follow-up: early id-3 list retirement was downstream of global async/resource readiness completing too quickly in runtime. Runtime DI now delays read completions with a Dolphin-style DVD timing model. New validation artifact:

- Runtime DI timing phase gate: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-di-timing-802356e0-pub430-20260813-codex`
- Result: `0x802356E0` / `0x802356F8` samples hold `0x802916C0 -> 1` from runtime pub 339 through 411, then `0` at pub 412; Dolphin held from pub 1328 through 1400, then cleared at pub 1401.
- Runtime DI timing effect loop: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-di-timing-effect-loop-pub650-fullprobe-20260813-codex`
- Result: first `0x8008B6E0` pass at runtime pub 496 has global word `0x1F2` and returns six emitters: `0x809128A0`, `0x80912688`, `0x80912470`, `0x80912258`, `0x80912040`, `0x80911E28`.
- Screenshot: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-di-timing-effect-loop-pub650-fullprobe-20260813-codex\runtime-di-timing-effect-loop.png`
- Later title-screen screenshot after the DI timing fix: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-di-timing-title-pub1303-20260813-codex\runtime-di-timing-title.png`. This frame has the logo and press-start UI present without the earlier large black artifact, but the water surface remains flat blue and the scene/camera placement still differs from Dolphin.

## Speed State

Current iteration costs:

- Runtime pub/sample sweeps before real DI delays: roughly 1-2 minutes depending on probe/watch volume.
- Runtime pub 1303 title-screen capture after DI timing: about 3 minutes 20 seconds headlessly.
- Runtime pub 1700..2400 sweep after DI timing: about 8 minutes headlessly for eight screenshots.
- Dolphin pub 2250..2650 sweep with the new one-process TCP sweep: about 7 minutes 20 seconds for five screenshots.
- Narrow runtime GPR probes: practical for tight loops.
- Runtime watch-range dumps: useful when scoped to a slot/list range; large watches overwrite early history.
- Dolphin oracle captures: usually several minutes, but the new sweep path makes multi-frame visual alignment practical.

Headless does not defeat the purpose. It improves repeatability and speed for exact frame/state comparison. The practical workflow is:

- Use headless runtime probes for fast narrowing.
- Use headless Dolphin captures as the oracle at selected boundaries.
- Use screenshots from both sides for self-audit.
- Use headed runs for final motion/visual review once the state diff is fixed.

## Durable Artifacts

Most recent title-attract visual/oracle artifacts:

- Runtime title sweep `900..1600`: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-title-sweep-900-1600-20260813-codex`
- Runtime title sweep `1700..2400`: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-title-sweep-1700-2400-20260813-codex`
- Dolphin title sweep `2250..2650`: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-dolphin-title-sweep-2250-2650-20260813-codex`
- Runtime/Dolphin early contact sheet: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-dolphin-title-sweep-compare-20260813-codex\runtime-dolphin-title-sweep-contact.png`
- Runtime/Dolphin late contact sheet: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-late-dolphin-title-sweep-compare-20260813-codex\runtime-late-dolphin-title-sweep-contact.png`
- Runtime pub1600 draw state: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-title-pub1600-drawstate-20260813-codex`
- Dolphin pub2355 draw state: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-dolphin-title-pub2355-swdraw-20260813-codex`
- Runtime pub1600 EFB copy/source: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-title-pub1600-efbcopy-20260813-codex`
- Dolphin pub2355 EFB copy/source: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-dolphin-title-pub2355-efbcopy-20260813-codex`
- EFB source contact sheet: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-efb-source-compare-runtime1600-dolphin2355-20260813-codex\efb-source-last-nonxfb-contact.png`

Most recent branch-cadence artifacts:

- Runtime: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-branch-cadence-pub500-20260813-codex`
- Dolphin: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-dolphin-branch-cadence-pub1530-20260813-codex`
- Compare: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-branch-cadence-compare-20260813-codex`

Most recent effect-slot artifacts:

- Runtime slot/memory: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-effect-slots-pub500-20260813-codex`
- Dolphin slot/memory: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-dolphin-effect-slots-pub1490-20260813-codex`
- Compare: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-effect-slots-compare-20260813-codex`
- Runtime slot watch: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-effect-slot-watch-pub350-20260813-codex`
- Runtime phase gate: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-phase-gate-pub350-20260813-codex`
- Runtime phase word: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-phase-word-pub350-20260813-codex`
- Dolphin phase gate: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-dolphin-phase-gate-pub1490-20260813-codex`
- Runtime counter-normalized first pass: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-counter-normalized-pub350-20260813-codex`
- Runtime counter-normalized target-field sample: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-counter-normalized-target-field-block620-20260813-codex`
- Runtime/Dolphin montage from that sample: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-counter-normalized-target-field-block620-20260813-codex\runtime-vs-dolphin-target-field-montage.png`
- Runtime/Dolphin absolute-error diff from that sample: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-counter-normalized-target-field-block620-20260813-codex\runtime-vs-dolphin-target-field-ae-diff.png`
- Runtime counter cadence: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-counter-cadence-pub350-20260813-codex`
- Dolphin counter cadence: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-dolphin-counter-cadence-pub1490-20260813-codex`
- Runtime overlap phase memory: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-overlap-phase-mem-pub320-20260813-codex`
- Dolphin overlap phase memory: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-dolphin-overlap-phase-mem-pub1490-20260813-codex`
- Counter-cadence comparison: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-counter-cadence-compare-20260813-codex\counter-cadence-compare-summary.json`
- Runtime overlap header watch: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-overlap-header-watch-first-wait-20260813-codex`
- Dolphin request-helper memory: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-dolphin-request-helper-mem-pub1490-20260813-codex`
- Runtime scene request phase: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-scene-request-phase-pub350-20260813-codex`
- Dolphin scene request phase: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-dolphin-scene-request-phase-pub1490-20260813-codex`
- Runtime WaitFade timer: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-waitfade-timer-pub350-20260813-codex`
- Dolphin WaitFade timer: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-dolphin-waitfade-timer-pub1490-20260813-codex`
- Runtime node request phase: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-node-request-phase-pub350-20260813-codex`
- Dolphin node request phase: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-dolphin-node-request-phase-pub1490-20260813-codex`
- Runtime inline create-tag probe: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-inline-probe-smoke3-20260813-codex`
- Dolphin inline create-tag probe: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-dolphin-inline-probe-pub1490-20260813-codex`
- Dolphin inline hex-spec smoke: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-dolphin-inline-probe-hexsmoke-20260813-codex`
- Runtime node/create return-memory probe: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-node-create-return-mem-pub350-20260813-codex`
- Dolphin node/create return-memory probe: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-dolphin-node-create-return-mem-pub1490-20260813-codex`
- Runtime id-3 tag watch: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-id3-tag-watch-pub350-20260813-codex`
- Runtime id-3 tag watch with XFB publication stamps: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-id3-tag-watch-pubstamp-pub350-20260813-codex`
- Dolphin id-3 tag timer/state probe: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-dolphin-id3-tag-timer-probe-pub1490-20260813-codex`
- Runtime node/clear-overlap re-anchor with XFB publication stamps: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-node-clearoverlap-pubstamp-pub500-20260813-codex`
- Dolphin id-3 tag/list removal probe: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-dolphin-id3-tag-list-removal-pub1490-20260813-codex`
- Runtime list-remove caller probe: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-list-remove-callers-pub500-20260813-codex`
- Dolphin list-remove caller probe: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-dolphin-list-remove-callers-pub1490-20260813-codex`
- Runtime tag-helper entry/flag probe: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-tag-helper-entry-flags-pub500-20260813-codex`
- Dolphin tag-helper entry/flag probe: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-dolphin-tag-helper-entry-flags-pub1490-20260813-codex`
- Runtime `0x80AC10D8` watch: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-80ac10d8-watch-pub330-20260813-codex`
- Dolphin `0x80AC10D8` phase probe: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-dolphin-80ac10d8-phase-pub1490-20260813-codex`
- Runtime `0x80AC10D8` phase-return probe: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-80ac10d8-phase-return-pub330-20260813-codex`
- Runtime `0x802356E0` internals probe: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-runtime-802356e0-internals-pub330-20260813-codex`
- Dolphin `0x802356E0` internals probe: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-dolphin-802356e0-internals-pub1490-20260813-codex`

Earlier useful comparison bundle:

- `F:\Projects\WindWakerRecomp\captures\beads-u2x2-gate-writer-compare-20260813-codex`

## Beads

Primary issue: `beads-u2x.2`

Local Beads state is current enough to identify the active JPADraw/list scheduling boundary. Remote push has been failing because the central Dolt remote is missing a referenced remote data ref, so local issue notes may be ahead of remote state.

## Immediate Next Step

The next correctness target is no longer finding the title-attract director. The director is pinned:

- Active demo manager pointer: `0x803CA6D0 -> 0x80AC11EC` on both runtime and Dolphin.
- Runtime pub 2203: demo frame counters `+0xD4/+0xD8 == 1710`.
- Dolphin pub 3200: demo frame counters `+0xD4/+0xD8 == 1713`.
- Matched visual oracle: `F:\Projects\WindWakerRecomp\captures\beads-u2x2-demo-frame-matched-compare-20260813-codex\demo-frame-contact.png`

Goal of the next probe: trace writes to the scratch source matrix at `0x8040CC18` on both runtime and Dolphin around the matched water pass. The immediate question is whether runtime misses a camera-composition writer entirely, runs it too early/late, or overwrites the composed matrix back to yaw-only before the direct upload.
