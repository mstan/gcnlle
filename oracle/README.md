# oracle/ — Dolphin as the independent differential oracle

Per PRINCIPLES.md, the oracle must be a **separately-authored emulator** that
shares none of our device/bus/timing models — otherwise it is blind to any bug
in a shared layer. [Dolphin](https://dolphin-emu.org) fits: it is unrelated to
our recompiler and it **boots the real GameCube IPL** as "GameCube Main Menu"
(Options → set a GC IPL/BIOS, or run the IPL directly), giving us a reference
for the exact boot we are recompiling.

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
