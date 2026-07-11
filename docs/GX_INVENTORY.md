# GX FIFO packet inventory — IPL menu (M2)

Complete decode of the GX FIFO command stream the IPL "GameCube Main Menu"
emits through the gather pipe, captured by the always-on FIFO recorder ring
(`_work/fifo_inventory.json`, 709 bursts × 32 bytes = **22688 bytes**).

Decoder: `tools/gx_fifo_decode.py` (stdlib-only). Format transcribed from
Dolphin `VideoCommon/OpcodeDecoding.{h,cpp}`, `CPMemory.h`, `XFMemory.h`,
`BPMemory.h`, and the `VertexLoader_*` size tables — never guessed.

Reproduce:

```
python tools/gx_fifo_decode.py _work/fifo_inventory.json            # full listing
python tools/gx_fifo_decode.py _work/fifo_inventory.json --summary  # inventory only
```

## Sanity: end-to-end decode

- **22688 bytes decoded into 2713 commands, stream consumed end-to-end, no
  truncation.** The tail is clean: `... VAT_A[1]/VAT_B[1]/VAT_C[1]` loads →
  `NOP ×32` → `SETDRAWDONE = 0x000002` → `NOP ×2`. The stream does **not** cut
  off mid-command.
- Unknown opcodes are a hard error in the decoder (offset + pc + context), never
  skipped. None occurred.

## (a) Ordered command summary (collapsed)

| Opcode | Count | Notes |
|---|---:|---|
| `LOAD_BP_REG` (0x61) | 1898 | Blit/PE/TEV/texture/scissor state |
| `LOAD_XF_REG` (0x10) | 574 | matrices, channel config, viewport, projection, texgen |
| `LOAD_CP_REG` (0x08) | 191 | VCD/VAT vertex formats, matrix index, array base/stride |
| `CALL_DL` (0x40) | 44 | display lists in guest RAM — **contents NOT in this stream** |
| `INVL_VC` (0x48) | 4 | vertex-cache invalidate (1 byte) |
| `NOP` (0x00) | 34 bytes | 2 runs only (×32, ×2); negligible padding |
| **`DRAW` (0x80–0xBF)** | **0** | **no primitive command appears in the gather-pipe stream** |

Structural shape: an init preamble (VAT group-B defaults, XF `ERROR`/`DUALTEX`),
then a repeated per-object pattern of `MATINDEX + SETMATRIXIND + SETTEXMTXINFO +
SETPOSTMTXINFO` matrix-index programming, texture/TEV/GENMODE setup, a `CALL_DL`
into a per-object display list, and periodic EFB→XFB copy+clear bursts. It ends
with one `SETDRAWDONE`.

**Key architectural fact:** the gather-pipe stream is *state setup + display-list
calls + framebuffer copies*. All actual geometry (`DRAW` primitives, and likely
more register loads) lives **inside the 44 display lists**, which are in guest
RAM (physical `0x00AD7520`–`0x00AD81xx`, i.e. guest `0x80AD…`) and are **not**
captured by the gather-pipe recorder. See (d)/(e).

## (b) Unique register / attribute set

### BP registers (name — values seen)

Blit/PE cluster and pipeline config. Highlights:

- `GENMODE 0x00` = `0x004210 / 0x004211`
- `SCISSORTL 0x20 / SCISSORBR 0x21 / SCISSOROFFSET 0x59` — scissor to ~640×480.
- `ZMODE 0x40` (`0x06/0x17/0x1F`), `BLENDMODE 0x41`, `ZCOMPARE 0x43`,
  `CONSTANTALPHA 0x42`, `ALPHACOMPARE 0xF3`.
- Texture units `TX_SETMODE0/1`, `TX_SETIMAGE0-3` (0x80–0x97): texture addresses
  `0x0D8000…0x0DDC00` region, sizes via `SU_SSIZE/SU_TSIZE`.
- TEV pipeline: `TEV_COLOR_ENV 0xC0` / `TEV_ALPHA_ENV 0xC1` (up to 16 stages),
  `TEV_REGISTER 0xE0-0xE7`, `TEV_KSEL 0xF6-0xFD`, `TREF/IREF` indirect setup,
  `FOG 0xE8-0xF2`.
