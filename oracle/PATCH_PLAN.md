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

### f1. `-v Software` EFB->XFB copy hook — VERIFIED on the pinned checkout (2026-08-11)

Per COSIM_DESIGN.md §7 task 2. Read the actual source, not inferred.

**Trigger site** — `Source/Core/VideoCommon/BPStructs.cpp`, function
`BPWritten(...)`, `case BPMEM_TRIGGER_EFB_COPY:` (line 240). The
`PE_copy.copy_to_xfb == 1` branch (line ~315-343) computes `destAddr`
(`bpmem.copyTexDest << 5`), `destStride` (`bpmem.copyDestStride << 5`), and
calls (line 339):
```cpp
g_texture_cache->CopyRenderTargetToTexture(
    destAddr, EFBCopyFormat::XFB, copy_width, height, destStride,
    is_depth_copy, srcRect, false, false, yScale, ...);
```

**Copy-execute site (the actual hook, common to every backend — NOT
backend-specific code as originally assumed)** —
`Source/Core/VideoCommon/TextureCacheBase.cpp`,
`void TextureCacheBase::CopyRenderTargetToTexture(...)` (line 2129).
`TextureCacheBase` is the shared, non-virtual implementation for this
function; `SW::TextureCache` (`Source/Core/VideoBackends/Software/TextureCache.h`)
only overrides the two backend-specific sub-steps
(`CopyEFB`/`CopyEFBToCacheEntry`), not this function.

- `const bool is_xfb_copy = ...` at line 2193.
- `bool copy_to_vram = g_backend_info.bSupportsCopyToVram && ...` at line 2194.
  For `-v Software`, `g_backend_info.bSupportsCopyToVram = false` is set
  unconditionally in `Source/Core/VideoBackends/Software/SWmain.cpp:71`, so
  `copy_to_vram` is **always false** for this backend → `copy_to_ram` is
  always true (line 2195-2197: `!copy_to_vram` alone forces it) and the
  VRAM-entry block (`if (copy_to_vram) {...}`, line 2304) never executes, so
  `entry` stays null and the deferred-flush branch (`else { entry->pending_efb_copy
  = ... }`, line ~2410-2417) is unreachable — Software always takes the
  **immediate flush** branch.
- Immediate flush, line 2404-2408:
  ```cpp
  if (!copy_to_vram || !g_ActiveConfig.bDeferEFBCopies)
  {
    // Immediately flush it.
    WriteEFBCopyToRAM(dst, bytes_per_row / sizeof(u32), num_blocks_y, dstStride,
                      std::move(staging_texture));
  }
  ```
  `WriteEFBCopyToRAM` (line 2535-2541) calls
  `staging_texture->ReadTexels(copy_rect, dst_ptr, stride)` — **synchronous**,
  blocks until the encoded bytes have actually landed at `dst_ptr`. `dst`
  (line 2244: `memory.GetPointerForRange(dstAddr, covered_range)`) is a raw
  pointer directly into **guest RAM at the real XFB destination address**,
  not an EFB-source staging buffer.
- **Format confirmed XFB-destination YUY2, not EFB-source**: `TextureEncoder::Encode`
  (`Source/Core/VideoBackends/Software/TextureCache.h:19`, called from `CopyEFB`
  override) encodes EFB pixels into the `EFBCopyFormat::XFB` (YUY2) layout
  *before* `WriteEFBCopyToRAM` flushes it to `dst` — so by the time the hook
  fires, `dst` already holds final YUY2 bytes, exactly what VI scans out.
- **Height is exact, no padding**: `TexDecoder_GetEFBCopyBaseFormat(EFBCopyFormat::XFB)
  == TextureFormat::XFB` (`TextureDecoder_Common.cpp:244-245`), whose block
  height is `1` (`TextureDecoder_Common.cpp:125-126` / `:193-194`), so
  `num_blocks_y == tex_h` exactly (`actualHeight = AlignUp(tex_h, 1) == tex_h`)
  — `num_blocks_y*dstStride` is both the exact live-data span and within
  `covered_range`, so reading exactly `num_blocks_y*dstStride` bytes from
  `dst` is safe and lossless.

