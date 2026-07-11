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

# Display-list draw inventory (section (f)): resolves each CALL_DL's bytes from a
# raw MEM1 span using the CP (VCD/VAT) state live at the call site.
python tools/gx_fifo_decode.py _work/fifo_inventory.json \
       --dl _work/dl_span.bin _work/dl_manifest.json --dl-only --summary
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

## (f) Display-list draw inventory

The follow-up flagged in (e)(ii) is now done. `_work/dl_span.bin` is a raw dump
of the guest MEM1 region holding all 22 display lists the menu calls (base
physical `0x00AD7520`); `_work/dl_manifest.json` maps each DL's physical address
→ {file offset, size}. The FIFO decoder was extended (`--dl <span> <manifest>`)
to, **at every `CALL_DL`, resolve the DL's bytes from the span and walk them with
the exact same opcode/VCD/VAT machinery** used for the gather-pipe stream — and
with the **CP (VCD/VAT) state live at the call site** (the stream reloads
`VCD_LO/HI` + `VAT_*` before each `CALL_DL`, so vertex sizing is call-site-exact).
The stream walker and the DL walker are literally the same `decode_one`; only the
byte buffer and the aggregation sink differ.

### Sanity: end-to-end decode

- **44 `CALL_DL` sites, all 44 resolved (0 unresolved); the 22 unique DL
  addresses are 1:1 with the manifest.** Each unique DL is called **exactly
  twice** → the capture spans **2 frames**.
- **Every DL decodes end-to-end and consumes exactly its declared size**
  (32 B … 1664 B), across **994 draw commands** and **87392 bytes** total, with
  **zero unknown opcodes and zero truncation.** Exact byte-consumption using the
  VCD/VAT-derived vertex sizes is itself the cross-check: a wrong offset or a
  wrong vertex size would desync the walker into an unknown opcode almost
  immediately; 22/22 clean is strong evidence the decode is correct.
- **No anomalies.** All DLs also decode identically across their two calls
  (consistent call-site CP state).

### Per unique display list

`ops` are per single invocation; a trailing `NOP` pad appears in most DLs.
`fmt` = `VCD_LO/VCD_HI` (see the format table below).

| DL (phys) | size | draws | verts | tris | prim | fmt(s) |
|---|---:|---:|---:|---:|---|---|
| `0x00AD7E00` | 0x2E0 | 80 | 240 | 80 | TRI_FAN | 600/0 |
| `0x00AD7C40` | 0x1C0 | 40 | 160 | 80 | TRI_FAN | 600/0 |
| `0x00AD7B20` | 0x120 | 48 | 144 | 48 | TRI_FAN | 400/0 |
| `0x00AD7A60` | 0xC0 | 24 | 96 | 48 | TRI_FAN | 400/0 |
| `0x00AD7A00` | 0x60 | 16 | 48 | 16 | TRI_FAN | 400/0 |
| `0x00AD79A0` | 0x60 | 10 | 40 | 20 | TRI_FAN | 400/0 |
| `0x00AD7860` | 0x140 | 52 | 156 | 52 | TRI_FAN | 400/0 |
| `0x00AD7780` | 0xE0 | 28 | 112 | 56 | TRI_FAN | 400/0 |
| `0x00AD7720` | 0x60 | 12 | 36 | 12 | TRI_FAN | 400/0 |
| `0x00AD76E0` | 0x40 | 8 | 32 | 16 | TRI_FAN | 400/0 |
| `0x00AD7680` | 0x60 | 16 | 48 | 16 | TRI_FAN | 400/0 |
| `0x00AD7620` | 0x60 | 10 | 40 | 20 | TRI_FAN | 400/0 |
| `0x00AD7600` | 0x20 | 4 | 12 | 4 | TRI_FAN | 400/0 |
| `0x00AD75E0` | 0x20 | 4 | 16 | 8 | TRI_FAN | 400/0 |
| `0x00AD7580` | 0x60 | 16 | 48 | 16 | TRI_FAN | 400/0 |
| `0x00AD7520` | 0x60 | 10 | 40 | 20 | TRI_FAN | 400/0 |
| `0x00AD8100` | 0x20 | 1 | 4 | 2 | TRI_FAN | 1600/2 |
| `0x00AD8120` | 0x20 | 1 | 4 | 2 | TRI_FAN | 1600/2 |
| `0x00AD8140` | 0x20 | 1 | 4 | 2 | TRI_FAN | 1600/2 |
| `0x00AD8160` | 0x20 | 1 | 4 | 2 | TRI_FAN | 1600/2 |
| `0x00AD80E0` | 0x20 | 1 | 4 | 2 | TRI_FAN | 1600/2 |
| `0x00AEC400` | 0x680 | 114 | 432 | 204 | TRI_FAN | 1400/2 |

