# gcnrecomp — Design Document

*Self-contained. Written to be read cold (e.g. pasted into another LLM for
discussion). Last updated 2026-07-09.*

---

## 1. Goal

Build a **static recompiler for the Nintendo GameCube**, in the same mold as
our existing PlayStation project (`psxrecomp`). The near-term goal is **not to
run games**. It is to statically recompile the GameCube **IPL** — the console's
boot ROM / "BIOS" — and boot into the console's own menu:

- the animated **rolling-cube "Nintendo GameCube" logo** and startup sound,
- the **main menu**,
- the **memory-card manager** (view / copy / delete saves on Slot A & B),
- the **settings**: **date & time**, sound (stereo/mono), screen position,
  language, progressive scan,
- the **disc-load screen** (reached, but not yet loading a game).

This is exactly what `psxrecomp` does for the PlayStation BIOS (boot the real
`SCPH1001.BIN`, navigate the memory-card manager, set the clock). We want the
GameCube equivalent as a fluid, faithful BIOS experience first; running actual
game discs is a later phase.

### Guiding principle: LLE-first

The project follows a strict **LLE-first** discipline (see `PRINCIPLES.md`):

- **LLE (Low-Level Emulation) is the baseline.** We recompile the *real*
  firmware and model the *real* hardware underneath it (raw Flipper/EXI/DI/VI
  registers), rather than reimplementing the SDK.
- **HLE (High-Level Emulation) is only ever a validated, per-subsystem
  replacement** on top of a proven LLE baseline — never the baseline itself,
  and never "fake the answer so a milestone looks done."
- The **oracle** that judges correctness must be a **separately-authored
  emulator** (Dolphin), so it shares none of our device models and can catch
  bugs in our shared layers that our own code cannot self-check.

This principle is *why* the project exists in this shape: as the survey below
shows, every existing GameCube recompiler is HLE-first, and none of them can
give us the IPL menu.

---

## 2. Hardware background (what we must model)

References: **YAGCD** (Yet Another GameCube Documentation) and the **Dolphin**
emulator source are the authoritative sources.

- **CPU — "Gekko":** an IBM PowerPC 750CXe derivative @ 485 MHz. Adds
  **paired-single** floating point (SIMD over pairs of 32-bit floats:
  `ps_*` ops and `psq_l/psq_st` quantized load/stores) and locked-cache /
  extra SPRs. 32 KB L1 I/D, 256 KB L2. **Big-endian.** This is the ISA the
  recompiler must decode.
- **Memory:**
  - **MEM1** — 24 MB main RAM at `0x80000000` (cached) / `0xC0000000`
    (uncached mirror). The IPL and everything else lives here.
  - **ARAM** — 16 MB auxiliary *audio* RAM, **not CPU-addressable**; reached
    only via DMA through the DSP.
  - **EFB** — 2 MB embedded framebuffer inside the GPU.
  - Low-memory OS globals live at `0x80000000..0x80003100` (boot magic, arena
    hi/lo, bus/CPU clock, video mode, etc.).
- **GPU — "Flipper":** @ 162 MHz. A **fixed-function TEV** (Texture
  Environment) pipeline — *not* programmable shaders. A **Command Processor**
  reads a **GX FIFO** (a display list of GX commands) from main memory and
  feeds the Transform/Setup/Rasterizer units. Rendering targets the EFB, which
  is copied out to an **XFB** (external framebuffer) in MEM1 that the Video
  Interface scans out.
- **MMIO map** (Flipper registers, `0xCC00xxxx`): CP `0xCC000000`, PE
  (pixel engine), **VI `0xCC002000`** (video), PI `0xCC003000` (processor
  interface / interrupts), MI `0xCC004000` (memory interface), **DSP/AI-DMA
  `0xCC005000`**, **DI `0xCC006000`** (disc), **SI `0xCC006400`** (controllers),
  **EXI `0xCC006800`** (expansion), **AI `0xCC006C00`** (audio interface).
- **EXI (External Interface):** a serial bus with 3 channels. Hangs off it:
  - **RTC + SRAM** (channel 0): the **real-time clock** (date/time) and the
    small **SRAM** that persists settings (language, sound mode, screen
    offset, progressive flag, plus the RTC bias). *This is where "set date/time"
    and the settings menu read/write.*
  - **Memory cards** (Slot A on EXI ch0, Slot B on EXI ch1): the card manager
    reads/writes these.
  - The **IPL mask ROM itself** is also an EXI device.
