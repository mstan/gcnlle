# COSIM_DESIGN.md — invariant-point XFB cosimulation against the Dolphin oracle

Status: **design only**, owner-approved priority, no code changed by this doc.
Companion to `docs/WIND_WAKER_COSIM.md` (CPU/RAM lockstep cosim, GDB-stub based)
and `oracle/PATCH_PLAN.md` (Dolphin trace-tap patch spec). This document adds
the piece neither of those covers: comparing **rendered output** — the actual
XFB bytes VI scans out — at deterministic anchors, so we catch bugs that are
invisible to CPU/RAM equality (rendering-only corruption) and invisible to our
own golden-chain (shared-model gaps both our paths omit identically).

Methodologically this is the **DIVERGENCE-INSTRUMENTS.md** family from
`recomp-template` (always-on capture on both sides, aligned *after the fact* by
ordinal), not `DIFFERENTIAL-COSIMULATION.md`'s deterministic lockstep: Dolphin's
VI/AI/renderer free-run once booted, so we cannot single-step it through a full
gameplay route the way `gcn_cosim.py`'s existing GDB-based `ab-step`/`ab-milestone`
commands single-step the shared BS2 boot window. XFB-copy events are the ruler
here, exactly as VBLANK-ordinal alignment is the ruler in the PSX case that doc
cites.

## 0. What already exists (read before building anything)

**Two counters, two anchor granularities, both already live in `runtime/src/gx.c`:**

- **Publication** (fine-grained, primary anchor) — one per EFB→XFB copy.
  `s_xfb_hash_pubs` (`gx.c:452`), incremented in `gcn_gx_xfb_hash_publish_done()`
  (`gx.c:490-500`), called at the software-raster EFB→XFB copy site
  `runtime/src/gx_raster.c:8373-8492` (guarded by `gcn_gx_xfb_write_begin/end`,
  fed via `gcn_gx_xfb_hash_feed(s_cpu->ram + phys, dest_stride, src_w*2u, dst_h)`
  at `gx_raster.c:8487`, published at `gx_raster.c:8492`) and mirrored in the GPU
  path `runtime/src/gx_vulkan.c:1592-1609`. This is the direct analogue of
  "one dump per XFB copy-out" the mission calls the preferred anchor.
- **DrawDone / frame generation** (coarse, secondary anchor) — one per PE
  `SETDRAWDONE` token. `s_xfb_generation` (`gx.c:147`), incremented at
  `gx.c:1431` inside the `GX_BP_SETDRAWDONE` case (`gx.c:1425-1433`), read via
  `gcn_gx_xfb_generation()` (`gx.c:502-504`), reset on boot at `gx.c:1749`.
  A frame can contain zero, one, or several publications before its DrawDone;
  use this only for coarse cross-checks ("did frame N publish anything"), never
  as the primary index — it is NOT 1:1 with rendered output.

Both counters are **gated independently**: publication counting/hashing only
runs when `GCN_GX_XFB_HASH=1` (`gx.c:454-459`); the generation counter is
**always on** (unconditional atomic add). The new dump knob below must be
**always-countable but opt-in to dump**, matching the existing hash knob's
shape so the two can run together without interference.

**Existing per-publication instrumentation to imitate, not duplicate:**
- `GCN_GX_XFB_HASH=1` — chained FNV-1a-64 over every published byte (`gx.c:450-500`).
- `GCN_GX_XFB_HASH_EVERY=1` — print the chain at every publication instead of
  every 256th (`gx.c:479-497`).
- `gcn_gx_xfb_hash_get_state`/`_set_state` (`gx.c:516-522`) — snapshot save/resume
  seeds the chain and pub-count so a resumed run **continues** the sequence
  rather than restarting it; seeded from `runtime/src/snapshot.c:591-598` (save)
  and `:1214-1230` (restore). This is the exact mechanism anchor alignment across
  a snapshot-resumed run must reuse (§1).

**Existing screenshot/dump command to imitate:**
`debug_server.c:743-793` (`screenshot`/`screenshot_file`) already: (a) drains
the GX pipeline (`gcn_gx_pipeline_drain()`, `:755`) so it reads the same bytes
a synchronous design would, (b) reads XFB geometry from `gcn_vi_xfb_info()`
(`:758`), (c) decodes YUY2→RGB using the documented inverse BT.601 matrix
matching Dolphin's `TextureConversionShader.cpp:1009-1035` (comment at
`:747-749`), (d) writes a header-only PPM, no image-library dependency. The new
per-publication dump reuses (a)-(d) but triggers automatically at the
publish-hook site instead of on-demand, and additionally offers **raw YUY2
bytes** (§3) since that is what the hash already consumes and what a byte-exact
comparator wants — RGB conversion is a lossy step both sides would have to
agree on bit-for-bit, which is unnecessary risk.

