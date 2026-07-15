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
| `read_ram` (`dump_ram`) | `addr`, `len` | read up to 64 KB of guest RAM (cached/uncached mirror → MEM1) as hex |
| `write_ram` | `addr`, `hex` | write hex bytes to guest RAM |
| `mmio_dump` | opt `addr`, `rw`, `count` | newest N MMIO entries; `addr`/`rw` filter over the **full** ring before the cap |
| `block_dump` | opt `count` | newest N retired-block PCs |
| `event_dump` | opt `count` | newest N device edges |
| `screenshot` (`screenshot_file`) | opt `path` (default `_work/screenshot.ppm`) | decode the XFB the VI is scanning out to a PPM: geometry from the guest-programmed VI regs (`gcn_vi_xfb_info`), YUY2→RGB via inverse BT.601 exactly as Dolphin's XFB decode (TextureConversionShader.cpp:1009). Returns `{path, width, height, xfb_addr, mean_luma}` — `mean_luma` ≈16 means black. Errors honestly if the guest has no XFB programmed. |
| `dsp_state` | — | live DSP-LLE core state: `pc`, `control`, non-consuming peeks of both mailboxes (bit 31 = mail pending). For diagnosing CPU↔DSP handshake stalls. |
| `rtc_state` | — | latch the live EXI RTC from current emulated CPU cycles and report `counter`, `running`, `anchor_cycles`, and `cpu_cycles`; this never resamples host time. |
| `audio_state` | — | report the live AID-to-host stream: WASAPI open state, sample rate, signal peak, received/audible/buffered frames, submitted packets, underruns, waits, and dropped frames. |
| `set_input` | opt `buttons`, `stick_x`, `stick_y`, `substick_x`, `substick_y`, `trigger_l`, `trigger_r` (ints, unspecified = leave unchanged); opt `reset`:1 (snap to neutral, ignoring every other field) | Drive the SI model's injected GC-controller pad report (ROADMAP M3/M4 input-injection surface) — the menu polls this every frame through `si.c`. `buttons` is the raw OR of GC pad bits (LEFT 0x0001, RIGHT 0x0002, DOWN 0x0004, UP 0x0008, Z 0x0010, R 0x0020, L 0x0040, A 0x0100, B 0x0200, X 0x0400, Y 0x0800, START 0x1000 — `PAD_USE_ORIGIN` 0x0080 is ORed in unconditionally by the model, never passed here); sticks/substick/triggers are raw bytes, 0x80 = centered. Echoes the resulting state. Hold a direction/button for 30-60 frames of guest time (PAD lib debounces a single poll) before releasing. |
| `quit` | — | end the run + shut the server down cleanly |

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

Note the screenshot shows whatever the XFB holds — until the GX command
processor is modeled the menu never draws, so a valid all-black image
(`mean_luma` ≈ 16) is the *correct* current output, not a bug.