- **DI (Disc Interface):** DMA reads from the disc. Needed only to reach the
  disc-load screen for now.
- **SI (Serial Interface):** up to 4 controllers (menu navigation input).
- **DSP + AI:** audio. The **startup sound** and menu sounds run through here.

### The IPL and the boot flow

The **IPL** is a ~2 MB mask ROM containing: **BS1** (boot stage 1, runs first
from ROM), **BS2** (boot stage 2), the **apploader** reader, the **boot
animation + sound**, **fonts** (ANSI + Shift-JIS), and the **menu** (main
menu, card manager, settings/clock). Boot flow:

1. Power on → CPU begins executing at the PowerPC reset vector in the IPL mask
   ROM (`0xFFF00100`, high exception prefix).
2. **BS1** runs from ROM: minimal setup, then **descrambles and hash-verifies
   BS2** into MEM1 and jumps to it.
3. **BS2** initializes hardware (VI, EXI, memory), plays the intro animation +
   sound, then: **no disc → show the menu**; disc present → read the apploader
   and boot the game.

**Critical wrinkle — the IPL is stored scrambled.** The IPL image is obfuscated
with a byte-wise stream cipher; BS1 descrambles it at boot. The descrambler
algorithm is public (Dolphin, Swiss, and others implement it). This is
Milestone 0's main task (see §6).

*Note:* the IPL is copyrighted Nintendo firmware and is region/revision-specific
(the animation and menu differ by region). The user supplies their own dump, as
they already do with `SCPH1001.BIN` for `psxrecomp`. It is never committed.

---

## 3. The ecosystem survey (what already exists)

We surface-investigated eight existing GameCube/Wii projects to decide what to
reuse. **Headline finding: none does LLE, and none targets the IPL.** Every one
that "boots" does so by *faking* the post-BS2 low-memory state and jumping
straight into a **game DOL**, or by reimplementing the SDK. That is precisely
the HLE-first shape our principles forbid as a foundation — but it also means
the gap (the IPL menu) is entirely in the *runtime* layer we write ourselves,
not in the CPU engine we can reuse.