- **EFB-copy cluster 0x49–0x54** — see (c).
- `SETDRAWDONE 0x45` — see (d).

(Full per-register value lists in `--summary` output.)

### CP registers — vertex descriptor / attribute formats

- `VCD_LO 0x50` (45 writes) and `VCD_HI 0x60` (45 writes) — vertex descriptor.
  Values include `0x00000400/0x00000600/0x00001400/0x00001600/0x00002200`
  (VCD_LO) with `VCD_HI ∈ {0,1,2}`. Decoding `0x00002200 / HI=1`
  (the final draw config): Position = **Direct**, Normal = NotPresent,
  Color0 = **Direct**, plus one texcoord present — i.e. per-vertex position +
  color (+ tex) with **indexed** variants also used earlier (`Index8/Index16`
  appear in other VCD values, matching the `ARRAY_BASE/STRIDE` loads below).
- `VAT_A[0/1] 0x70` = `0x50E00C09 / 0x50F76C09 / 0x5AE00047 / 0x5AE16047 /
  0x5CE16047`, `VAT_B[*] 0x80` = `0x80000000` (+ `0xB9DCEE77`),
  `VAT_C[0/1] 0x90` = `0x00000000 / 0x3B9DCEE7`. These define pos/color/tex
  element counts, formats and frac shifts per VAT index.
- `MATINDEX_A 0x30` / `MATINDEX_B 0x40` — position/tex matrix indices (mirrored
  into XF `SETMATRIXINDA/B`).
- `ARRAY_BASE 0xA0/A1/A4` = `0x00AD5D80 / 0x00AD6B00 / 0x00AD6B20`
  (and `…0x00AE…`), `ARRAY_STRIDE 0xB0/B1/B4` = `0x0C / 0x06 / 0x04`. Vertex
  attribute arrays live in the **same guest-RAM region as the display lists**
  → indexed attributes are fetched from guest RAM, not from the FIFO stream.

### XF registers

- **Matrices:** `POSMATRIX 0x0000+` (51 base writes + rows), `NORMALMATRIX
  0x0400+`, `POSTMATRIX 0x05F4`, `LIGHT 0x0600`.
- **Channel config:** `SETNUMCHAN 0x1009` (51), `SETCHAN0/1_AMB/MAT/COLOR/ALPHA`
  0x100a-0x1011.
- **Vertex spec:** `INVTXSPEC 0x1008` = `0x00000011`, `DUALTEX 0x1012`,
  `SETNUMTEXGENS 0x103f` (51), `SETTEXMTXINFO 0x1040-0x1047`,
  `SETPOSTMTXINFO 0x1050-0x1057`, `SETMATRIXINDA/B 0x1018/0x1019`.
- **Viewport `SETVIEWPORT 0x101a`** (3 writes, 6 floats each), e.g.
  `[320, -240, 1.67772e7, 660, 579.5, 1.67772e7]` and `[296, -224, …, 636,
  563.5, …]` — a ~640×480 viewport (scale x=320→width 640; z range = 2^24).
- **Projection `SETPROJECTION 0x1020`** (2 writes, 6 coeffs + type word):
  `[0.00340136, 0.00680272, 0.00446429, -0, -0.000100503, -1.00503]` with
  **type word = 1 → ORTHOGRAPHIC**. Coeff[0]=2/w with w≈588, coeff[1]=2/h.
  This is a **2-D orthographic projection** — consistent with a menu that draws
  screen-space textured quads (the logo/text), not a 3-D scene.

### Draw calls

**Zero draw commands in the gather-pipe stream.** Every primitive is emitted
inside a display list (see CALL_DL). The decoder's vertex-size machinery
(VCD+VAT → byte size, transcribed from `VertexLoaderBase::GetVertexSize` +
the `VertexLoader_*` tables) is implemented and ready for when the interpreter
walks display-list contents.

### Display lists (CALL_DL)

44 calls, all 32-byte-aligned addr + size, targets in guest physical
`0x00AD7520`–`0x00AD81xx` (guest `0x80AD…`), sizes `0x20`–`0x2E0`.
**The recorder captured only the gather pipe; DL contents are in guest RAM and
are NOT in this capture.** → **Follow-up required:** to inventory the actual
draws, the interpreter (or a follow-up probe) must read each DL's bytes from
guest RAM at these addresses and decode them with the same walker.