**Conclusion:** hook immediately after the `WriteEFBCopyToRAM(...)` call at
`TextureCacheBase.cpp:2406-2408`, guarded by `if (is_xfb_copy)`, using
`dst`/`dstAddr`/`tex_w`/`num_blocks_y`/`dstStride` already in scope. This one
site is correct for **every** backend (it is common code, not
`VideoBackends/Software/*`), but for `-v Software` specifically it is
*guaranteed* synchronous/immediate (never the deferred path other backends
can take), which is exactly the property the byte-exact comparator needs.
Implemented as `GcnTrace::EmitXfbCopy(...)`, see task 3 below.

### f2. Two environment footguns hit while validating task 3 (2026-08-11) — read before any future headless invocation

**1. `-v Software` is silently WRONG and falls back to the default backend
(D3D11 on this Windows box) with no error.** The Software backend's CLI/config
match key is `SW::VideoSoftware::CONFIG_NAME = "Software Renderer"`
(`Source/Core/VideoBackends/Software/VideoBackend.h:22`), **not** `"Software"`.
`VideoBackendBase::ActivateBackend()` (`VideoCommon/VideoBackendBase.cpp:239-252`)
does a `std::ranges::find` by `GetConfigName()`; on no match it just `return`s
and silently keeps whatever `g_video_backend` already defaulted to —
`backends.front()` from `GetAvailableBackends()` (`:204-236`), which on
`_WIN32` is `DX11::VideoBackend` (pushed first, `:208`). **This means every
prior "-v Software" invocation in this repo's own scripts and in the
"dolphin-oracle-recipe" memory note was actually running D3D11**, not the
software rasterizer — confirmed by instrumenting `ActivateBackend` directly:
requesting `"Software"` logs a clean list of 6 registered backends (`D3D`,
`D3D12`, `OGL`, `Vulkan`, `Software Renderer`, `Null`) and "NO MATCH, keeping
default (D3D)". It still renders a correct-looking frame (D3D11 works fine),
which is why this went unnoticed — the bug is invisible unless you check
`g_backend_info.bSupportsCopyToVram` or similar backend-identity state.
**Always pass the full string `Software Renderer` (quoted) to `-v`/
`--video_backend` on this checkout**, or use `-C
Dolphin.Core.GFXBackend="Software Renderer"`.

**2. `-v "Software Renderer"` (with the embedded space) requires the space to
survive into the actual `argv[]` the process receives, and several common
Windows launch paths silently swallow it without any error:**
- **PowerShell `Start-Process -ArgumentList @(...)`** joins the array with
  plain spaces — it does **not** re-quote elements containing spaces (unlike
  `System.Diagnostics.ProcessStartInfo.ArgumentList` used directly from .NET
  code, which does). Verified by instrumenting `wmain()`
  (`Source/Core/DolphinNoGUI/MainNoGUI.cpp:405-415`) to print
  `GetCommandLineW()` and the resulting `argv[]`: an array element
  `'Software Renderer'` arrives on the wire as bare `-v Software Renderer`,
  which `CommandLineToArgvW` (correctly) splits into two argv entries
  `"Software"` and `"Renderer"` — the option parser only consumes the first.
  **Fix:** embed literal quotes in the array element itself:
  `@('-v','"Software Renderer"', ...)`.