| Project | What it is | LLE/HLE | License | Verdict |
|---|---|---|---|---|
| **ExpansionPak/DolRecomp** | PPC→C static recompiler engine, C11, ~236 opcodes **incl. full paired-singles**, host-intercept dispatch, "bring your own runtime" | CPU-only, OS-agnostic | **GPL-3.0** | **FORK — the engine.** Proven to boot titles (Luigi's Mansion title screen). Both real GC ports build on it. |
| **vinvirile/reshine** | Super Mario Sunshine (J) port = DolRecomp + hand-written runtime (flat memory, MMIO intercept, per-frame dispatch, GX-FIFO→OpenGL). Renders the NINTENDO logo. Strong docs. | LLE-CPU + HLE peripherals; boots game DOL | GPL-3.0 | **Reference** — best architectural template for the runtime. |
| **aharonahdoot/GXRuntime** ("DolRuntime") | Runtime for DolRecomp running Mario Strikers. C device models: **EXI (RTC/SRAM), memory_card**, VI, DI, SI, ARAM, mmio_bus + `aurora` graphics. | HLE boot (fakes IPL result) | GPL-3.0 | **Harvest device models** (esp. EXI-RTC + memory card). |
| **sp00nznet/gcrecomp** | Own PPC→C recompiler + D3D11 GX/TEV + hardware headers (`gc_hw.h`, `os_defs.h`). Most complete *on paper*. | HLE OS, Windows/D3D-locked | **MIT** | **Reference** for hardware register maps. No evidence a title boots. |
| **encounter/decomp-toolkit** (`dtk`) | The de-facto GC/Wii binary CLI (Rust): DOL/REL/disc parse, disasm, split, compression. | tooling (runs nothing) | MIT/Apache | **Tool** — external, for disasm cross-checks & disc work. |
| **encounter/aurora** | High-quality **SDK-level** HLE runtime (GX-on-WebGPU, CARD, VI, PAD). Powers shipped decomp source ports. | HLE (intercepts `GXInit` etc.) | MIT | **Reference only** — paradigm mismatch (SDK-entry HLE + shader GX). |
| KaiserGranatapfel/GameCubeRecompiled | AI-generated Rust; empty output artifacts; self-contradictory license | HLE | CC0(+EULA?) | **Discard.** |
| BlackLineInteractive/NWiiRecomp | Wii/Wii U-diluted; non-free custom "commercial-restricted" license | HLE | non-free | **Discard.** |

**Dependency reality:** `reshine → DolRecomp` (hard dep) and
`GXRuntime → DolRecomp + aurora`. **DolRecomp is the hub every booting
GameCube port sits on.** gcrecomp and GameCubeRecompiled are independent
islands nothing consumes.

---

## 4. Decision

- **Fork DolRecomp** as the recompiler engine. Rationale: it is the proven,
  load-bearing PPC→C engine (both real ports use it), it is clean C11 that
  matches our C-based ecosystem, it has full paired-singles, and — crucially —
  it is **OS-agnostic / CPU-only**, so there is no baked-in HLE to fight when
  we point it at firmware instead of a game.
- **Accept GPL-3.0.** DolRecomp, reshine, and GXRuntime are all GPL-3.0; the
  only permissive engine (gcrecomp) has no evidence of booting anything. We
  take the proven-but-copyleft path; the whole project is therefore GPL-3.0.
  (The permissive alternative — a clean-room PPC front-end — was considered and
  rejected as more upfront work for no functional gain here.)
- **Write our own LLE runtime** pointed at the **IPL** (not a game DOL). This
  is the net-new work and the whole reason the project exists.
- **Dolphin is the oracle** (it boots the real IPL as "GameCube Main Menu").

---

## 5. Architecture (mirrors `psxrecomp/`)

```
gcnrecomp/
  recompiler/   Fork of DolRecomp (PPC→C). Modules: dr_frontend (decode +
                DOL/REL/RPX containers), dr_analysis (CFG / jump tables),
                dr_backend (split-C emit), dr_cpu, dr_platform, dr_app.
                Retarget its slice-walker from game DOLs at the IPL image.
                Vendored @ commit f3a129d (see recompiler/UPSTREAM.md).
  bios/         Your own ipl.bin dump (gitignored — copyrighted firmware).
  runtime/      NET-NEW LLE host. Loads/descrambles the IPL, runs the
                recompiled firmware, models the hardware it touches:
                  memory.c    flat big-endian MEM1 (24MB) + bus primitives
                  cpu_glue.c  PPCContext entry/exit, exceptions, rfi
                  dispatch.c  block-dispatch loop + host-intercept table
                  exi.c       EXI: RTC (date/time) + SRAM (settings) + cards
                  vi.c        Video Interface: XFB scanout → host window
                  gx.c        Flipper GX FIFO command processor → renderer
                  dsp.c/ai.c  audio (startup chime, menu sound)
                  di.c        Disc Interface (stub until disc phase)
                  si.c        Serial Interface (controller input)
                  debug_server.c  TCP debug surface + ALWAYS-ON ring buffers
                                  (MMIO/PC/event/device-write rings),
                                  screenshot, state queries, input injection —
                                  cross-cutting, must exist before M2
                                  (mirror psxrecomp debug_server.c/TCP_COMMANDS.md)
  oracle/       Dolphin trace frontend (mirror psxrecomp's beetle_libretro):
                boot the real IPL, log PC/registers/low-mem writes as JSONL in
                the same shape the runtime emits; diff by value + order.
  tools/        dtk (decomp-toolkit) for disasm cross-checks & disc work.
  docs/         ROADMAP.md (terse), DESIGN.md (this file).
  PRINCIPLES.md CLAUDE.md README.md build.sh LICENSE(GPL-3.0)
```

We **harvest** GXRuntime's C device models (EXI-RTC/SRAM, memory_card, VI, DI,
SI — all GPL-3.0), use **reshine** as the runtime-architecture reference
(especially its GX-FIFO→GL path and its `docs/`), and consult **gcrecomp**'s
`gc_hw.h` for the register map.

**The runtime must satisfy the IPL's *embedded* SDK.** The IPL/BS2 is not
hand-written bare-metal glue — it is a DOL-like C program **linked against
early Nintendo SDK code**, and a large fraction of the payload is ordinary SDK
library routines (`OSInit`, `DVDInit`, `CARDInit`, `VIInit`, `PADInit`, …).
This is *good* for the LLE stance: those routines are **guest code we
recompile and run**, not host APIs we HLE. But it sets the bar for "minimal
runtime" — the device models must satisfy what those embedded SDK routines
expect from VI/GX/EXI/DSP/AI/ARAM, not merely what obviously-hand-written IPL
logic touches. (External design review, 2026-07-09.)