## (c) EFB-copy cluster analysis

7 `TRIGGER_EFB_COPY` (BP 0x52) events. Cluster registers latched at each
trigger (decoded via `UPE_Copy` / `X10Y10` / `copyTexDest<<5` /
`clearcolor = (AR<<16)|GB`):

| # | pc | src (x,y) | w×h | dest_addr | stride | clear ARGB | clear_Z | trigger | copy_to_xfb | clear |
|--:|---|---|---|---|---|---|---|---|:--:|:--:|
| 1 | 0x8134BA40 | 0,0 | 592×226 | `0x800040` | 0x4A0 | `0xFF000000` | 0xFFFFFF | 0x4803 | yes | yes |
| 2 | 0x8134B930 | 0,0 | 592×226 | `0x800040` | 0x4A0 | `0xFF000000` | 0xFFFFFF | 0x4803 | yes | yes |
| 3 | 0x8134BA18 | 0,0 | 592×226 | **`0x881860`** | 0x4A0 | `0xFF000000` | 0xFFFFFF | 0x4803 | yes | yes |
| 4 | 0x8134BA34 | 0,0 | 592×226 | **`0x881860`** | 0x4A0 | `0xFF000000` | 0xFFFFFF | 0x4803 | yes | yes |
| 5 | 0x8134E6AC | 0,0 | 592×226 | **`0x881860`** | 0x4A0 | `0xFF000000` | 0xFFFFFF | 0x4803 | yes | yes |
| 6 | 0x8134B940 | 0,2 | 592×224 | `0x8C2460` | 0x4A0 | `0xFF000000` | 0xFFFFFF | 0x4802 | yes | yes |
| 7 | 0x8134BA40 | 0,0 | 592×2  | `0x145E6E0`| 0x4A0 | `0xFF000000` | 0xFFFFFF | 0x4803 | yes | yes |

Observations:

- **Clear color = `0xFF000000` (black, opaque)** and **clear Z = `0xFFFFFF`** at
  every copy trigger. (`CLEAR_AR 0x4F` also carries `0x00FF40` and `CLEAR_GB
  0x50` `0x004040` at other points — a dark-grey `0xFF404040` — but those are
  *not* the values latched at any of the 7 copy triggers; every triggered copy
  clears to black.)
- **`copy_to_xfb=1` and `clear=1` on all 7** (trigger `0x4803` = bits 0,1
  clamp_top/bottom + bit11 clear + bit14 copy_to_xfb; `0x4802` drops clamp_top).
  `target_pixel_format=0`, `intensity_fmt=0`, `auto_conv=0`, `half_scale=0`,
  `scale_invert=0`, `COPYYSCALE 0x4E = 0x0100` (yscale 256/256 = 1.0).
- **dest_stride = `0x4A0` = 1184 bytes = 592 px × 2 bytes/px** → XFB is a 592-wide
  16-bit (YUYV) framebuffer.
- **Copies #3/#4/#5 target `0x881860`, which is exactly the VI XFB base**
  (`GX_PLAN`/vi.c). These are the frames VI scans out. Copies #1/#2 (`0x800040`),
  #6 (`0x8C2460`), #7 (`0x145E6E0`) target other buffers (double-buffered /
  intermediate XFBs / partial strip).
- This is a straight **copy-EFB→XFB-then-clear-EFB-to-black** loop. With no
  rasterizer, the EFB holds only the clear color, so the XFB at `0x881860`
  receives a solid black (defined) frame — the correct output until draws exist.

**This defines the copy-clear behavior the interpreter must implement first:**
model an EFB initialized to the clear color; on `TRIGGER_EFB_COPY` with
`copy_to_xfb=1`, convert the EFB region (w×h from `EFB_TL`/`EFB_WH`) to XFB
format and write it to guest RAM at `copyTexDest<<5`; if `clear=1`, reset the
EFB to `clearcolorAR/GB` and `clearZValue`.

## (d) What the menu waits on

- **`SETDRAWDONE` (BP 0x45) appears exactly once**, at offset 22681 (the very
  end of the stream), value `0x000002` (**bit1 set = end-of-list / request
  draw-done / PE_FINISH**).
