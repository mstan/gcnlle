# gcnrecomp Roadmap

Goal: a PSXRecomp-style **LLE-first** GameCube experience — statically
recompile the real **IPL** and boot into the console's own menu: rolling-cube
logo → main menu → memory-card manager → date/time & sound/screen options →
disc-load screen (stubbed until we build the disc phase).

The engine is a fork of **ExpansionPak/DolRecomp** (PPC→C static recompiler,
GPL-3.0) retargeted from game DOLs to the IPL. The runtime is net-new LLE.
The oracle is **Dolphin** (boots the real IPL as "GameCube Main Menu"),
kept independent of our code per PRINCIPLES.md.

## Why LLE / why the IPL

Every existing GameCube recomp (gcrecomp, reshine, GXRuntime, NWiiRecomp)
HLEs the boot — it fakes the post-BS2 low-memory state and jumps straight
into a game DOL, or reimplements the SDK (aurora). None runs the IPL. The
IPL *is* the "BIOS thing" we want, so we recompile it and model the hardware
underneath it. This is the foundation; enhancements come later (ENHANCEMENTS
phase), never instead of it.

## Milestones

**M0 — IPL ingest & descramble.** Point the recompiler's slice-walker at
`bios/ipl.bin`. Handle the BS1 descrambler: either recompile BS1 so it
descrambles BS2 in-CPU (most faithful), or descramble offline and recompile
the plaintext body. Deliverable: recompiler emits C for the IPL entry region;
disassembly cross-checks against `dtk`.

**M1 — CPU core boots under the runtime.** Wire `runtime/` (MEM1, cpu_glue,
dispatch) to the recompiled entry. Bring up `oracle/` Dolphin trace frontend.
Deliverable: recompiled IPL executes early boot; **lockstep vs Dolphin** on
PC + register + low-mem-write order (order+state+caller diffing, not frame
alignment).

**M2 — VI + GX → rolling-cube logo.** Model Video Interface scanout (XFB →
host window) and enough of the Flipper GX FIFO to render the boot animation.
Harvest reshine's GX-FIFO→GL path as reference. Deliverable: **screenshot of
the GameCube logo** (screenshot-before-asserting, per PRINCIPLES).

**M3 — EXI: RTC + SRAM → date/time & settings.** Model the EXI RTC (real-time
clock) and SRAM (persisted settings). Deliverable: the menu shows/sets **date
& time** and sound/screen/language options, persisted across runs.

**M4 — Memory-card manager.** Model EXI memory-card devices; back them with
host `.raw`/`.gci` files (Dolphin-compatible). Deliverable: **navigate,
view, copy, delete** save blocks in the IPL's card manager.

**M5 — Disc-load screen (stub).** Model DI enough to reach the "insert disc"
screen. No game loading yet — that's a later phase. Deliverable: menu →
disc screen transition.

## Independent-oracle discipline

Dolphin is a separately-authored emulator, so it can arbitrate shared-layer
(bus/timing/EXI) bugs our own code can't self-check. Build a headless trace
frontend mirroring psxrecomp's `beetle_libretro` pattern: same JSONL RAM/PC
trace shape both sides emit, diffed by value+order. Reproduce by driving the
menu, not by aligning savestates.

## Asset used from each investigated repo

- **DolRecomp** (GPL-3.0) — forked as `recompiler/`. The engine.
- **reshine** (GPL-3.0) — runtime architecture + GC recomp docs as reference
  (flat memory, MMIO intercept, per-frame dispatch, exception/rfi handling).
- **GXRuntime** (GPL-3.0) — C device models to harvest: EXI(RTC/SRAM),
  memory_card, VI, DI, SI, ARAM, mmio_bus.
- **decomp-toolkit / dtk** (MIT/Apache) — external CLI for DOL/REL/disc
  parsing & disassembly cross-checks (`tools/`).
- **aurora** (MIT) — reference only for CARD/GX/VI reimplementations;
  paradigm mismatch (SDK-entry HLE), not linked.
- **gcrecomp** (MIT) — reference for hardware register maps (`gc_hw.h`,
  `os_defs.h`); its HLE OS layer is not used.
- **GameCubeRecompiled, NWiiRecomp** — discarded (unproven / non-free).