**Two operating disciplines carried out of that review:**
- **Observe before implementing GX.** Do not pre-guess the GX subset. Stand up
  a **GX FIFO recorder** (an always-on ring of parsed CP/BP/XF packets, in the
  ring-buffer spirit of our PRINCIPLES) and implement only the commands the IPL
  actually emits. No "2D blitter" shortcut — the menu is a real 3D glass-cube
  scene (transforms, textures, alpha, EFB→XFB copy).
- **DSP as a silence shim first.** Start audio as a DSP/AI/ARAM shim that
  accepts commands and advances mailboxes/interrupts but outputs silence; only
  promote DSP to first-class if the menu actually blocks on DSP-side
  completion (decided empirically from the trace, not guessed).

---

## 6. Milestones

> **Design-review revision (2026-07-09).** The original plan made
> "IPL ingest + descramble" M0. The review reordered this: **defer BS1 and the
> in-CPU descrambler to M1**, and make M0 a *declarative seed contract* that
> boots offline-descrambled BS2 from the real IPL start. This gets the menu
> proving the runtime without BS1/descramble edge cases eating the first month
> — and it sidesteps a DolRecomp limitation for free: DolRecomp does not handle
> self-modifying code, and the bootrom's cache/DMA **descramble is exactly
> that**, so deferring BS1 defers the SMC problem too.

**M0 — Declarative seed contract → BS2 boots to first divergence.** Construct a
seed with a *provenance for every byte* — no lifting a Dolphin RAM snapshot as
input (that would make the oracle a data source, not a checker). Specifically:
- plaintext BS2/IPL, obtained by an **offline descramble of the real IPL ROM**,
  loaded at its real target address;
- **entry at the true IPL start routine** (which runs
  `BS2Init → OSInit → DVDInit → CARDInit → CheckSram → VIInit → PADInit →
  BS2Main`) — *not* at an already-initialized inner menu function. `BS2Init`
  clears low memory and sets physical-memory-size / console-type itself, so
  almost nothing in MEM1 needs seeding;
- MEM1 = deterministic reset pattern except the loaded payload;
- BAT/MMU/MSR/HID CPU latches reconstructed from **BS1 disassembly + docs**
  (the only genuinely Dolphin-adjacent unknowns; Dolphin *verifies* them, never
  provides them — any latch we can't derive is a **flagged debt**, not a silent
  snapshot);
- EXI ROM / SRAM / RTC backed by **explicit fixtures** (valid SRAM checksum,
  fixed RTC epoch), no disc or a fixed dummy disc.

*Oracle:* the Dolphin trace verifies the first low-mem writes, MMIO writes, EXI
traffic, interrupt order, and GX FIFO stream. *Deliverable:* recompiled BS2
executes from the seed and **locksteps vs Dolphin** (order + state + caller) up
to a documented first divergence.

**M1 — Real BS1 + in-CPU descrambler.** Recompile/run BS1 (starts at
`0xFFF00100`, prepares hardware, descrambles/copies BS2 via EXI ROM reads) so
the boot is faithful end-to-end. This is where the **self-modifying-code /
cache/DMA descramble** work lives — tackled only *after* the menu is alive, so
it can't block early progress. *Deliverable:* boot from real scrambled ROM with
no offline pre-descramble, matching Dolphin from reset.

**Observability (cross-cutting; stand up before M2).** A runtime **TCP debug
server + always-on ring buffers** (MMIO/PC/event/device-write), plus
`screenshot`, state queries, and input injection — mirroring psxrecomp's
`debug_server.c`/`TCP_COMMANDS.md`. Not a phase that "finishes": it is standing
infrastructure every later milestone leans on, and it MUST exist before M2
because "reached the logo / at the card manager" claims have to come from
captured pixels (PRINCIPLES: screenshot-before-asserting; always-on rings;
never arm-then-time). The `GCN_TRACE_OUT` file trace stays for the deterministic
first-boot lockstep; rings+TCP carry rendering and the interactive menus.

