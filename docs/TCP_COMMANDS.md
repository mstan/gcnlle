# Runtime TCP Debug Server

The gcnrecomp runtime (`gcn_boot`) exposes a TCP debug surface over its
**always-on ring buffers** plus live CPU/RAM state. It mirrors psxrecomp's
`debug_server.c` (JSON-over-newline), sized to gcnrecomp.

- Source: `runtime/src/debug_server.c` (+ `runtime/src/rings.c` for the rings).
- Client: `tools/gcn_debug_client.py`.
- **Enable:** set `GCN_DEBUG_PORT` (e.g. `4380`). When set, `gcn_boot` runs
  **unbounded** and, if the guest stops or parks on unmodeled hardware, keeps
  serving queries until a client sends `quit`. When unset, the rings still
  record — there is just no query surface.
- **Race-free AOT checkpoint:** set `GCN_CHECKPOINT_PC` before launch.
  Optionally set both `GCN_CHECKPOINT_GPR` and `GCN_CHECKPOINT_GPR_VALUE`.
  `GCN_CHECKPOINT_LR` adds an independent live-link-register condition.
  The server auto-arms that condition before the first recompiled block runs;
  this is equivalent to `checkpoint_arm` but cannot miss one-time early title
  initialization while a TCP coordinator is connecting.

The rings record continuously from runtime start (PRINCIPLES: always-on;
probes **query** a window, never arm-then-run-then-hope). Bind is localhost-only.

## Protocol

JSON-over-newline: one request object per line, one response line back.

- Request: `{"id":N,"cmd":"<command>",...params}`
- Success: `{"id":N,"ok":true,...data}`
- Failure: `{"id":N,"ok":false,"error":"<msg>"}`

```bash
python tools/gcn_debug_client.py [--port N] <cmd> [key=value ...]
# key=value become JSON fields (0x/decimal ints stay ints, else strings)
```

## The three always-on rings

| Ring | Cap | Source site | Contents |
|---|---|---|---|
| **MMIO** | 262144 | `mmio.c` (single dispatch point) | every device access: pc, addr, val, size, rw, `mapped` (0 = hit the unmapped fallback). The "device-write ring" is this filtered to `rw=1`. |
| **block/PC** | 262144 | `dispatch.c` (per retired block) | entry PC of every recompiled block; advances the global block index that stamps the other rings |
| **event** | 65536 | device source sites | interrupt/DMA/DSP/EXI **edges** (never per-poll samples) — see `GcnEventKind` in `debug/rings.h` |

## Commands

| Command | Params | Description |
|---|---|---|
| `ping` (`frame`) | — | `{block, pc}` — heartbeat + current block index + live PC |
| `get_registers` (`regs`) | — | 32 GPRs + pc/lr/ctr/cr/xer/msr/srr0/srr1/dar/dsisr/exception/timebase |
| `read_ram` (`dump_ram`) | `addr`, `len` | read up to 64 KB of guest CPU memory (MEM1/MEM2 or the locked-L1 window at `0xE0000000`) as hex |
| `write_ram` | `addr`, `hex` | write hex bytes to guest RAM |
| `mmio_dump` | opt `addr`, `rw`, `count` | newest N MMIO entries; `addr`/`rw` filter over the **full** ring before the cap |
| `block_dump` | opt `count` | newest N retired-block PCs |
| `pc_seen` | `pc` | non-evicting exact coverage query for an AOT dispatcher-entry PC in MEM1/MEM2 or the IPL ROM window; records continuously from process start |
| `checkpoint_arm` | `pc`; optional `gpr` + `gpr_value`; optional `lr` | arm an AOT-safe dispatcher-boundary checkpoint, optionally conditional on one live GPR and/or LR; native shards remain enabled |
| `checkpoint_status` | — | report armed/parked state, condition, live PC, and block index |
| `checkpoint_resume` | — | resume a parked checkpoint and disarm it |
| `checkpoint_continue` | `pc`; optional `gpr` + `gpr_value`; optional `lr` | while parked, atomically arm the next AOT checkpoint and resume; this avoids losing short intervals to an arm-after-resume race |
| `event_dump` | opt `count` | newest N device edges |
| `screenshot` (`screenshot_file`) | opt `path` (default `_work/screenshot.ppm`) | decode the XFB the VI is scanning out to a PPM: geometry from the guest-programmed VI regs (`gcn_vi_xfb_info`), YUY2→RGB via inverse BT.601 exactly as Dolphin's XFB decode (TextureConversionShader.cpp:1009). Returns `{path, width, height, xfb_addr, mean_luma}` — `mean_luma` ≈16 means black. Errors honestly if the guest has no XFB programmed. |
| `dsp_state` | — | live DSP-LLE core state: `pc`, `control`, non-consuming peeks of both mailboxes (bit 31 = mail pending). For diagnosing CPU↔DSP handshake stalls. |
| `rtc_state` | — | latch the live EXI RTC from current emulated CPU cycles and report `counter`, `running`, `anchor_cycles`, and `cpu_cycles`; this never resamples host time. |
| `audio_state` | — | report the live AID-to-host stream: WASAPI open state, sample rate, signal peak, received/audible/buffered frames, submitted packets, underruns, waits, and dropped frames. |
| `gx_draw_state` | — | with `GCN_GX_TEV_CENSUS=1`, return the last observed draw in every shading-config bucket: raw BP/TEV words, primitive/vertex metadata, and each active texture's MEM1 address, exact tiled byte length, draw-time FNV-1a hash, and first 32 bytes. Use the reported physical ranges with `cosim_pages`/`read_ram` to trace a pixel mismatch back to guest memory. |
| `set_input` | opt `buttons`, `stick_x`, `stick_y`, `substick_x`, `substick_y`, `trigger_l`, `trigger_r` (ints, unspecified = leave unchanged); opt `reset`:1 (snap to neutral, ignoring every other field) | Drive the SI model's injected GC-controller pad report (ROADMAP M3/M4 input-injection surface) — the menu polls this every frame through `si.c`. `buttons` is the raw OR of GC pad bits (LEFT 0x0001, RIGHT 0x0002, DOWN 0x0004, UP 0x0008, Z 0x0010, R 0x0020, L 0x0040, A 0x0100, B 0x0200, X 0x0400, Y 0x0800, START 0x1000 — `PAD_USE_ORIGIN` 0x0080 is ORed in unconditionally by the model, never passed here); sticks/substick/triggers are raw bytes, 0x80 = centered. Echoes the resulting state. Hold a direction/button for 30-60 frames of guest time (PAD lib debounces a single poll) before releasing. |
| `quit` | — | end the run + shut the server down cleanly |