- **A `.bat` file containing `-v "Software Renderer"` literally does carry the
  quotes through correctly** (cmd.exe does not strip them from an external
  command's argument text) — this form works as-is.
- Only once the space survives into `argv[]` does `ActivateBackend` find the
  `"Software Renderer"` entry (confirmed via the same instrumentation).

**3. Even with the name fixed, the true Software Renderer backend CANNOT
initialize under `--platform=headless` on Windows in this checkout, and fails
*silently* (clean exit, no console output, no log) unless `Dolphin.Interface.
UsePanicHandlers` is left in its default state.** `VideoSoftware::Initialize`
(`VideoBackends/Software/SWmain.cpp:92-96`) calls `SWOGLWindow::Create(wsi)`
(`VideoBackends/Software/SWOGLWindow.cpp:18-28`), which calls
`GLContext::Create(wsi)` (`Common/GL/GLContext.cpp:78-118`). That function's
platform dispatch is:
  ```cpp
  #if defined(_WIN32)
    if (wsi.type == WindowSystemType::Windows)
      context = std::make_unique<GLContextWGL>();
  #endif
  ...
  #if HAVE_EGL
    if (wsi.type == WindowSystemType::Headless || wsi.type == WindowSystemType::FBDev)
      context = std::make_unique<GLContextEGL>();
  #endif
    if (!context) return nullptr;
  ```
  There is **no Windows-headless case**: `WindowSystemType::Windows` needs a
  real `HWND` (not present under `--platform=headless`), and the
  `WindowSystemType::Headless` branch is gated behind `HAVE_EGL`, which this
  build does not define for Windows (ANGLE/EGL not wired up in this checkout's
  Windows target). So `GLContext::Create` returns `nullptr` unconditionally
  for headless on Windows, `SWOGLWindow::Create` hits its
  `PanicAlertFmt("Failed to create OpenGL window")` path and returns
  `nullptr`, and `VideoSoftware::Initialize` returns `false` — Dolphin then
  aborts the boot before any GX/BP traffic exists. `PanicAlertFmt` output
  (like all of Dolphin's own logger) goes to `<userdir>/Logs/`, which is only
  created if file logging is explicitly enabled — with the default config
  there is genuinely **zero output anywhere**, just a clean `exit(0)` in well
  under a second. Do not mistake this for "reached the menu instantly" or
  "trace tap broken" — it is a headless/Windows OpenGL-context limitation,
  upstream of anything this patch touches.
- **Workaround used to validate task 3:** `-v Null`. `Null::VideoBackend` sets
  `g_backend_info.bSupportsCopyToVram = false` the same as the Software
  backend (confirmed by instrumenting the `copy_to_vram`/`copy_to_ram`
  computation directly: identical values, identical branch taken —
  `copy_to_vram=0 copy_to_ram=1`, immediate-flush branch, `is_xfb_copy=1`) and
  needs no real GPU/window surface, so it exercises the *exact* hook site
  and code path task 2/3 target, just with the Null backend's (uninteresting,
  possibly-blank) pixel content instead of real software-rasterized pixels.
  This is sufficient to validate the dump *mechanism* (task 3's literal
  acceptance: correct prefix, correct byte count, sequential ordinals) but
  **not** sufficient for any real pixel-content comparison — getting the real
  Software Renderer backend running (on Linux where `HAVE_EGL` is normally
  available, or by wiring up EGL/ANGLE for the Windows target) is a
  precondition for COSIM_DESIGN.md §6's actual validation runs and should be
  tracked as its own follow-up rather than assumed solved by this patch.

### f3. beads-u2x.3 fixed (2026-08-11): loud backend-selection failure + real headless Software Renderer on Windows

Both bugs in f2 above are now fixed. **Bead beads-u2x.3, status done.**

**Fix 1 — silent backend fallback (f2 point 1).**
`VideoBackendBase::ActivateBackend()` (`Source/Core/VideoCommon/VideoBackendBase.cpp:241`)
now:
- Resolves the convenience alias `"Software"` → `"Software Renderer"` before
  the `std::ranges::find` (so both spellings work; no more space-in-argv
  footgun for the common case).
- On no match, prints `FATAL: unknown video backend '<name>' ...` plus the
  full list of registered backend names to stderr and calls `std::exit(1)`
  instead of silently `return`ing and leaving `g_video_backend` at
  `backends.front()` (DX11 on Windows). Verified: `-v Bogus` now prints the
  fatal line and exits 1 (previously: silent D3D11).
- On a successful activation, unconditionally prints
  `[gcnrecomp] video backend ACTIVE: '<resolved>' (requested '<name>')` to
  stderr — this is now the loud, unambiguous way to confirm which backend is
  really live, since the old code path made that state invisible short of
  instrumenting internals.

**Fix 2 — headless Software Renderer on Windows (f2 point 3).**
Investigated whether GLContext is used for anything besides presentation in
this backend: **confirmed no** — `Rasterizer.cpp` and `EfbInterface.cpp`
(grepped for `gl[A-Z]|GL_|OpenGL`) make **zero** GL calls; rasterization is
pure CPU. The only two GL uses are (1) the `GLExtensions::Init`/GL-3.1 gate in
`SWOGLWindow::Initialize` and (2) the presentation quad in
`SWOGLWindow::ShowImage`, and `SWGfx::ShowImage`
(`Source/Core/VideoBackends/Software/SWGfx.cpp:110-115`) *already* skips
calling into (2) whenever `IsHeadless()` is true — the only thing missing was
a way to reach that headless state on Windows at all, since
`GLContext::Create` (`Common/GL/GLContext.cpp:78-108`) has no
`WindowSystemType::Windows`-without-HWND branch and the
`WindowSystemType::Headless` branch is `#if HAVE_EGL`, undefined on this
Windows build (confirmed in f2 point 3 already).

Chose the **no-present mode** option (not the hidden-HWND fallback) since it
was not structurally hard once traced: it only required propagating a
"no GLContext acquired" state through three call sites, all already
identified:
- `SWOGLWindow::Initialize` (`SWOGLWindow.cpp:35`): if
  `wsi.type == WindowSystemType::Headless`, return `true` immediately without
  calling `GLContext::Create` or doing any of the shader/texture GL setup.
  `m_gl_context` stays null.
- `SWOGLWindow::IsHeadless()` (`SWOGLWindow.cpp:30`): now
  `!m_gl_context || m_gl_context->IsHeadless()` instead of unconditionally
  dereferencing `m_gl_context->IsHeadless()`.
- `SWOGLWindow::ShowImage` (`SWOGLWindow.cpp:87`): defensive `if
  (!m_gl_context) return;` guard (belt-and-suspenders; `SWGfx::ShowImage`
  already gates this call on `!IsHeadless()`).
- `SWGfx::BindBackbuffer` (`SWGfx.cpp:58`) and `SWGfx::GetSurfaceInfo`
  (`SWGfx.cpp:138`): both called `m_window->GetContext()` and dereferenced it
  unguarded for backbuffer width/height (presentation-window sizing only,
  irrelevant to EFB/XFB content) — both now null-check and fall back to a
  1x1/0x0 placeholder (already clamped to 1 by the existing `std::max(...,
  1u)` in `GetSurfaceInfo`).

Diff stat (`git diff --stat`):
```
 Source/Core/VideoBackends/Software/SWGfx.cpp       | 21 +++++++++---
 Source/Core/VideoBackends/Software/SWOGLWindow.cpp | 27 ++++++++++++++-
 Source/Core/VideoCommon/VideoBackendBase.cpp       | 40 ++++++++++++++++++++--
 3 files changed, 81 insertions(+), 7 deletions(-)
```

**Build:** MSBuild solution-level `/t:DolphinNoGUI` failed for an unrelated
reason — the vendored `glslang` external's Makefile-type project
(`Externals/glslang/glslang.vcxproj`) re-invokes `cmake` + a nested
`msbuild.exe` + `mkdir -p`/`copy` every build regardless of whether its
outputs are current, and that nested invocation fails intermittently under
`/m:2` (and even `/m:1`) with `exited with code -1` — pre-existing, unrelated
to this patch, and glslang's own outputs were already present and current.
**Workaround:** build the two projects that actually needed rebuilding
directly, skipping already-built project references:
```
MSBuild Source/Core/DolphinLib.vcxproj  -p:Configuration=Release -p:Platform=x64 -p:BuildProjectReferences=false -m:2
MSBuild Source/Core/DolphinNoGUI/DolphinNoGUI.vcxproj -p:Configuration=Release -p:Platform=x64 -p:BuildProjectReferences=false -m:2
```
(`BuildProjectReferences=false` skips rebuilding referenced projects — safe
here since only `DolphinLib`'s own three source files changed; `glslang.lib`
etc. were already built and current.) Confirmed the compiler actually
recompiled the three touched files (`SWGfx.cpp`, `SWOGLWindow.cpp`,
`VideoBackendBase.cpp`) and relinked both `DolphinLib.lib` and
`DolphinNoGUI.exe`.

**Environment footgun hit during validation, distinct from f2's:**
`NoDefaultCurrentDirectoryInExePath=1` is set process-wide on this box, so
`cmd.exe` (including inside a `.bat`) will **not** find a bare
`DolphinNoGUI.exe` by searching the current directory even right after a
successful `cd /d` into the directory containing it (`dir DolphinNoGUI.exe`
succeeds in the same shell; running it bare fails with "not recognized as an
internal or external command"). Always invoke it by full path
(`F:\...\Binary\x64\DolphinNoGUI.exe ...`) in any script on this box, not by
bare name after a `cd`.

**Validation (headless boot with the TRUE Software Renderer, NTSC_U IPL,
`GCN_TRACE_XFB_DUMP`, two independent ~45s runs from a fresh user dir each):**
- **(i) backend identity, loud:** stderr showed
  `[gcnrecomp] video backend ACTIVE: 'Software Renderer' (requested 'Software
  Renderer')` on both runs — confirmed the real software rasterizer was live,
  not a silent D3D11 fallback.
- **(ii) real pixels:** run 1 produced 3353 `dolphin.N.yuy2` dumps; run 2
  (independent boot) produced 2441. Histogrammed several (`dolphin.10`,
  `.1000`, `.3000`, `.3352`): all non-constant (dozens of distinct byte
  values, not a solid fill) — unlike task 3's `-v Null` mechanism-only proof,
  which had no real pixel content. Decoded `dolphin.10.yuy2` (592x226 YUY2,
  header-declared `width` is the real pixel width, stride == width*2 bytes)
  to PPM/PNG and visually confirmed the GameCube cube boot logo; decoded
  `dolphin.2500.yuy2` and `dolphin.3352.yuy2` and visually confirmed the IPL
  main-menu card carousel ("Game Play" panel with the pink dot border and,
  in later frames, the neighboring "Options"/"Calendar" panel labels) —
  unambiguous real software-rasterized IPL content, not blank/constant bytes.
- **(iii) unknown backend fails loudly:** `-v Bogus` printed the `FATAL:
  unknown video backend 'Bogus' ...` line (with the full registered-backend
  list) to stderr and exited with code 1 — matches the fix-1 acceptance
  criterion exactly.

**Determinism (cheap two-run byte-exact diff, same boot command, same ~45s
wall bound, fresh user dir each run):** of 2441 overlapping `pub_seq`
ordinals, publications **0 through 1050 were byte-exact identical** between
the two independent runs; **1390 of the 2441 overlapping files (57%) differed
starting at `pub_seq` 1051**. Decoding a divergent pair (`dolphin.1051.yuy2`,
86846 of 267596 bytes differing) showed both runs at the *same* menu-carousel
scene ("Game Play" panel, "Options"/"Calendar" neighbors visible) but at a
visibly different rotation/pan phase — **animation-timing drift, not
rasterizer non-determinism**. This is the expected consequence of *not*
setting `PATCH_PLAN.md §d`'s determinism knobs
(`Dolphin.Core.CPUCore=0`/interpreter, `Dolphin.Core.EnableCustomRTC=True` +
fixed `CustomRTCValue`) for this quick validation run — the CPU core defaults
to JIT (wall-clock/host-timing-coupled) and the RTC defaults to real time, so
the two runs' menu-animation phase relative to `pub_seq` drifts as soon as
the boot reaches the animated menu, exactly the failure mode those knobs
exist to eliminate. **The pre-menu boot prefix (0-1050) being byte-exact is
itself decent evidence the software rasterizer is deterministic** for a
fixed instruction stream; confirming determinism *through* the animated menu
requires re-running this same two-run diff with the interpreter + custom-RTC
knobs set, which is follow-up work, not done here.

**Anomalies noted, not investigated further (out of scope for this bead):**
- Run 1 and run 2 reached different total publication counts (3353 vs 2441)
  in the same ~45s wall-clock bound — consistent with the same JIT/host-timing
  coupling that explains the animation-phase drift above, not a new bug.
- `dolphin.3000.yuy2` in run 1 had `height=2` (vs. the usual 226) — a
  much-smaller-than-usual publication, plausibly a partial/transition-frame
  copy; not confirmed against Dolphin's own copy-size logic, flagged only in
  case it recurs as a real bug once determinism-knob-pinned comparisons start.

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

## Trace-window control (GcnTrace.h)

`NoteRetired(insn)` arms the tap's stop flag on two conditions, both env-tunable
so the same build can capture different boot windows:

- `GCN_TRACE_MAX_INSNS` — stop after N retired instructions (0 = unlimited).
- `GCN_TRACE_MAX_SC` — stop after the Nth `sc` (primary opcode 17). **Default 1**
  reproduces the original "first-syscall boundary" trace. Set to **0** to run
  through every `sc` (the BS2 exception handlers execute in Dolphin), bounded
  only by `GCN_TRACE_MAX_INSNS` — this is how we extend the oracle past the
  stage-2 syscall wall (`oracle/dolphin_trace_ext.bat`) once the runtime can
  execute the low-memory handlers itself.
- `GCN_TRACE_NO_RETIRED` — MMIO/EXI records only (skip per-instruction RETIRED),
  keeping the extended trace compact for the value+order diff.

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