**M2 — VI + GX FIFO recorder → rolling-cube logo.** Model VI scanout (XFB →
host window). Build the **GX FIFO recorder first**, inventory the exact CP/BP/XF
packets + primitives + texture formats the IPL emits, then implement only that
subset (harvest reshine's GX-FIFO→GL path). *Deliverable:* a **screenshot of
the GameCube logo** (captured via the debug server above).

**M3 — EXI: RTC + SRAM → date/time & settings.** Model the EXI RTC and SRAM.
*Deliverable:* the menu shows and **sets date & time** and sound/screen/
language options, persisted across runs.

**M4 — Memory-card manager.** Model EXI memory-card devices, backed by host
`.raw`/`.gci` files (Dolphin-compatible). *Deliverable:* **navigate, view,
copy, delete** saves in the IPL's card manager.

**M5 — Disc-load screen (stub).** Model DI enough to reach the "insert disc"
screen. No game loading. *Deliverable:* menu → disc-screen transition.

*(Running actual game discs is a later phase, out of scope here.)*

---

## 7. Open technical questions / risks

**Resolved by the 2026-07-09 design review:**

- ~~Descramble altitude.~~ **Settled:** offline-descramble for M0, real in-CPU
  BS1 descramble deferred to M1 (§6). Keeps the LLE line without letting the
  bootrom eat the project first.
- ~~Dolphin trace mechanics.~~ **Settled:** a small **trace-tap patch to a
  local Dolphin build** (force CPU interpreter + deterministic RTC/SRAM/memcard;
  emit a tiered trace: event ledger → full-register divergence window →
  memory-watch expansion). Not the GUI debugger, movie/`--batch`, or a libretro
  core — those are either not automatable or below the instruction/MMIO
  granularity we need. A ~30-line local `--boot-gc-ipl` flag beats abusing the
  game/disc-oriented stock CLI.

**Standing discipline (from the review, matches PRINCIPLES.md):** the oracle
must stay *independent of the code under test*. Use Dolphin traces for **event
order + state targets only**; take *implementation* from YAGCD/hardware docs and
our own observations. Never transplant Dolphin implementation code into the
runtime (it also protects the GPL boundary — keep the trace-tap patch local).

**Still genuinely open — to be answered *empirically from the trace*, not by
speculation:**

1. **PE completion waits (M2).** Does the menu **busy-wait on PE draw-done /
   finish tokens**, or fire-and-forget the FIFO? Decides whether M2 needs real
   PE completion *timing* or can ack instantly. → Shows up as a spin on a GX/PE
   MMIO read in the trace.
2. **Memory-card UI prerequisites (M4).** Is card presence/format/dir/BAM
   enough to *navigate* the card manager, or does that UI first gate on the RTC
   and on **IPL-ROM font data being decoded correctly** (i.e. is font/EXI-ROM
   fidelity a prerequisite to even reaching a legible card screen)? → Shows up
   as ordering in the EXI / low-mem stream.
3. **Timing model.** How much cycle/timing fidelity does the menu require? Menu
   is largely event-driven (VI retrace, input polling) so possibly little — but
   the boot animation and audio may be timing-sensitive.
4. **Audio / DSP scope (M2/M3).** What produces the startup chime and menu
   sounds — IPL DSP ucode on ARAM (the `gc-ipl` repo carrying a *Jaudio*
   decompile dir is a warning sign it isn't a single AI-DMA beep) or simpler AI
   streaming? Silence-shim first; promote only if the menu blocks on it.
5. **EXI RTC/SRAM specifics (M3).** Exact RTC command protocol and SRAM layout
   (settings fields, checksum, RTC bias). GXRuntime models this — verify its
   fidelity against Dolphin.
6. **Region/revision coverage.** Which IPL revision first, and how much does the
   menu differ across NTSC-U / NTSC-J / PAL?

---

## 8. Current status (as of this writing)

- Repo scaffolded and committed (`git`, GPL-3.0), private at
  `github.com/mstan/gcnrecompiled`.
- `recompiler/` = DolRecomp fork, **builds green, 10/10 tests pass**.
- `runtime/` = LLE skeleton, compiles a placeholder (no IPL yet).
- `bios/`, `oracle/`, `tools/`, `docs/` structured with READMEs/plans.
- `./build.sh` builds the tree (handles the mingw64 PATH quirk).
- Plan pressure-tested in an **external design review (2026-07-09)**; M0/M1
  reordered to the declarative-seed-contract form above (§6), GX-recorder and
  DSP-silence-shim disciplines adopted (§5), descramble + trace mechanics
  settled (§7).
- **Nothing boots the IPL yet.** M0 (seed contract + Dolphin trace-tap) is the
  next work; it is gated on a user-supplied `bios/ipl.bin` for the actual
  recompile, but the trace-tap, offline descrambler, and seed-contract format
  can be built ahead of that.