- **`PE_TOKEN_ID` (BP 0x47): 0 writes. `PE_TOKEN_INT_ID` (BP 0x48): 0 writes.**
  The menu uses no PE token handshake, only the single draw-done marker —
  matching `GX_PLAN.md`: PE_TOKEN (0x200) / PE_FINISH (0x400) stay masked in PI
  INTMR; the menu enables only CP (0x800) at the `0x1FC→0x9FC` step.

**Interpretation:** the menu emitted a full frame (state + display-list calls +
EFB→XFB copies) and terminated it with one `SETDRAWDONE`, then stopped after 709
bursts. It is waiting for the **GPU/FIFO to be consumed** — the CP read pointer
to advance (draining `CPReadWriteDistance`) and/or the `SETDRAWDONE` token to be
processed so PE signals draw-done. With **no FIFO consumer** in the runtime, the
read pointer never advances and the token is never processed, so the menu blocks
before emitting the next frame. The consumer (the FIFO interpreter that processes
commands up to `SETDRAWDONE`) is exactly what M2 is due to add — this inventory
scopes it.

## (e) Scoping verdict — minimal interpreter subset

### (i) Copy-clear only (defined-color XFB) — smallest step to a real frame

The interpreter must **parse the gather-pipe stream** (all opcodes decode
cleanly here) but only *act* on the blit cluster:

- **Opcodes to walk (structure/skip):** `NOP`, `LOAD_CP_REG`, `LOAD_XF_REG`,
  `LOAD_BP_REG`, `LOAD_INDX_A-D`, `CALL_DL` (skip contents), `INVL_VC`,
  `UNKNOWN_METRICS`, draw `0x80–0xBF` (skip payload — needs CP VCD/VAT sizing,
  already implemented). Correct payload sizing is mandatory to stay in sync.
- **Registers to *implement*:** BP `EFB_TL 0x49`, `EFB_WH 0x4A`, `EFB_ADDR 0x4B`
  (copyTexDest), `EFB_STRIDE 0x4D`, `COPYYSCALE 0x4E`, `CLEAR_AR 0x4F`,
  `CLEAR_GB 0x50`, `CLEAR_Z 0x51`, `TRIGGER_EFB_COPY 0x52`; plus `ZMODE 0x40`
  and `BLENDMODE 0x41` (only their color/alpha/z update-enable bits gate the
  clear). Model an EFB buffer, do the EFB→XFB conversion + clear on trigger.
- **Result:** a correctly-colored (black) XFB written to `0x881860`, and — once
  the read pointer advances past `SETDRAWDONE` — the menu should proceed to the
  next frame. This is the minimal path from the current all-black stall to a
  faithful, defined-color presented frame *and* to unblocking menu progression.

### (ii) Full logo render (rasterization) — the whole GX pipeline

Requires everything in (i) **plus following `CALL_DL` into guest RAM** (the 44
display lists hold all geometry — 0 draws are in the gather pipe), and then:

- **CP:** full VCD/VAT decode + `ARRAY_BASE/STRIDE` indexed-attribute fetch from
  guest RAM (VCD uses Direct *and* Index8/Index16 attributes).
- **XF transform:** position/normal/tex matrices, `SETVIEWPORT`, the
  orthographic `SETPROJECTION`, texgen (`SETNUMTEXGENS`, `SETTEXMTXINFO`,
  `SETPOSTMTXINFO`), channel/lighting config.
- **Rasterizer + TEV/texture pipeline:** `GENMODE`, scissor, `TX_SETIMAGE/MODE`
  + TMEM texture load, `TEV_COLOR/ALPHA_ENV`, `TEV_KSEL`, `TEV_REGISTER`,
  indirect-texture (`IREF/TREF/IND_*`), `ALPHACOMPARE`, `ZMODE`, `BLENDMODE`.
- Draw primitives (`0x80–0xBF`) with the vertex loader; then the same EFB→XFB
  copy path from (i) presents the rendered EFB.

That is essentially the complete GX raster pipeline. **Recommended sequencing:**
ship (i) first — it is a genuine, oracle-checkable milestone (defined-color XFB +
FIFO consumer that advances the read pointer past `SETDRAWDONE`), and it is the
prerequisite for (ii) regardless.