**Existing memcard auto-format (the alignment hazard the mission flags):**
`runtime/src/boot.c:291-401` (`setup_memcard`) auto-creates an in-memory
factory-blank card for slot A by default (`:582-584` comment: "Slot A defaults
to a formatted card (EXT=1, matching the oracle)"), sized via
`GCN_MEMCARD_A_MBITS` (`:260-269, 308-309`). `tools/gcn_memcard/main.c`
(`format`/`import`/`list`/`check` subcommands, header comment `:1-13`) is a
standalone CLI over the same `memcard_image.h` that already mints
Dolphin-compatible `.raw` images — this is the tool to generate the **shared**
fixture in §5, not new code.

**Existing GDB-based CPU/memory cosim (sibling tool, do not conflate):**
`tools/gcn_cosim.py` talks to Dolphin over its GDB remote stub (`GdbRemote`
class, `:60-233`) and to the runtime over `GCN_DEBUG_PORT` JSON (`JsonTcp`,
`:36-57`), for `gate1`/`gate3` (A-vs-A + injected-divergence validation),
`dolphin-probe`, `dolphin-run-to`, `ab-initial`/`ab-step`/`ab-milestone`
(shared-BS2-entry register/RAM lockstep, with an explicit, documented Dolphin
fake-timebase seam at `:1137-1181`). This tool proves **CPU/RAM** equality
through the boot window under true lockstep. It cannot single-step Dolphin
through a full gameplay route (too slow, and VI/AI free-run once past boot) —
this is why the XFB cosim below is a **separate**, ordinal-aligned instrument,
added as new subcommands in the same file (§4), not a replacement.

**Existing psxrecomp pattern actually mirrored by "beetle_libretro"** (re-read;
this differs from the assumption in the task brief): psxrecomp does not run a
*separate* Beetle process compared out-of-band. `runtime/src/beetle_libretro.cpp`
**statically links** the beetle-psx core in-process and exposes the identical
JSON/TCP debug protocol on its own port (`docs/beetle-linux.md:1-3, 10-30`), so
one generic client tool (their `find_divergence.py` family) talks to native and
oracle over the *same wire shape*. GameCube cannot do this for Dolphin: Dolphin
is GPL-2.0-or-later, architecturally enormous, and `PATCH_PLAN.md`'s own
independence argument (§ "Independence", `PATCH_PLAN.md:9-16`) requires it stay
a separate process with no shared code — so gcnrecomp already correctly departed
from a literal beetle_libretro port in favor of the GDB-stub / trace-tap hybrid
in `oracle/`. The one part of the pattern worth preserving here is "same wire
shape on both sides" — this design's Dolphin-side dump and runtime-side dump
(§2, §3) use one shared binary anchor-record format for exactly that reason.

**Dolphin source IS in tree and already partially patched.**
`oracle/dolphin` is a full Dolphin checkout (`git remote -v` →
`https://github.com/dolphin-emu/dolphin.git`, HEAD `6de526c`). `git status`
shows **modified**: `Source/Core/Common/Assembler/GekkoIRGen.cpp`,
`Source/Core/Core/HW/EXI/EXI_Channel.cpp`, `Source/Core/Core/HW/MMIO.h`,
`Source/Core/Core/PowerPC/Interpreter/Interpreter.cpp`,
`Source/Core/DolphinNoGUI/MainNoGUI.cpp` (82 lines added — almost certainly the
`--boot-gc-ipl` flag from `PATCH_PLAN.md` §d1), and **untracked**:
`Source/Core/Core/GcnTrace.h`, `Source/Core/Core/GcnTraceFormat.h`. So tiers (a)
retired-instruction trace, (b) MMIO, (c) EXI from `PATCH_PLAN.md` are wired.
Tier (f) — GX FIFO / PE / **VI XFB events, the thing this design needs** — is
**not** wired: `GcnTraceFormat.h` already reserves `GCN_TR_GX`/`GCN_TR_VI`
record types and full payload structs (`gcn_tr_gx` at `:194-204`, `gcn_tr_vi` at
`:206-215`, including `GCN_VI_XFB_SET` at `:206`), but `GcnTrace.h` only
implements `EmitMmio` (`:232-245`) and `EmitExi` (`:249-279`) — there is no
`EmitVi`/`EmitGx`, and `git diff --stat` confirms `VideoCommon/*.cpp` and
`HW/VideoInterface.cpp` are untouched. **This is the gap §2 closes.** Also
untracked: `udir-ww-cadence-run3/` (a prior trace-capture run directory) and
`Projectsgcnrecomporacledolphin-user/` (a stray/misplaced user-dir artifact —
flag for cleanup, not part of this design).

## 1. Anchor semantics

**Primary anchor: publication-k.** Our side counts one integer per completed
EFB→XFB copy (`s_xfb_hash_pubs`, gated by `GCN_GX_XFB_HASH=1` today; §3 makes
counting unconditional and dumping separately opt-in). Dolphin's side must
count the identical event: one increment per completed
`TextureCacheBase::CopyRenderTargetToTexture` (or the VideoSoftware equivalent —
confirm exact function name on the pinned checkout; `PATCH_PLAN.md` §f already
names `VideoCommon/PixelEngine.cpp` for PE events and `Core/HW/VideoInterface.cpp`
for VI/XFB-set, but the actual EFB→XFB **copy** call happens earlier in the
copy-execute BP-write handler inside `VideoCommon/CommandProcessor.cpp` /
the per-backend `TextureCacheBase::CopyRenderTargetToTexture` override — for
`-v Software` headless this resolves to `VideoSoftware`'s copy path, which
must be located and confirmed on the pinned checkout before coding; see §2).
**Secondary anchor: DrawDone-k**, mirroring `s_xfb_generation` — one per PE
`SETDRAWDONE` token (`PixelEngine.cpp`'s finish/token handling), used only to
sanity-check that publication count *within* a DrawDone interval matches
between the two sides, never as the primary comparison index.

**What guarantees index alignment from a common reset:** both counters reset
to 0 at a true cold boot of the IPL menu (ours: `gx.c:1749`; Dolphin's: process
start with a fresh `CopyRenderTargetToTexture` call count). From a shared reset,
publication-k on both sides refers to the same guest event **by construction**
— both models process the identical guest CP/BP FIFO stream that the recompiled
BS2/IPL code emits, and neither backend can materialize a publication the guest
program didn't issue. This is the same "shared physical event sequence, aligned
by ordinal" guarantee `DIVERGENCE-INSTRUMENTS.md` documents for device-IRQ
ordinals (see its "Aligning two independently-run processes" section) — publish
ordinal is a strict per-run invariant as long as both sides consume the same
GX command stream, which they do by definition of being two implementations of
the same guest program.

**What breaks it — the two documented hazards, and the handshake for each:**

1. **Snapshot-resumed run vs Dolphin true-reset.** Our runtime can resume from a
   BIOS-skip snapshot (`docs/SNAPSHOT_RESUME.md`) whose publication count is
   already nonzero (seeded via `gcn_gx_xfb_hash_set_state`, `gx.c:520-522`, from
   `snapshot.c:1223-1230`). Dolphin, booting the real IPL cold, starts its own
   count at 0 and must first *replay* the BS2/apploader/IPL-menu boot sequence
   before reaching the guest PC where our snapshot begins — during which it
   racks up its own publication count from 0. **Required handshake: a
   publication-offset handshake, derived once per snapshot, not assumed.**
   Concretely: record, at snapshot-save time, the publication count Dolphin
   independently reaches at the *same guest PC* the snapshot captures (measured
   once via a true-reset Dolphin boot to that PC, using `gcn_cosim.py`'s
   existing `dolphin-run-to --pc <snapshot_pc>` machinery, §4) — call this
   `dolphin_offset`. Comparisons then align `our publication (chain_pubs_seed + i)`
   with `Dolphin publication (dolphin_offset + i)`. Store `dolphin_offset`
   alongside the snapshot file (extend the snapshot format's XFB-chain section,
   `snapshot.c:591-598`, with one more `u64`) so it travels with the fixture
   instead of being re-derived ad hoc.
2. **True-reset overlap segment (fallback / validation gate).** Before trusting
   any snapshot-resumed comparison, run **both sides from true cold reset**
   through the *first N publications after the snapshot point* and confirm the
   offset-adjusted indices already agree byte-for-byte (§6 "known-good segment"
   gate). This overlap-segment run is the proof that the handshake in (1) is
   correct, not a one-time assumption — it is cheap (N ~ a few dozen
   publications covering one full IPL-menu screen) and must be re-run whenever
   the snapshot point changes.
3. **Card-state and input alignment** (§5) — if either side takes a different
   guest-visible branch (blocked on a "format this card?" dialog, or a
   different pad state), publication k on one side is not the same guest event
   as publication k on the other even though both counters read the same
   integer. Card-state/input alignment is therefore a **precondition** for
   anchor validity, not an independent concern — a divergence report from a
   misaligned run is meaningless and must be distinguished from a real bug
   (§5, §6).

## 2. Dolphin-side patch spec

**Hook site (needs confirmation on the pinned checkout, per PATCH_PLAN.md's
own verification discipline):** the EFB→XFB copy is dispatched from the CP
BP-register handler that services `BPMEM_TRIGGER_EFB_COPY` in
`Source/Core/VideoCommon/BPStructs.cpp` (this is Dolphin's analogue of our
`gx.c:1424` `GX_BP_SETDRAWDONE`-style BP switch — the copy-trigger case, not
draw-done, is the one to hook), which calls into the active backend's
`TextureCacheBase::CopyRenderTargetToTexture` (`VideoCommon/TextureCacheBase.cpp`,
overridden per-backend). For `-v Software` (headless, matching `oracle/README.md`
Milestone-1 intent and the existing `dolphin_trace*.bat` scripts) this resolves
to the VideoSoftware backend's copy-to-XFB path — locate its exact function on
the pinned commit before writing code; `PATCH_PLAN.md`'s own "NOT independently
verified" section (`:320-334`) already flags this class of claim as
grep-before-trusting, and this hook is new (not in that file), so it inherits
the same discipline.

**Dump format: raw XFB bytes, not PNG — closest to what our chain hashes.**
One record per copy-out:
- A small **fixed header** appended to the existing `gcn_trace_record` shape
  (reuse `GCN_TR_VI` type, `gcn_tr_vi` payload, `GcnTraceFormat.h:206-215`) so
  the anchor event rides the *same* trace stream already flowing for tiers
  (a)-(c) — this is the "same wire shape" principle from psxrecomp (§0). Set
  `event = GCN_VI_XFB_SET` (already defined, `:206`), `xfb_addr` = the
  destination address Dolphin just copied to, and add the publication ordinal
  into the existing `line` field's reinterpreted role for this record type
  (a one-line comment change; do not renumber the enum) — or, more robustly,
  extend `gcn_tr_vi` with an explicit `uint64_t pub_seq` before the next
  version bump, since overloading `line` is fragile. **Recommend the explicit
  field**; bump `GCN_TRACE_VERSION` (`GcnTraceFormat.h:45`) accordingly, update
  the `_Static_assert` sizes (`:274-285`).
- The **pixel payload** goes to a **sibling file**, not inline in the 204-byte
  trace record (raw XFB spans are far larger than one record): write
  `<dump_dir>/dolphin.<pub_seq>.yuy2` — a raw dump of exactly the copied span
  (`width*2 bytes/row * height rows`, YUY2, no padding, no header beyond a tiny
  fixed prefix `width:u32,height:u32,stride:u32` matching what our side emits,
  §3) so the two sides' files are byte-comparable without a schema lookup.
- **New tap function** `GcnTrace::EmitXfbCopy(pc, addr, width, height, stride,
  const uint8_t* pixels)` added to `GcnTrace.h` next to `EmitMmio`/`EmitExi`
  (`:232-279`): writes the `GCN_TR_VI` trace record (ordering proof) and, when
  `GCN_TRACE_XFB_DUMP` env var names a directory, also writes the sibling pixel
  file. Gate the (expensive) pixel write independently from the (cheap) ordinal
  record, mirroring our own two-tier `GCN_GX_XFB_HASH` / new-dump-knob split
  (§3) — always count, optionally dump.
- **Config knob:** reuse the already-verified `-C`/`--config` mechanism
  (`PATCH_PLAN.md` §e) — no new CLI flag needed for the dump *destination* path,
  since env vars (`GCN_TRACE_XFB_DUMP=<dir>`) are simpler and match the existing
  `GCN_TRACE_OUT`/`GCN_TRACE_MAX_INSNS` pattern already used by `GcnTrace.h`
  (`:69, 78-85`).
- **Expected overhead:** one `memcpy` + one buffered file write per copy, on the
  order of the XFB size (typically ≤ 640×480×2 bytes ≈ 600 KiB, usually far
  smaller for the IPL menu's actual programmed dimensions). At IPL-menu
  publication rates (well under 60/s — the menu does not redraw every field)
  this is negligible; for a gameplay route with per-field publication it is a
  few tens of MB/s, acceptable for a diagnostic run and boundable via
  `GCN_TRACE_XFB_DUMP_EVERY=N` (dump every Nth publication) mirroring the
  existing `GCN_GX_XFB_HASH_EVERY` knob shape (`gx.c:476-497`) for long soaks.

## 3. Our-side spec

Add a new env-gated dump alongside `GCN_GX_XFB_HASH`, at the **same two call
sites** that already feed the hash (`gx_raster.c:8487` software path,
`gx_vulkan.c:1604` GPU path), immediately before `gcn_gx_xfb_hash_publish_done()`
so it captures the identical byte span with the writer lock still held
(`gcn_gx_xfb_write_begin/end`, `gx_raster.c:8373` / `:8491`):

- **New knob `GCN_GX_XFB_DUMP=<dir>`** (mirrors `GCN_GX_XFB_HASH=1` shape,
  `gx.c:454-459`): when set, on every publication write
  `<dir>/runtime.<pub_seq>.yuy2` with the same tiny prefix
  (`width:u32,height:u32,stride:u32` little-endian, matching Dolphin's §2
  format byte-for-byte) followed by exactly `stride*height` raw bytes copied
  from `s_cpu->ram + phys` (the same source `gcn_gx_xfb_hash_feed` reads,
  `gx_raster.c:8487`) — i.e. literally the same bytes already fed to the hash,
  just also written to disk. Add `GCN_GX_XFB_DUMP_EVERY=N` mirroring
  `GCN_GX_XFB_HASH_EVERY` (`gx.c:476-497`) for long soaks.
- **Unconditional counting.** Per §0, decouple "count this publication"
  (`s_xfb_hash_pubs++`) from "hash this publication" so the ordinal is stable
  and query-able (`cosim_status`-style debug command, see below) even when
  `GCN_GX_XFB_HASH=0` — currently the counter only exists behind the hash gate
  (`gx_xfb_hash_on()` check at `gx.c:463, 491`). Promote `s_xfb_hash_pubs` to an
  always-incrementing counter (rename to `s_xfb_pub_count` to stop conflating
  "count" with "hash"); keep the hash itself opt-in. This is a small, root-cause
  fix (the counter's current name and gating conflate two concerns), not a
  workaround.
- **Naming/indexing convention shared with Dolphin's dumps:** `runtime.<k>.yuy2`
  / `dolphin.<k>.yuy2` in the **same directory**, with `k` the publication
  ordinal **after** the §1 offset handshake is applied (i.e. the comparator, not
  the producers, does the offset arithmetic — each producer writes its own
  native ordinal; §4's comparator subtracts `dolphin_offset` when pairing files).
  This keeps each producer simple and keeps the alignment logic in exactly one
  place (the comparator), matching the "one central choke point" principle from
  `DIVERGENCE-INSTRUMENTS.md` (§0 "Why one central choke point per event class").
- **Debug-server exposure:** add `xfb_pub_count` to `debug_server.c`'s command
  table (next to `gx_draw_state`, `:732`) returning `{"ok":true,"pub_count":N,
  "generation":M}` (both counters, §0) — lets `gcn_cosim.py` poll "how many
  publications has the runtime reached" without needing the dump knob enabled,
  useful for driving the runtime to a specific publication count before
  triggering a Dolphin-side comparison window.

## 4. Comparator tool spec

Extend `tools/gcn_cosim.py` (do not replace — its GDB/JSON-based CPU cosim
commands, §0, remain the right tool for the boot window) with a **new
subcommand family** that does not require Dolphin's GDB stub at all, since XFB
comparison only needs file-system access to the two dump directories from §2/§3:

```
gcn_cosim.py xfb-diff --runtime-dir <dir> --dolphin-dir <dir> \
    --dolphin-offset <k> [--perceptual-fallback LAYER...] [--first-n N]
```

- **Exact-byte compare first-class.** For each ordinal `k` starting at 0, read
  `runtime.<k>.yuy2` and `dolphin.<k + dolphin_offset>.yuy2`; if either is
  missing, report `"pairing_gap"` (distinguish "runtime ran ahead/behind" from
  "found a real divergence" — a gap is a harness bug or a card/input
  misalignment per §1 item 3, never itself the finding). Compare the fixed
  prefix (`width/height/stride`) first — a geometry mismatch is reported
  distinctly from a pixel mismatch, since a geometry mismatch usually means a
  VI-register-programming divergence, not a rendering-content divergence, and
  should be triaged differently. Then compare payload bytes exactly (`==`,
  no tolerance) — this is deliberate: our own instrumentation already proved
  determinism (golden chain, `docs/DOLPHIN_AUDIT.md`), and Dolphin's
  VideoSoftware backend is likewise deterministic for a fixed CPU-interpreter,
  fixed-RTC, no-audio-jitter configuration (`PATCH_PLAN.md` §d determinism
  knobs) — so exact equality is the correct default, not an optimistic one.
- **Perceptual fallback only for documented-nondeterministic layers.** The one
  known nondeterministic layer is anti-aliasing/dithering choices that differ
  between our rasterizer and Dolphin's *if* either ever samples host-timing-
  derived jitter — not expected for VideoSoftware, but if `--perceptual-fallback`
  is passed with a named layer (e.g. `dither`), fall back to a documented
  perceptual delta (mean absolute per-channel difference after YUY2→RGB
  decode, reusing the exact BT.601 matrix cited in `debug_server.c:747-749`)
  **only for that named layer**, and always print the exact-byte result too so
  the fallback never silently hides a real divergence. Default: no fallback
  layers enabled — a run that needs one must say so explicitly and it appears
  in the report.
- **Divergence report format**, modeled on the existing `compare_states` /
  `full_byte_audit` reports (`gcn_cosim.py:346-450`) and the corun's plane
  checks: first divergent `k`, the guest `pc`/instruction-count if available
  from a coincident CPU-cosim run, geometry (width/height/stride) for both
  sides, a byte-offset of the first differing pixel, and a **per-region
  localization** — partition the frame into a coarse grid (e.g. 8×8 tiles) and
  report which tiles differ and by how much (count of differing bytes per
  tile), so "flat-blue ocean" (one large contiguous region, most tiles in one
  band differ) is visually distinguishable in the JSON report from "missing
  logo overlay" (a small rectangular region differs) without opening an image
  viewer. This directly answers the mission's two proven bug classes: TLUT-
  rollover magenta noise (beads-u2x.1) shows as scattered per-tile noise across
  many tiles at a specific `k`; missing-logo/flat-ocean (beads-u2x.2) shows as
  one geometrically coherent tile region differing consistently across a run of
  `k`s.
- **Output:** one JSON line per compared `k` (matching `gcn_cosim.py`'s existing
  `json.dumps(..., separators=(",", ":"))` convention throughout), plus a final
  summary line with first-divergence `k`, total compared, and pass/fail — same
  shape as `cmd_gate1`'s per-checkpoint report (`:453-501`).

## 5. Card-state + input alignment plan

**Card state.** Generate ONE shared factory-blank slot-A `.raw` image with the
existing tool: `gcn_memcard format fixtures/slotA.raw --mbits 16` (or whichever
size the target route needs — `tools/gcn_memcard/main.c:1-13`, backed by
`memcard_image.h`'s Dolphin-documented layout per `oracle/README.md:8-13`).
Feed it to **both** sides so neither side hits its own from-scratch
auto-format/no-save-file dialog path (our side would otherwise auto-format
in-memory per `boot.c:291-401`; Dolphin would otherwise block forever on the
no-save-file dialog cycle per the mission's stated known fact):
- Runtime: point `GCN_MEMCARD_A` at the shared file (existing env var,
  `boot.c:188-196` comment; `setup_memcard` at `:326` loads it if it exists
  rather than auto-creating).
- Dolphin: `-C Dolphin.Core.SlotA=1 -C Dolphin.Core.MemcardAPath=<same file>`
  (both keys already verified present in `Config/MainSettings.cpp` per
  `PATCH_PLAN.md:174-176, 304-306`).
Both configurations now see an **already-formatted** card at boot, matching
guest-visible behavior (no dialog on either side) and keeping publication
ordinals aligned (§1 item 3).

**Input alignment.** For the zero-input IPL-menu route this is moot (no pad
state affects publication timing meaningfully). For scripted routes:
- Runtime: `set_input` over the debug-server JSON protocol
  (`debug_server.c:795-829`) already exists — script it from a driver process
  that also watches `xfb_pub_count` (§3) to pace input to publication ordinals
  rather than wall-clock frames (never align by frame number, per
  `oracle/README.md:30-34` and the mission's explicit instruction).
- Dolphin: use its **movie (`.dtm`) input** mechanism, not the FIFO player —
  `-m/--movie` is already a verified-present CLI flag
  (`PATCH_PLAN.md:194-211, "-m, --movie"`), and TAS movies are Dolphin's
  standard deterministic-input replay path (frame-indexed pad state, immune to
  the CPUThread/idle-skip determinism knobs already forced off per §d). Record
  one `.dtm` per scripted route once, generated from the same button sequence
  the runtime's `set_input` driver script issues, so both sides consume
  logically identical input — the driver script becomes the single source of
  truth for both the runtime's live `set_input` calls and the recorded `.dtm`.
  (Dolphin's FIFO player replays only a GX command stream, not guest input; it
  is the wrong mechanism here since we need Dolphin to run the *real* recompiled
  guest boot, not a captured command list.)

## 6. Validation plan

1. **Known-good segment gate (prove the instrument before trusting it).** Run
   both sides zero-input through the IPL-menu boot with the shared card fixture
   (§5), dump every publication (§2/§3), run `xfb-diff` (§4) with
   `--dolphin-offset` derived per §1's handshake. Expect: **exact match**, every
   `k`, through however many publications the menu produces before any known
   divergence. This both proves the offset handshake is correct (§1 item 2's
   overlap-segment requirement) and gives a regression baseline. If this gate
   fails, the bug is in the harness (card/input misalignment, offset
   miscalculation, or a genuine — and very concerning — day-one divergence in
   the menu itself); do not proceed to §6.2 until it's green.
2. **Point it at the title vista (the real target).** Run the scripted route to
   the known missing-logo/flat-ocean transition (`beads-u2x.2`, cf.
   `docs/WIND_WAKER_COSIM.md`'s wave/scene fixtures and the memory note
   "Resident fly-in divergence" for the general shape of this kind of finding).
   Expected outcome, stated in advance so the gate is falsifiable: `xfb-diff`
   reports the **first** divergent `k` at or shortly after the guest publishes
   the frame containing the logo overlay / wave water, localized (§4's tile
   grid) to the region where the overlay/water should be, persisting across
   subsequent `k`s (a *feature gap*, not transient noise) — as opposed to the
   TLUT-rollover class (beads-u2x.1), which the instrument should localize as
   scattered per-tile noise at a specific, narrow `k` window. Getting a result
   that matches this expected *shape* (persistent geometric region vs scattered
   noise) is the acceptance bar for "the instrument works," independent of
   whether the specific route reproduces beads-u2x.2 on the first try.
3. **Regression re-run of §6.1** after any change to the Dolphin patch, the
   runtime dump knob, or the comparator — the known-good gate must stay green
   before any new divergence finding is trusted (mirrors the CPU-cosim's own
   gate1/gate3 validate-before-trust discipline, `gcn_cosim.py:453-533`).

## 7. Work breakdown (ordered, sized for subagents)

Each task lists file(s) touched and an acceptance criterion a subagent can
self-check without a human screenshot review (screenshot/visual review is
reserved for §6.2's final human-in-the-loop step per PRINCIPLES.md).

1. **Runtime: promote publication counting, add dump knob.**
   `runtime/src/gx.c`, `gx_raster.c:8487`, `gx_vulkan.c:1604`, `debug_server.c`.
   Rename `s_xfb_hash_pubs`→`s_xfb_pub_count`, decouple from the hash gate
   (§3), add `GCN_GX_XFB_DUMP`/`_EVERY`, add `xfb_pub_count` debug command.
   *Acceptance:* with `GCN_GX_XFB_HASH=0 GCN_GX_XFB_DUMP=<dir>`, running the IPL
   menu produces `runtime.0.yuy2, runtime.1.yuy2, ...` in `<dir>`, and
   `xfb_pub_count` over the debug port returns a matching count with the hash
   disabled — proves the decoupling.
2. **Dolphin: confirm exact copy-hook function on the pinned checkout.**
   `oracle/dolphin` (read-only investigation only, produces updated citations
   for §2). *Acceptance:* a comment block naming file+function+line for the
   `-v Software` EFB→XFB copy call site, verified by actually reading the
   pinned source (not inferred), added to `PATCH_PLAN.md` §f.
3. **Dolphin: implement `EmitXfbCopy` + `GCN_TRACE_XFB_DUMP`.**
   `oracle/dolphin/Source/Core/Core/GcnTrace.h`,
   `GcnTraceFormat.h` (version bump + `pub_seq` field), the hook site from
   task 2. *Acceptance:* booting the patched `DolphinNoGUI` on the IPL menu
   with `GCN_TRACE_XFB_DUMP=<dir>` produces `dolphin.0.yuy2, dolphin.1.yuy2,
   ...` with the same prefix format as task 1's runtime dumps, confirmed by a
   byte-level prefix-format unit check (no full-frame comparison needed yet).
4. **Shared card fixture + config wiring.** `tools/gcn_memcard` (no code
   change expected, just usage), a checked-in fixture generation note (not the
   binary card itself — gitignored like other binaries) or a build-time
   generation step. *Acceptance:* both a runtime boot with `GCN_MEMCARD_A=
   fixtures/slotA.raw` and a Dolphin boot with matching `-C` flags reach the
   menu with zero card-format dialog on either side (existing `screenshot`
   command, `debug_server.c:743`, used to confirm no dialog is showing).
5. **Comparator: `xfb-diff` subcommand.** `tools/gcn_cosim.py` (additive
   subcommand per §4; does not touch existing subcommands). *Acceptance:*
   run against two directories of hand-crafted identical `.yuy2` fixtures →
   reports 0 divergences; flip one byte in one file → reports exactly that
   `k`, that byte offset, and the correct tile in the localization grid
   (this is the injected-divergence proof, mirroring `gate3`'s discipline,
   before trusting the tool on real captures).
6. **Offset handshake tooling.** `tools/gcn_cosim.py` (extend
   `dolphin-run-to`, already capable of running Dolphin to a guest PC via its
   GDB stub, §0) to also report Dolphin's publication count at that PC — this
   likely needs a lightweight Dolphin-side query (e.g. an added GDB monitor
   command, or simply reading the `GCN_TR_VI` record count already in the
   trace file up to that point) rather than new infrastructure. *Acceptance:*
   for a fixed snapshot PC, three independent Dolphin true-reset runs report
   the identical `dolphin_offset` (proves it's a deterministic property of the
   boot sequence, not run-to-run noise) — this is task 1's precondition for §6.1.
7. **Run §6.1 (known-good gate) end to end.** Depends on 1-6. *Acceptance:*
   `xfb-diff` reports zero divergences across every publication in the IPL-menu
   zero-input boot window, with a machine-checkable exit code (already
   `gcn_cosim.py`'s convention — 0 pass / 1 fail, `main():1537-1541`).
8. **Run §6.2 (title-vista validation) and file findings as Beads issues.**
   Depends on 7. Per the user's global tracking policy, record the finding
   (or the absence of one, with evidence) under the appropriate `Game:`/`Meta:`
   epic in the Beads tracker, not only in this document — this document is the
   design; the finding is a defect record with its own acceptance criteria.

## Summary of open items / blockers

- **Dolphin source is in tree, tiers (a)-(c) of the trace tap are already
  patched** (`git status` in `oracle/dolphin`) — no "obtain Dolphin" blocker.
  The blocker is narrower: tier (f)'s exact VideoSoftware copy-hook function
  name is unverified on the pinned checkout (task 2 above resolves this).
- **Field-dump anchoring is confirmed a dead end** (per the mission's stated
  2026-08-10 finding: 1 PNG/field at 59.94, no dedup, per-field noise in
  VideoSoftware output defeats exact-hash) — this design does not fall back to
  it; publication-hook anchoring (§2) is the only path pursued.
- **Two stray untracked paths in `oracle/dolphin`** worth cleaning up before
  this patch grows: `Projectsgcnrecomporacledolphin-user/` (looks like a
  misconfigured `--user` path artifact) and `udir-ww-cadence-run3/` (a prior
  capture run) — flagged, not touched by this design.
