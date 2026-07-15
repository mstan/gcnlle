# gcnrecomp

An experimental, LLE-first static recompiler for the **Nintendo GameCube IPL**.
The project recompiles the console boot ROM and models enough hardware to run
its native rolling-cube logo, menu, calendar, settings, memory-card manager,
and disc-detection screen.

> **Early development:** this is research software, not a general GameCube
> emulator and not ready for ordinary game use. Interfaces, build steps, and
> behavior can change without notice.

## Current state

- The recompiled IPL reaches and runs its native menu on Windows.
- EXI, SRAM, RTC, memory-card, VI, GX, DSP/AI, DI, and SI models cover the
  firmware paths exercised so far.
- The RTC can sample host local time **once at boot** and then advances only
  from emulated CPU cycles; it does not continuously substitute host time.
- Memory-card image validation and the IPL's copy and erase paths have been
  exercised end-to-end on Dolphin-compatible raw images, including persisted
  journal verification after each operation.
- The software renderer remains the correctness baseline for headless/oracle
  runs. Interactive Windows launches use the exact resident Vulkan backend by
  default, with synchronized software fallback for unsupported state, and a
  vsynced double-buffered WGL presenter so DWM never observes a partial frame.
  Set `GCN_GX_BACKEND=software` to force the reference renderer or `GCN_GL=0`
  to select the diagnostic GDI presenter.
- Commercial-game recompilation and execution are outside the present scope.

The detailed milestone history is in [docs/ROADMAP.md](docs/ROADMAP.md), and
the current performance work is in [docs/PERF_CAMPAIGN_3.md](docs/PERF_CAMPAIGN_3.md).

## Source layout

- `recompiler/` — a pinned snapshot of the public
  [gcnrecomp DolRecomp integration fork](https://github.com/mstan/DolRecomp),
  based on the official
  [ExpansionPak/DolRecomp](https://github.com/ExpansionPak/DolRecomp).
- `runtime/` — project runtime, device models, renderer, and ROM-free tests.
- `tools/ipl_descramble/` — IPL descrambler and tests.
- `oracle/` — scripts and documentation for differential comparison with a
  separately downloaded Dolphin checkout.
- `bios/` — instructions for user-supplied firmware; firmware is ignored by Git.

See [PRINCIPLES.md](PRINCIPLES.md) for the validation discipline and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for component provenance.

## Build and test without firmware

The tested host setup is 64-bit Windows on an AVX2-capable CPU, with MSYS2
MinGW64 GCC, CMake, and Ninja. The rendered runtime defaults to the fixed
`x86-64-v3` baseline; use `-DGCN_X86_64_V3=OFF` for an older CPU (the portable
build is correct but may not sustain real-time IPL rendering). The recompiler
is C11; the runtime also builds a C++20 DSP component. Vulkan is optional and
is detected automatically.

From an MSYS2 shell:

```bash
./build.sh
ctest --test-dir recompiler/build --output-on-failure
ctest --test-dir runtime/build --output-on-failure
ctest --test-dir tools/ipl_descramble/build --output-on-failure
```

These default builds and tests do not require Nintendo firmware, games, or save
files. Set `GCN_MELEE_GCS` to a save file you are authorized to use if you want
to run the optional real-container memory-card import check.

## Building the IPL runtime

To produce `gcn_boot`, place your own GameCube IPL dump at `bios/ipl.bin`, then:

```bash
./build.sh
./runtime/generate.sh
cmake -S runtime -B runtime/build-boot -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DGCN_WITH_GENERATED=ON
cmake --build runtime/build-boot
```

The full LLE boot additionally uses user-supplied DSP IROM and coefficient
dumps as described in [bios/README.md](bios/README.md). The commands above
produce the baseline BS2 build; the true-reset BS1 merge is the staged,
advanced flow documented in `runtime/generate_bs1.sh`. Runtime switches and
their exact semantics are documented at the top of `runtime/src/boot.c`.

Generated IPL C source, firmware dumps, DSP ROMs, disc images, memory-card
images, save files, captures, and build outputs are intentionally excluded from
the repository.

## Recompiler fork

General DolRecomp users should use the official ExpansionPak repository. The
`mstan/DolRecomp` fork exists to preserve this project's reviewable integration
changes and make potentially useful patches easy to inspect or upstream. Its
`gcnrecomp` branch is pinned in [recompiler/UPSTREAM.md](recompiler/UPSTREAM.md).

## License and trademark notice

The combined source is distributed under **GPL-3.0**; incorporated
GPL-2.0-or-later components are distributed under compatible GPLv3 terms. See
[LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

This is an independent research project. It is not affiliated with, endorsed
by, or sponsored by Nintendo. GameCube is a trademark of Nintendo. No Nintendo
firmware, games, keys, or copyrighted artwork are included.