Three families are visible:

- **Position-only fans (`0xAD7520`–`0xAD7E00`, 16 DLs):** `Pos=Index8/16`, no
  normal, no texcoord, no per-vertex color. Untextured flat-filled triangle fans
  — the vector-drawn UI/text panels.
- **Single textured+lit quads (`0xAD80E0`–`0xAD8160`, 5 DLs):** one 4-vertex fan
  each, `Pos=Index16 + Nrm=Index8 + Tex0=Index8`. The 5 individual textured
  sprites.
- **Main textured+lit object (`0xAEC400`):** 114 fans, `Pos=Index8 + Nrm=Index8 +
  Tex0=Index8`, 204 triangles — the bulk textured mesh (the rotating cube logo).

### Union across all 22 DLs

- **Draw opcodes:** `GX_DRAW_TRIANGLE_FAN` only — **994 draws, all VAT index 0.**
  No other primitive type appears. Vertex counts are only **3** (536 draws → 1
  tri each) and **4** (458 draws → 2 tris each).
- **No BP, XF, or `LOAD_INDX` registers are written INSIDE any DL**, and no
  nested `CALL_DL`. DLs are **pure geometry** (CP `VCD/VAT`/`MATINDEX` +
  `DRAW`s); *all* pipeline/transform/texture state is set in the gather-pipe
  stream (sections (b)–(c)). This matters for the interpreter: DL execution only
  needs the vertex loader + array fetch + the transform/raster state already
  latched by the stream.

**Vertex formats actually used at draw time** (VCD decoded into named
attributes; the VAT supplies component formats):

| fmt (VCD_LO/HI) | Pos | Normal | TexCoord0 | Col0/1 | vtx bytes | draws (×2 frames) |
|---|---|---|---|---|---:|---:|
| `0x600 / 0x0` | Index16 | — | — | — | 2 | 240 |
| `0x400 / 0x0` | Index8 | — | — | — | 1 | 516 |
| `0x1600 / 0x2` | Index16 | Index8 | Index8 | — | 4 | 10 |
| `0x1400 / 0x2` | Index8 | Index8 | Index8 | — | 3 | 228 |

VAT index 0 is loaded with two alternating value sets during the frame
(`g0 = 0x50E00C09` and `0x50F76C09`); both decode to: **Position = 3×`Float`
(XYZ), Normal = 3×`Short`, TexCoord0 = 2×`Short` (ST)**, `ByteDequant=1`. (The
two sets differ only in the unused Color0/1 element/format fields — RGB565 vs
RGBA8888 — which never matter because Color is `NotPresent` in every VCD.) **No
attribute is ever `Direct`/inline: every present attribute is indexed**, so DL
payloads are tiny (1–4 bytes/vertex, just indices).

**Vertex arrays referenced by indexed attributes** (Dolphin `CPArray`
numbering) — matches the stream's `ARRAY_BASE`/`ARRAY_STRIDE` loads exactly:

| attribute | `ARRAY_BASE[i]` | stride (from stream) | element |
|---|---|---:|---|
| Position | `[0]` | `0x0C` = 12 | 3×float XYZ |
| Normal | `[1]` | `0x06` = 6 | 3×short |
| TexCoord0 | `[4]` | `0x04` = 4 | 2×short ST |

