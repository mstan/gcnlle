# gcnrecomp

A static recompiler for the **Nintendo GameCube**, in the psxrecomp mold:
**LLE-first**. The near-term goal is not to run games — it is to statically
recompile the GameCube **IPL** (the console boot ROM) and boot into the
console's own menu: the animated rolling-cube logo, the main menu, the
**memory-card manager**, **date/time** and sound/screen options, and the
disc-load screen. Exactly what PSXRecomp does for the PlayStation BIOS.

## Status

Early scaffold.

- `recompiler/` — **builds green (10/10 tests)**. Fork of
  [ExpansionPak/DolRecomp](https://github.com/ExpansionPak/DolRecomp), a
  PowerPC (Gekko/Broadway) → C static recompiler, being retargeted from game
  DOLs to the IPL. See `recompiler/UPSTREAM.md`.
- `runtime/` — LLE host skeleton (compiles a placeholder). Device models to
  come: MEM1, EXI (RTC/SRAM/memory cards), VI, GX, DSP/AI, DI, SI.
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

**GPL-3.0** — this project forks GPL-3.0 code (DolRecomp) and harvests
GPL-3.0 device models, so the whole is GPL-3.0. See `LICENSE`.
