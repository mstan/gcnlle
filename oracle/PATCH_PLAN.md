# PATCH_PLAN.md — a LOCAL Dolphin trace-tap for the gcnrecomp oracle

> **This is a plan, not code.** It describes a *local, uncommitted patch* to a
> developer's own Dolphin checkout that makes Dolphin emit a
> `oracle/trace_format.h` stream while it boots the real GameCube IPL as
> "GameCube Main Menu". It requires a local Dolphin build.
>
> **Do NOT transplant Dolphin code into gcnrecomp, and do NOT commit this patch
> into gcnrecomp.** Two reasons, both load-bearing:
> 1. **Independence** (PRINCIPLES.md "The Differential Oracle Must Be Independent
>    of the Code Under Test"): the oracle only stays a valid arbiter of our
>    *shared-layer* bugs if it shares none of our device/bus/timing models.
>    Take *event order + state targets* from the trace; take *implementation*
>    from YAGCD / hardware docs / our own observation — never from Dolphin.
> 2. **License**: Dolphin is GPL-2.0-or-later; keeping the tap as a patch to a
>    separate Dolphin tree avoids entangling our GPL-3.0 source with it.
>
> The tap forces the **CPU interpreter** (not JIT) and deterministic
> RTC/SRAM/memcard so the two streams are comparable by value + order.

## 0. Get a local Dolphin and pin it

```
git clone https://github.com/dolphin-emu/dolphin.git
cd dolphin
git submodule update --init
# Pin a commit and record it here so the cited paths/line-context stay valid:
#   DOLPHIN_PIN = <fill in the exact commit SHA you build against>
```

All source paths below are **relative to the Dolphin repo root** and were
verified to exist on `master` (July 2026) via the GitHub API/raw content — see
"Verification status" at the end. **Line numbers are intentionally omitted**;
they drift. Hook by *function/class name*, which was verified; confirm each
against your pinned checkout before editing.

---

## (a) Retired-instruction trace — in the CPU INTERPRETER

**File:** `Source/Core/Core/PowerPC/Interpreter/Interpreter.cpp`
**Class:** `Interpreter`

- Per-instruction execution is `int Interpreter::SingleStepInner()`; the outer
  loop is `void Interpreter::Run()` and `void Interpreter::SingleStep()`.
  `SingleStepInner()` sets `m_ppc_state.npc = m_ppc_state.pc + sizeof(UGeckoInstruction)`
  and `UpdatePC()` commits `pc = npc`. **Hook: at the top of `SingleStepInner()`
  (or immediately after `UpdatePC()`), emit one `GCN_TR_RETIRED` record.**
- Dolphin already has a matching hook to model: the `Interpreter::Trace(inst)`
  method, gated by `m_start_trace`, which `DEBUG_LOG_FMT`s PC + registers. Reuse
  that call site; replace the log with a binary-record write.

**Register sources** — the CPU state is `m_ppc_state`
(`PowerPC::PowerPCState`, declared in `Source/Core/Core/PowerPC/PowerPC.h`):

| trace_format field | Dolphin source |
|---|---|
| `pc`, `npc` | `m_ppc_state.pc`, `m_ppc_state.npc` |
| `gpr[0..31]` | `m_ppc_state.gpr[i]` |
| `lr`, `ctr` | `LR(m_ppc_state)` = `spr[SPR_LR]`, `CTR(m_ppc_state)` = `spr[SPR_CTR]` |
| `cr` | `m_ppc_state.cr.Get()` (ConditionRegister → packed u32) |
| `xer` | `PowerPC::GetXER(m_ppc_state).Hex` (reconstructed from `xer_ca`/`xer_so_ov`/`xer_stringctrl`) |
| `msr` | `m_ppc_state.msr.Hex` |
| `fpscr` | `m_ppc_state.fpscr.Hex` (set `GCN_TR_F_FP_VALID`) |
| `ps_hash` | hash over `m_ppc_state.ps[0..31]` (both paired-single lanes) |
| `insn` | the fetched opcode word for this step |

**Force the interpreter** so this path actually runs (see §e): config
`Core.CPUCore = 0` (`PowerPC::CPUCore::Interpreter`; enum verified in
`PowerPC.h`: `Interpreter=0, JIT64=1, JITARM64=4, CachedInterpreter=5`).

> Note the enum discriminant: `Interpreter = 0`. `CachedInterpreter = 5` does
> **not** run `SingleStepInner()` and must not be selected for the tap.

---

## (b) MMIO reads/writes

**File:** `Source/Core/Core/HW/MMIO.h` (+ `MMIO.cpp`)
**Class:** `MMIO::Mapping`

- Every MMIO access funnels through the templated
  `Unit Mapping::Read<Unit>(Core::System&, u32 addr)` and
  `void Mapping::Write<Unit>(Core::System&, u32 addr, Unit val)` before they
  dispatch to `GetHandlerForRead/Write`. **Hook both**: emit a `GCN_TR_MMIO`
  record (`is_write`, `size = sizeof(Unit)`, `addr`, `val`, current PC).
- Address decode: `MMIO::IsMMIOAddress()` covers the GameCube block
  `0x0C00xxxx`. The per-block id for `GCN_TR_MMIO.block` can be derived from the
  address (`0xCC000000` CP, `…2000` VI, `…3000` PI, `…5000` DSP, `…6800` EXI,
  `…6C00` AI, etc. — the map in DESIGN.md §2).
- **GX FIFO writes do NOT go through MMIO** — the gather-pipe write path is
  explicitly excluded from `IsMMIOAddress` (`GATHER_PIPE_PHYSICAL_ADDRESS`).
  Trace the FIFO separately (see §f).

Current PC for MMIO/mem records: read `system.GetPPCState().pc` (or thread the
interpreter's `m_ppc_state.pc`).

**Memory writes** (`GCN_TR_MEM`, MEM1 stores): hook the write helpers in
`Source/Core/Core/HW/Memmap.cpp` / `Source/Core/Core/PowerPC/MMU.cpp`
(`MMU::Write_U8/U16/U32/U64`), filtered to the MEM1 range so the low-mem OS
globals `0x80000000..0x80003100` are captured. (MMU.cpp verified present via the
Interpreter's memory path; confirm the exact `Write_*` names on your checkout.)

---

## (c) EXI traffic (RTC / SRAM / memory card / IPL-ROM)

**Files (all under `Source/Core/Core/HW/EXI/`, directory verified):**
- `EXI_Channel.cpp` / `EXI_Channel.h` — `CEXIChannel` (per-channel CS + xfer)
- `EXI_Device.cpp` / `EXI_Device.h` — `IEXIDevice::TransferByte(u8&)`, the base
  byte-transfer hook shared by every device
- `EXI_DeviceIPL.cpp` — `CEXIIPL`: **RTC + SRAM** live here
- `EXI_DeviceMemoryCard.cpp` — `CEXIMemoryCard`: the card manager's device

**Hook:** wrap `IEXIDevice::TransferByte(u8& data)` (and/or
`CEXIChannel` CS assert/deassert) to accumulate a per-transaction command +
response byte stream, then emit one `GCN_TR_EXI` record on CS-deassert
(`select → 0`): fill `channel`, `device`, `cmd_len`/`resp_len`,
`cmd_crc`/`resp_crc` (CRC32 over the full streams) + the 16-byte inline prefix.

**RTC/SRAM specifics** (verified in `EXI_DeviceIPL.cpp`):
- `CEXIIPL::TransferByte()` reads the 4-byte command first, then services the
  RTC (`WII_RTC_BASE`/`GC_EPOCH` region), the SRAM window
  (`SRAM_BASE .. SRAM_BASE+SRAM_SIZE`), or the mask-ROM/font region.
- Emulated time: `CEXIIPL::GetEmulatedTime(GC_EPOCH)` via `UpdateRTC()`.
  **Determinism:** drive it from custom RTC (see §d) so the RTC bytes are
  reproducible run-to-run.
- SRAM is reached via `m_system.GetSRAM()` (a `Sram&`), **not** a global.

---

## (d) Booting the IPL deterministically ("GameCube Main Menu")

**Boot entry:**
- `Source/Core/Core/Boot/Boot.h` — `struct BootParameters` with the variant
  `std::variant<Disc, Executable, DiscIO::VolumeWAD, NANDTitle, IPL, DFF>`.
  The **`IPL`** alternative is:
  ```cpp
  struct IPL {
    explicit IPL(DiscIO::Region region_);
    IPL(DiscIO::Region region_, Disc&& disc_);
    std::string path;
    DiscIO::Region region;
    std::optional<Disc> disc;
  };
  ```
- `Source/Core/Core/Boot/Boot.cpp` — `bool CBoot::BootUp(...)` dispatches the
  variant (visitor `BootTitle`); the `IPL` case calls `CBoot::Load_BS2()`, which
  reads the IPL ROM (capped at 2 MiB), CRC32-validates it, **descrambles**, and
  sets PPC state via `SetupBAT()` / `SetupMSR()` / `SetupHID()` /
  `CopyDefaultExceptionHandlers()`.

**There is no stock CLI flag that boots the IPL menu directly.** DolphinNoGUI's
`--exec` builds `BootParameters::GenerateFromFile()` (disc/DOL/ELF), *not* the
`IPL` variant. Two options:

1. **Recommended (DESIGN.md §7): a ~30-line local `--boot-gc-ipl <region>`
   flag** in `Source/Core/DolphinNoGUI/MainNoGUI.cpp` that constructs
   `BootParameters` holding `BootParameters::IPL{region}` (region from
   `DiscIO::Region`) and passes it to the boot path, instead of
   `GenerateFromFile`. This is the deterministic, scriptable entry the tap
   needs.
2. Alternatively point Dolphin at the IPL in the configured GC BIOS dir and
   select "GameCube Main Menu" — but that is GUI-driven and not what a headless
   tap wants.

**Determinism knobs** (config keys verified in
`Source/Core/Core/Config/MainSettings.cpp`; set via `--config` — see §e):

| Purpose | Config info | Key string |
|---|---|---|
| Force interpreter | `MAIN_CPU_CORE` | `Dolphin.Core.CPUCore = 0` |
| Fixed RTC (date/time) | `MAIN_CUSTOM_RTC_ENABLE` + `MAIN_CUSTOM_RTC_VALUE` | `Dolphin.Core.EnableCustomRTC = True`, `Dolphin.Core.CustomRTCValue = <unix epoch>` |
| Null renderer | `MAIN_GFX_BACKEND` | `Dolphin.Core.GFXBackend = Null` |
| Memcard A/B present | `MAIN_SLOT_A` / `MAIN_SLOT_B` + `MAIN_MEMCARD_A_PATH`/`B_PATH` | `Dolphin.Core.SlotA`, `Dolphin.Core.MemcardAPath = <fixed .raw>` |
| Card size | `MAIN_MEMORY_CARD_SIZE` | `Dolphin.Core.MemoryCardSize` |

**SRAM fixture** (verified in `Source/Core/Core/HW/Sram.h` / `Sram.cpp`):
- `struct Sram { BigEndianValue<u32> rtc; SramSettings settings; SramSettingsEx settings_ex; }`
  (total `0x44` bytes). `SramSettings`: `checksum`, `checksum_inv`, `ead0`,
  `ead1`, `rtc_bias` (u32), `vi_horizontal_offset` (s8), `language` (u8),
  `flags` (`SramFlags`). `SramFlags`: `kStereo` (bit 2), `kBootToMenu` (bit 6),
  `kProgressiveScan` (bit 7).
- Use `System::GetSRAM()` to seed a fixed SRAM, then `FixSRAMChecksums(&sram)`
  so the IPL's checksum test passes with a **known** language / sound / screen /
  progressive config. This is the fixture our runtime's EXI-SRAM must match.

Also set (for a stable stream): dual-core OFF, idle-skipping OFF, and DSP mode
fixed (§e). Insert **no disc** (or a fixed dummy) to land on the menu, not a
game.

---

## (e) CLI flags

**Shared parser:** `Source/Core/UICommon/CommandLineParse.cpp`
(`CommandLineParse::ParseArguments(...)`; `-C/--config` applied via a
`CommandLineConfigLayerLoader` → `Config::AddLayer`). Options verified present:

| Flag | Meaning / use for the tap |
|---|---|
| `-b, --batch` | headless, no UI (needs `--exec` or a boot target) |
| `-d, --debugger` | debugger panes (optional; **interpreter is forced via config, not this flag**) |
| `-C, --config <System.Section.Key=Value>` | set any config — this is how we force interpreter, fixed RTC, Null renderer, memcard paths |
| `-v, --video_backend <name>` | e.g. `Null` (or `Software`) — no window needed |
| `-a, --audio_emulation <HLE\|LLE>` | maps to `MAIN_DSP_HLE` (`Dolphin.Core.DSPHLE`); `LLE` → runs real DSP ucode. Pick one and keep it fixed across both trace runs. |
| `-e, --exec <file>` | file boot (disc/DOL/ELF) — **not** the IPL variant |
| `-u, --user <dir>` | isolated user dir → reproducible config/SRAM/memcard |
| `-m, --movie`, `-c, --confirm` | not needed for the tap |

**Frontend (headless):** `Source/Core/DolphinNoGUI/MainNoGUI.cpp` adds
`--platform` (use `headless`), `--exec`, `--nand_title`, `--save_state`,
`--user`; builds boot params via `BootParameters::GenerateFromFile()` /
`NANDTitle`. This is where the new `--boot-gc-ipl` from §d(1) is added.

**Example invocation (after applying the patch):**
```
Dolphin \
  --platform=headless --batch \
  --boot-gc-ipl=NTSC_U \
  -v Null -a LLE \
  -C Dolphin.Core.CPUCore=0 \
  -C Dolphin.Core.CPUThread=False \
  -C Dolphin.Core.EnableCustomRTC=True \
  -C Dolphin.Core.CustomRTCValue=1136073600 \
  -C Dolphin.Core.GFXBackend=Null \
  -C Dolphin.Core.SlotA=1 \
  -C Dolphin.Core.MemcardAPath=/fixtures/slotA.raw \
  --user=/tmp/dolphin-oracle-user \
  --gcn-trace=/tmp/oracle.trace        # <-- new flag added by this patch
```
(`--gcn-trace` is a new tap-output flag added alongside `--boot-gc-ipl`.)

---

## (f) GX FIFO / PE completion (needed for M2, wire it in now)

Because GX FIFO bytes bypass MMIO (§b):
- **CP / FIFO:** `Source/Core/VideoCommon/CommandProcessor.cpp`,
  `Source/Core/VideoCommon/Fifo.cpp`, `Source/Core/VideoCommon/OpcodeDecoding.cpp`
  (all verified present). Emit `GCN_TR_GX` on a FIFO write span: `fifo_addr`,
  `fifo_len`, `fifo_crc` over the span, and the parsed `packet_type`.
  The gather-pipe producer side is `Source/Core/Core/HW/GPFifo.cpp`.
- **PE draw-done / token / finish:** `Source/Core/VideoCommon/PixelEngine.cpp`
  → set `GCN_TR_GX.event` = `GCN_GX_DRAWDONE / _TOKEN / _FINISH` (+ `token`).
  This directly answers DESIGN.md §7 open question #1 (does the menu busy-wait
  on PE completion?).
- **VI** (`GCN_TR_VI`): `Source/Core/Core/HW/VideoInterface.cpp` — emit on XFB
  set + retrace.
- **DSP / AI / ARAM** (`GCN_TR_DSP`): `Source/Core/Core/HW/DSP.cpp`
  (mailbox + ARAM DMA) and `Source/Core/Core/HW/AudioInterface.cpp` (AI
  start/stop, rate).
- **Interrupts** (`GCN_TR_INTR`): `Source/Core/Core/HW/ProcessorInterface.cpp`
  (PI pending/mask) + the interpreter's exception delivery
  (`Source/Core/Core/PowerPC/…` exception check in `SingleStep`).

---

## Emitting the record

Add a tiny local writer TU in the Dolphin tree (e.g.
`Source/Core/Core/GcnTrace.cpp/.h`) that (1) opens the `--gcn-trace` file and
writes a `gcn_trace_file_header` (`producer = GCN_TRACE_PROD_DOLPHIN = 1`), and
(2) appends fixed 204-byte records. **Vendor a copy of `oracle/trace_format.h`
into that Dolphin tree** (the header is *our* net-new code, GPL-3.0-compatible
to carry there; it is data-format only, no Dolphin logic flows back to us). Keep
a global monotonic `seq` counter incremented per emitted record so `diff.py`
orders correctly. Buffer writes; flush on exit / SIGINT so a killed headless run
still yields a usable prefix.

---

## Verification status (per PRINCIPLES "Tool Skepticism")

**Verified to exist on Dolphin `master` (GitHub API / raw content, July 2026):**
- `Interpreter.cpp` — `Interpreter::SingleStepInner/Run/SingleStep/Trace`,
  `m_ppc_state`, `m_start_trace`. ✔
- `PowerPC.h` — `enum class CPUCore { Interpreter=0, JIT64=1, JITARM64=4,
  CachedInterpreter=5 }`; `PowerPCState` fields `pc/npc/gpr[32]/cr/msr/fpscr/
  spr[]/ps[32]`. ✔
- `HW/MMIO.h` — `Mapping::Read<Unit>/Write<Unit>`, `IsMMIOAddress` (GC
  `0x0C00xxxx`), gather-pipe exclusion. ✔
- `HW/EXI/` dir — `EXI_Channel.cpp`, `EXI_Device.cpp/.h`, `EXI_DeviceIPL.cpp`,
  `EXI_DeviceMemoryCard.cpp`; `CEXIIPL::TransferByte/UpdateRTC/GetEmulatedTime`,
  `m_system.GetSRAM()`, `FixSRAMChecksums`. ✔
- `Boot/Boot.h` — `BootParameters` variant incl. `struct IPL{path,region,disc}`,
  `GenerateFromFile`. `Boot/Boot.cpp` — `CBoot::BootUp/Load_BS2`,
  `SetupBAT/MSR/HID`. ✔
- `Config/MainSettings.cpp` — keys `Dolphin.Core.CPUCore / DSPHLE /
  EnableCustomRTC / CustomRTCValue / GFXBackend / SlotA / SlotB /
  MemcardAPath / MemcardBPath / MemoryCardSize`. ✔
- `HW/Sram.h` — `Sram` (0x44 bytes), `SramSettings`, `SramFlags{kStereo=bit2,
  kBootToMenu=bit6, kProgressiveScan=bit7}`, `FixSRAMChecksums`, `InitSRAM`. ✔
- `UICommon/CommandLineParse.cpp` — `-b/--batch`, `-d/--debugger`,
  `-C/--config`, `-v/--video_backend`, `-a/--audio_emulation` (→`MAIN_DSP_HLE`),
  `-e/--exec`, `-u/--user`, `-m/--movie`, `-c/--confirm`. ✔
- `DolphinNoGUI/MainNoGUI.cpp` — `--platform/--exec/--nand_title/--save_state/
  --user`, `GenerateFromFile`. ✔
- `HW/` dir — `GPFifo.cpp`, `Memmap.cpp`, `MMIO.cpp`, `ProcessorInterface.cpp`,
  `DSP.cpp`, `AudioInterface.cpp`, `VideoInterface.cpp`, `Sram.cpp`,
  `SystemTimers.cpp`. ✔
- `VideoCommon/` dir — `CommandProcessor.cpp`, `PixelEngine.cpp`, `Fifo.cpp`,
  `OpcodeDecoding.cpp`. ✔

**NOT independently verified — confirm on your pinned checkout before editing:**
- **Exact line numbers / signatures.** Verified via summarizing fetches
  (function/class *names* and file existence), not a line-by-line raw read.
- **`MMU::Write_U8/16/32/64` exact names** for the `GCN_TR_MEM` hook in `MMU.cpp`
  — inferred, not directly fetched. Grep locally.
- **XER accessor** (`PowerPC::GetXER` vs a member) — name inferred from the
  reconstructed `xer_ca/xer_so_ov/xer_stringctrl` fields; confirm the helper.
- **`--config` System-name prefix.** Config `System::Main` is written as the
  `Dolphin.` prefix in `--config` (i.e. `Dolphin.Core.<Key>`); verify the
  System→string mapping in your tree if a `-C` line is silently ignored.
- **No stock IPL-boot CLI** — confirmed *absent* by inspection; the
  `--boot-gc-ipl` addition (§d1) is **new local work**, not an existing feature.
- **`CustomRTCValue` units** (unix epoch seconds vs GC epoch) — set, then read
  the emitted EXI RTC bytes back through the tap to confirm.

**Open questions for spot-check** (answered *empirically from the first trace*,
not guessed — matches DESIGN.md §7): does the menu busy-wait on PE completion
(§f, VI/GX ordering)? does the card manager gate on RTC + IPL-ROM font decode
before it is legible? DSP LLE vs HLE for a stable, comparable audio stream?
