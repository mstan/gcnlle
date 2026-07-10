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
| `screenshot` (`screenshot_file`) | opt `path` | **stub until VI/XFB (M2)** — returns a clear "no framebuffer yet" error, never a fake image |
| `quit` | — | end the run + shut the server down cleanly |

### Examples

```bash
python tools/gcn_debug_client.py regs
python tools/gcn_debug_client.py read_ram addr=0x80000034 len=8
python tools/gcn_debug_client.py mmio_dump addr=0xCC00202C count=8   # watch a poll
python tools/gcn_debug_client.py event_dump count=16
python tools/gcn_debug_client.py quit
```

## Not yet wired (grows with the milestones)

- **`screenshot`** — needs VI/XFB scanout (M2). Command exists; returns "not yet".
- **Input injection** (`set_input`) — deferred until the menu is reachable and
  something consumes pad input (M3/M4); wiring it before then would be untestable.
- **`device_state`** — per-device register snapshots, added as each device model
  grows a reason to expose one.
