# gcnrecomp — working notes for Claude

Current handoff: [`docs/HANDOFF_2026-08-09.md`](docs/HANDOFF_2026-08-09.md).
The COW implementation is local commit `a2a90cc` on branch
`experiment/moderngekko-gpl`; WindWakerRecomp still pins `8a2e0d2`. Do not
describe the COW result as title-pinned or as proof of 60-Hz presentation.

Read **PRINCIPLES.md** first; it governs everything. Highlights that bite here:

- **LLE-first is the whole point.** We recompile the real GameCube **IPL** and
  model the hardware under it. Do NOT drift toward HLE-ing the boot the way
  gcrecomp/reshine/GXRuntime do (they fake post-BS2 low-mem and jump into a
  game DOL). HLE is only ever a *validated per-subsystem replacement* on top of
  a proven LLE baseline — never the baseline, never fake-the-answer.
- **Dolphin is the independent oracle.** It boots the real IPL as "GameCube
  Main Menu" and shares none of our device models, so it can catch shared-layer
  bugs our own interpreter can't. Diff by value+order (PC / register / low-mem
  write order), not by frame alignment. Reproduce by driving the menu, never by
  aligning savestates.
- **Screenshot before asserting visible state.** "reached the logo",
  "at the card manager", "black screen" — capture through the debug surface
  first, then describe pixels.
- **Fix the recompiler/runtime, not the generated C.** Generated code is
  evidence, not authority. No per-game hints (there is no "game" yet — it's
  firmware); config holds only genuine facts (IPL region/revision, entry
  points, RAM/MMIO map).

## Layout

- `recompiler/` — forked DolRecomp (PPC→C), GPL-3.0. Modules: `dr_frontend`
  (decode/containers), `dr_analysis` (CFG/jumptables), `dr_backend` (C emit),
  `dr_cpu`, `dr_platform`, `dr_app`. Vendored @ commit in `recompiler/UPSTREAM.md`.
  Retarget its slice-walker at `bios/ipl.bin`.
- `runtime/` — LLE host (net-new). See its CMakeLists for the device-module
  plan. Harvest GXRuntime's C EXI/memcard/VI models (GPL-3.0); use reshine's
  runtime + `docs/` as architectural reference.
- `bios/` — your own `ipl.bin` dump; gitignored (copyrighted firmware).
- `oracle/` — Dolphin trace frontend (mirror psxrecomp `beetle_libretro`).
- `tools/` — `dtk` (decomp-toolkit) for DOL/REL/disc parsing + disasm cross-checks.
- `docs/ROADMAP.md` — milestones M0..M5.

## Build

`./build.sh` (exports `/c/msys64/mingw64/bin` onto PATH — without it CMake's
try-compile fails with a bare "cannot compile a simple test program" because
gcc can't find its own runtime DLLs). The current COW checkpoint builds in both
Vulkan and `GCN_VULKAN=OFF` configurations and passes the 14-test runtime suite
in each configuration when the MinGW CTest runner is used.

## Reference clones (surface-investigated, kept out of tree)

Under the session scratchpad `repos/`: DolRecomp, reshine, GXRuntime,
gcrecomp, decomp-toolkit, aurora, GameCubeRecompiled, NWiiRecomp. See
`docs/ROADMAP.md` "Asset used from each investigated repo" for the verdict on
each.
