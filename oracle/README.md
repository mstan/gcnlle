# oracle/ — Dolphin as a differential oracle

For independently authored runtime components, [Dolphin](https://dolphin-emu.org)
is a separately authored system-level comparison target. It boots the real
GameCube IPL as "GameCube Main Menu" and provides traces for the same boot that
this project recompiles.

That independence claim does **not** apply to Dolphin-derived components. The
DSP LLE interpreter under `runtime/dsp_lle/` is vendored from Dolphin, and the
memory-card image module follows Dolphin's documented layout and algorithms.
Those components use focused tests, format checks, firmware behavior, and
hardware documentation as applicable; agreement with Dolphin alone is not
treated as independent evidence.

## Plan (Milestone 1)

Mirror psxrecomp's `beetle_libretro` pattern: a small headless frontend that
drives Dolphin, boots the IPL, and emits a per-step trace (PC, registers,
low-memory writes) as JSONL in the **same shape** our runtime emits, so a
diff tool can compare the two by value + order.

Options, cheapest first:
- Dolphin's scripting / debugger interface or `--batch` movie playback to log
  PC/register/mem traces.
- A libretro Dolphin core (if used) behind the same trace frontend shape.
- Failing those, a thin patch to a local Dolphin build exposing a trace tap.

Dolphin binaries/cores are **fetched, not vendored** (see `.gitignore`).

## Discipline

Diff by value + order, not frame number. Watch the same milestone PCs on both
sides, capture the caller (return address) on entry, compare the SEQUENCE.
Reproduce by driving the menu; never align savestates across the two builds.
