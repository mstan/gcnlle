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

*(Reordered per the 2026-07-09 design review — see DESIGN.md §6 for the full
rationale. BS1/descramble deferred to M1; M0 is now a declarative seed
contract. This also defers DolRecomp's self-modifying-code gap, since the
bootrom descramble IS the SMC.)*

**M0 — Declarative seed contract → BS2 boots to first divergence.** Boot
offline-descrambled BS2 (from the real ROM) at the **true IPL start routine**
(lets `BS2Init`/`OSInit`/… initialize low-mem itself). Every seed byte has a
provenance — ROM / BS1-disasm / fixture — and **no Dolphin RAM snapshot is
lifted as input** (oracle stays a checker, not a data source). Wire
`runtime/` (MEM1, cpu_glue, dispatch) and the Dolphin trace-tap. Deliverable:
recompiled BS2 **locksteps vs Dolphin** on PC + register + low-mem/MMIO/EXI/GX
write order (order+state+caller, not frame alignment) to a documented first
divergence.

**M1 — Real BS1 + in-CPU descrambler.** Recompile/run BS1 (`0xFFF00100`) so it
descrambles/copies BS2 via EXI ROM reads — the faithful end-to-end boot. This
is where the self-modifying cache/DMA descramble work lives; done after the
menu is alive so it can't block early progress. Deliverable: boot from the real
scrambled ROM with no offline pre-descramble, matching Dolphin from reset.

**M2 — VI + GX FIFO recorder → rolling-cube logo.** Model VI scanout (XFB →
host window). Build a **GX FIFO recorder first**, inventory the exact CP/BP/XF
packets the IPL emits, then implement only that subset (harvest reshine's
GX-FIFO→GL path). Deliverable: **screenshot of the GameCube logo**
(screenshot-before-asserting, per PRINCIPLES).

**M3 — EXI: RTC + SRAM → date/time & settings.** Model the EXI RTC (real-time
clock) and SRAM (persisted settings). Deliverable: the menu shows/sets **date
& time** and sound/screen/language options, persisted across runs.

**M4 — Memory-card manager.** Model EXI memory-card devices; back them with
host `.raw`/`.gci` files (Dolphin-compatible). Deliverable: **navigate,
view, copy, delete** save blocks in the IPL's card manager.

**M5 — Disc-load screen (stub).** Model DI enough to reach the "insert disc"
screen. No game loading yet — that's a later phase. Deliverable: menu →
disc screen transition.

## Observability (cross-cutting — must be in place before M2)

This is **not a phase you finish**; it is always-on infrastructure every
milestone leans on, so it is built as a standing surface, not bolted on late
(PRINCIPLES: "extend the structured debug surface", "always-on ring buffers",
"screenshot before asserting visible state"). It must exist **before M2**,
because the moment we render, claims about visible state have to be made from
captured pixels, not from memory.

Mirror psxrecomp's `debug_server.c` + `TCP_COMMANDS.md`. The runtime exposes a
**TCP debug server** with:

- **Always-on ring buffers**, recording continuously from runtime start (Release
  too; bounded by eviction): an MMIO ring, a retired-PC/block ring, an
  event ring (interrupts/EXI/DMA/GX/VI), and a device-write ring. Probes
  **query** the ring for a window — they never arm-then-run-then-hope
  (the arm-and-time anti-pattern is banned).
- **`screenshot` / `screenshot_file`** — capture the framebuffer through the
  debug surface (never by stealing focus), so every "reached the logo / at the
  card manager / black screen" claim is made from real pixels.
- **State queries** — CPU regs, low-mem globals, SRAM/RTC, device state.
- **Input injection** — drive the menu (D-pad/A/B/START) deterministically, so
  interactive milestones (M3 date/time, M4 card manager) are scriptable and
  re-runnable without a human at the pad.

The current file-based oracle trace (`GCN_TRACE_OUT`) stays for the deterministic
first-boot lockstep; the ring/TCP surface is what carries us through rendering
and the interactive menus.

## Independent-oracle discipline

Dolphin is a separately-authored emulator, so it can arbitrate shared-layer
(bus/timing/EXI) bugs our own code can't self-check. Unlike our libretro-core
oracle pattern (snesref/beetle), Dolphin gets a **small trace-tap patch to a
local build** — instruction/MMIO-granular traces are below what a libretro core
exposes, and Dolphin's core is unmaintained. Force the CPU interpreter +
deterministic RTC/SRAM/memcard; emit the same trace shape both sides produce,
diffed by value+order. Use Dolphin only for **event order + state targets**;
take *implementation* from YAGCD/hardware docs (keep the oracle independent, and
keep the GPL patch local). Reproduce by driving the menu, not by aligning
savestates.

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