(The stream loads `ARRAY_BASE[0]=0x00AD5D80`, `[1]/[4]` in the same
`0x00AD…`/`0x00AE…` MEM1 region — i.e. the indexed vertex data lives in guest
RAM alongside the DLs, and is included in the same MEM1 span family.)

### Geometry totals

- **Per frame** (each of the 22 unique DLs once): **1720 vertices, 726
  triangles.**
- Whole capture (44 invocations, 2 frames): 3440 vertices, 1452 triangles.

### Textures referenced (from the stream's BP texture cluster)

Because texcoords are indexed and DLs carry no BP writes, texture state is
entirely in the gather-pipe stream. Decoding the texture-unit-0 BP registers:

- **`TX_SETIMAGE0` (BP 0x88):** two configs, **`352×40 I8`** and **`128×128 I8`**
  — both **I8** (8-bit intensity/luminance), i.e. monochrome. Only texture
  **unit 0** is programmed (units 1–3 untouched).
- **`TX_SETIMAGE3` (BP 0x94):** source addresses `(val<<5)` = **`0x00AD8800`**
  and **`0x00AECCE0`** — both in **MEM1**, inside the same DL/vertex-array span
  family. ⚠️ **Correction to (b):** the `0x0D8000…0x0DDC00` values noted there
  are `TX_SETIMAGE1/2` **TMEM** cache offsets, *not* MEM1 texture source
  addresses; the real MEM1 sources are these two `TX_SETIMAGE3` addresses.
- **`GENMODE` (BP 0x00) = `0x004210`/`0x004211`:** **1 TEV stage, 1 color
  channel, 0 or 1 texgens** — a single-stage TEV (texture × channel-color), the
  minimal texturing pipeline.

### Rasterizer scoping verdict — minimal GX pipeline for the logo render

Everything needed to render this menu frame, and nothing more:

1. **Vertex loader (CP):** `GX_DRAW_TRIANGLE_FAN` only, VAT 0 only, 3- and
   4-vertex fans. Support **indexed** `Position` (float XYZ), `Normal` (short),
   `TexCoord0` (short ST) — **all Index8/Index16, no Direct/inline attributes,
   no per-vertex color**. Fetch from `ARRAY_BASE[0]/[1]/[4]` in guest RAM with
   strides 12/6/4. (The generic Direct-attribute and Color paths are exercised
   *nowhere* in this frame — they can be stubbed/deferred.)
2. **XF transform:** single position matrix (VCD `PosMatIdx=0`, so no per-vertex
   matrix index), normal matrix, one tex matrix; the recorded **orthographic**
   `SETPROJECTION` (type word = 1, ~588-wide) + `SETVIEWPORT` (~640×480) from
   (b); `SETNUMTEXGENS`/`SETTEXMTXINFO`/`SETPOSTMTXINFO` texgen; 1 color channel
   (`SETNUMCHAN=1`) with material/ambient color (per-vertex color absent, so the
   fans are colored by the channel constant, and lit via the normal for the
   textured families).
3. **TEV / texture:** **1 TEV stage, 1 color channel**; **texture unit 0 only**,
   **I8** format, two textures (`352×40`, `128×128`) loaded from MEM1
   (`0x00AD8800`, `0x00AECCE0`) into TMEM; `TX_SETMODE0/1`, `SU_SSIZE/TSIZE`;
   `ALPHACOMPARE`, `ZMODE`, `BLENDMODE` from the stream. No indirect textures are
   needed for the geometry (the `IREF/TREF/IND_*` seen in (b) can wait — no draw
   consumes a second texgen/indirect stage here).
4. **Present:** the existing (e)(i) EFB→XFB copy+clear path presents the rendered
   EFB — no change.

Net: the logo frame is **flat/material-colored + single-stage-I8-textured
triangle fans under a 2-D orthographic projection**, fed by indexed vertex arrays
in guest RAM. The heavy generic-GX surface — Direct attributes, per-vertex color,
multi-stage TEV, indirect textures, non-fan primitives, VATs 1–7 — is **not**
touched by this frame and can be deferred behind a loud "unimplemented" trap
rather than built up front.