### Co-simulation commands

| Command | Params | Description |
|---|---|---|
| `cosim_status` | none | With `GCN_COSIM=1`, report whether the interpreter-only diagnostic guest is parked plus its retired-instruction count, cycle count, current PC, and outstanding coordinator budget. |
| `cosim_step` | `count` | Grant a parked co-sim guest exactly N interpreter instructions, after which it parks again. Returns immediately; poll `cosim_status` until `parked:true`. |
| `cosim_run_to` | `pc`, optional `max_instructions` | Run the parked diagnostic guest until the named guest PC is reached, or park when the safety budget is exhausted. This supports milestone comparison across hardware delay loops. |
| `cosim_state` (`state_hash`) | none | Coherent parked-state hashes for the PPC architectural state currently represented by `CPUState`, MEM1, MEM2, and Gekko's 256-KiB locked-L1 backing. The response explicitly reports `complete:false` until canonical device snapshots are added. |
| `cosim_cpu_bytes` | none | Parked-only canonical byte serialization of exactly the CPU fields covered by the `cosim_state` CPU sub-hash. The coordinator uses this for the template's hash-vs-byte validation gate. |
| `cosim_pages` | optional `space` (`mem1`/`mem2`/`l1`), `start`, `count` | FNV-1a hashes for up to 256 4-KiB pages, used to localize a memory sub-hash mismatch before fetching bytes with `read_ram`. |
| `cosim_inject` | `kind` (`gpr`/`ram`/`timebase`), `index` or `addr`, optional `xor`, or `value_hi` + `value_lo` | Parked-only fault injection for the mandatory detection gate, or an explicitly disclosed oracle-seam normalization. |

### Examples

```bash
python tools/gcn_debug_client.py regs
python tools/gcn_debug_client.py read_ram addr=0x80000034 len=8
python tools/gcn_debug_client.py mmio_dump addr=0xCC00202C count=8   # watch a poll
python tools/gcn_debug_client.py event_dump count=16
python tools/gcn_debug_client.py set_input buttons=0x0008   # hold D-pad UP
python tools/gcn_debug_client.py set_input reset=1          # release back to neutral
python tools/gcn_debug_client.py quit
```

## Not yet wired (grows with the milestones)

- **`device_state`** — per-device register snapshots, added as each device model
  grows a reason to expose one (`dsp_state` is the first).

## Differential co-simulation

`GCN_COSIM=1` is a diagnostic mode implementing the parked-checkpoint method in
`F:\Projects\recomp-template\DIFFERENTIAL-COSIMULATION.md`. It requires
`GCN_DEBUG_PORT`. The runtime parks before the reset-vector instruction and uses
the existing PPC interpreter for coordinator-granted instruction budgets. This
does not change the ordinary AOT-first execution path.

Use deterministic host settings for every validation run:

```powershell
$env:GCN_COSIM = '1'
$env:GCN_CYCLES_DERIVED = '1'
$env:GCN_GX_PIPELINE = '0'
$env:GCN_GX_THREADS = '1'
$env:GCN_WINDOW = '0'
$env:GCN_AUDIO = '0'
$env:GCN_THROTTLE = '0'
```

The current `cosim_state` surface is intentionally labeled incomplete: it
canonically covers CPU + MEM1 + MEM2 + locked L1, but not yet every device
scheduler/FIFO.
This is useful for bringing up determinism and register/RAM comparisons, but it
is not the template's full-state decision procedure until device coverage and
all four validation gates are complete.

`tools/gcn_cosim.py gate1` also performs Gate 4 every ten checkpoints by
default: it compares the canonical CPU serialization and every byte of MEM1,
MEM2, and locked L1 even when their hashes match. Use
`--byte-audit-every N` to change the interval (or zero to disable it for a
deliberately fast smoke test).

`tools/gcn_cosim.py ab-step` parks this runtime and Dolphin's independent GDB
stub at the shared BS2 entry and compares common PowerPC registers plus selected
RAM after each instruction budget. Dolphin exposes TBL/TBU as zero at that HLE
entry, then materializes a restart-dependent fake-timebase epoch on the first
`mftb`. For the IPL delay loop,
`ab-step --align-dolphin-timebase-at 10` performs one audited write of that
epoch and `r5` into the parked runtime after the first `mftb`, then immediately
resumes strict comparison. It does not mask either field later.

For Dolphin continue-to-breakpoint milestones, launch with both
`-C Dolphin.General.GDBPort=<port>` and
`-C Dolphin.Interface.DebugModeEnabled=True`. See
`docs/WIND_WAKER_COSIM.md` for the validated BS2, DI-transfer, and apploader
entry gates and their explicitly bounded oracle seams.

Note the screenshot shows whatever the XFB holds — until the GX command
processor is modeled the menu never draws, so a valid all-black image
(`mean_luma` ≈ 16) is the *correct* current output, not a bug.
