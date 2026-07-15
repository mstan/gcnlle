# gcnrecomp

A static recompiler for the **Nintendo GameCube**, in the psxrecomp mold:
**LLE-first**. The near-term goal is not to run games — it is to statically
recompile the GameCube **IPL** (the console boot ROM) and boot into the
console's own menu: the animated rolling-cube logo, the main menu, the
**memory-card manager**, **date/time** and sound/screen options, and the
disc-load screen. Exactly what PSXRecomp does for the PlayStation BIOS.

## Status

IPL milestones M0–M5 are complete: the recompiled boot ROM reaches and runs
the native menu, including the rolling-cube animation, calendar, memory-card
manager, options, and disc-detection screen.

- `recompiler/` — **builds green (13/13 tests)**. Vendored from the public
  [gcnrecomp DolRecomp integration fork](https://github.com/mstan/DolRecomp),
  based on the canonical
  [ExpansionPak/DolRecomp](https://github.com/ExpansionPak/DolRecomp). Its
  reviewable fork history separates IPL/runtime-specific work from changes
  that may be useful upstream. See `recompiler/UPSTREAM.md`.
- `runtime/` — LLE models for MEM1, EXI/RTC/SRAM/memory cards, VI, GX,
  DSP/AI, DI, and SI, validated against byte-exact frame-buffer goldens and
  Dolphin MMIO traces.
- Uniform-cycle interactive boots run in real time. Derived-cycle performance
  work remains active; statically recompiled commercial-game execution is
  intentionally out of the current scope.
- `bios/` — you supply your own `ipl.bin` dump (not distributed).
- `oracle/` — Dolphin as the independent differential oracle.
- `tools/` — decomp-toolkit (`dtk`) for binary/disc wrangling.

See **[docs/ROADMAP.md](docs/ROADMAP.md)** for milestones (M0 IPL descramble →
M5 disc screen) and **[PRINCIPLES.md](PRINCIPLES.md)** for the LLE-first,
oracle-validated discipline this project follows.

## Build

```bash
./build.sh          # sets up mingw64 PATH, builds recompiler (+ runtime)
```

Requires CMake + Ninja + a C11 compiler (msys2 mingw64 gcc tested). The
recompiler is portable C11; the runtime will target Windows first.

## License

**GPL-3.0** — the combined project includes GPL-3.0 DolRecomp code and
GPL-2.0-or-later Dolphin-derived components distributed under GPLv3-compatible
terms. See `LICENSE` and the component provenance notes.
