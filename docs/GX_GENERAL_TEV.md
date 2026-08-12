# General TEV program (exact-integer ubershader) — implementation spec

Status: **implemented historical specification** (2026-08-03). Program 31 is
present in the current runtime: phase 1a is the default level 2, while exact
fog+CMPR level 1 remains opt-in. Current measurements and next-step decisions
are in `../ISSUES.md`; the survey below records the design basis and must not be
treated as a current bottleneck ranking. Survey basis: full read of gx_raster.c
software general path + gx_vulkan.c/gx_draw_f.comp fused infrastructure
(citations verified 2026-08-03 at commit 00337d4).

## Why

Post page-CRC-memo census (00337d4, headed WW route): GX = 43.1% of
dispatch wall; inside GX, software triangle raster = 93.7% of draw time,
34.9M pixels/route, ~539K synchronized fallback draws — 100% of them
`fused_program == 0` ("general"). Top-5 general configs = 84% of those
pixels; per-shape folding (programs A..AD) has hit diminishing returns.
Dolphin proves the whole TEV state machine fits one integer shader
(`UberShaderPixel.cpp`); our version targets OUR software rasterizer
(`tev_draw`), inside our own compute raster, so none of Dolphin's
fixed-function-GPU caveats apply. Arithmetic contract: `tev_draw` and its
helpers, bit-exact, per pixel.

## Shape (from the audited survey)

- **New program id `31` (GENERAL)**. `compute_program_id`
  (gx_raster.c:3994-4026) falls through to 31 instead of 0 IFF the draw
  passes the general-eligibility gate (below); ineligible draws keep id 0
  and today's synchronized software fallback, loudly, unchanged.
  `fused_program == 0` keeps its exact current meaning everywhere (corun
  tile bit 0, census, gating) — no semantics change for existing ids.
  Raise `GX_VK_DRAW_PROGRAM_COUNT` to 31 (gx_vulkan.c:34); the existing
  `resident_record_draw`/`submit_fused_draw`/`snapshot_fused_draw`/
  shadow-validate plumbing treats ids generically (verified at
  gx_vulkan.c:667, 927, 1421, 1719, 1915-1947) so no new harness code.
- **Grow the draw packet** from 128 to 256 u32 words (`DRAW_PACKET_BYTES`
  512→1024). The SSBO is a raw `uint[]` with a hand-enforced stride (no
  std430 struct penalty); update the `_Static_assert`s in
  `snapshot_fused_draw`, and every hardcoded `128u` stride in
  gx_vulkan.c + gx_draw_f.comp (tiled-batch addressing in `main()`).
  Words 0..111 keep their EXACT current layout (existing programs
  untouched); the general block lives in words 112..255.
- **Pack raw BP register windows, not pre-decoded fields.** The shader
  re-derives bitfields with a line-for-line port of the CPU extraction
  (`color_arg`/`alpha_arg`/`draw_color_regular`/`draw_alpha_regular`/
  `draw_color_compare`/`draw_alpha_compare`/`konst_lookup`/`swap_table`/
  `alpha_test`/`BlendTev`/`LogicBlend`/`Dither`/`ZCompare`,
  gx_raster.c:956-1822). This is the most auditable path to bit-exactness:
  every GLSL function cites the C function it transcribes.

### General block layout (words 112..255)

| words | content |
|---|---|
| 112 | genmode counts: numtevstages(4) \| numtexgens(4)<<4 \| numcolchans(3)<<8 |
| 113..144 | raw `cc`,`ac` per stage (16×2) — BP 0xC0..0xDF |
| 145..152 | TRef order words BP 0x28..0x2F (ras/tex select per stage pair) |
| 153..160 | swap-table source words BP 0xF6..0xFD (kcsel/kasel bits included) |
| 161..176 | `tev_reg[4][4]` full register file incl. Prev and Reg3, RGBA |
| 177..192 | `Konst[4][4]` (konst registers; shader runs `konst_lookup`) |
| 193 | raw alpha-test word BP 0xF3 |
| 194..197 | raw ZMode BP 0x40, BlendMode BP 0x41, dest-alpha BP 0x42, PEControl BP 0x43 |
| 198..225 | `color[1]` slopes (4× GxRasterSlope, 7 words each) — second Gouraud channel |
| 226..246 | `tex[1]` slopes (3× GxRasterSlope) — second texgen |
| 247..255 | second texture metadata block (format/dims/wrap/offset), mirroring words 79-90's schema, for the stage(s) sampling texmap≠0 |

(Exact offsets may shift during implementation; the invariant is: words
0..111 frozen, one documented table in the packer comment, shader `jw()`
indices match it, `_Static_assert` on the C struct that mirrors it.)

### Eligibility gate (must EXACTLY mirror shader capability)

Eligible for id 31 only if ALL hold:
- no indirect texturing: every enabled stage's `tevind` (BP 0x10+stage)
  demands the identity/no-op path `tev_indirect_coord` short-circuits, and
  no stage uses ras colorchan 5/6 (bump alpha);
- fog disabled: `fsel == 0` (BP 0xF1 bits 21-23);
- no z-texture: BP 0xF4/0xF5 op = disabled;
- `numtexgens <= 2`, and every SAMPLED texmap resolves through the
  resident texture gate: format ∈ supported set, no mip
  (`resolve_fused_texture` rules, gx_vulkan.c:998-1038, extended to two
  units);
- pixel format supported by the existing GPU EFB model (same rule as
  programs 1..30).

Everything else → id 0 → today's loud synchronized fallback. The gate and
the shader MUST be extended in lockstep; a gate that admits what the
shader cannot do bit-exactly is a correctness bug, not a perf bug.

### Measure-first (implementer's step 1)

Extend `GCN_GX_TEV_CENSUS` (gx_raster.c ~229) to also record, per prog=0
config: fog fsel, ztex enable, indirect-active, texture format(s) of
sampled units, mip on/off, texgen count. One headed route run
(`GCN_MAX_BLOCKS=110000000`) then tells us exactly what fraction of the
34.9M px phase 1 covers and which texture formats must be added. If CMPR
(fmt 14) carries major pixel weight, add a GPU CMPR decoder to
`decode_texture` (DXT1-style, integer, transcribed from `decode_texel`
gx_raster.c:1176-1329) in the same phase; same for RGB565/RGB5A3/IA8 if
they appear. CI/palette formats need a TLUT upload path — defer unless
the census says otherwise.

### Measured verdict (census extension, 2026-08-03, route @3b58247)

The eligibility census reshaped the phasing:

- Phase-1 gate (no indirect/fog/ztex/mip, ≤2 texgens) covers **78.00% of
  prog=0 pixels but only 4,362 of ~565K prog=0 draws (0.77%)** — the
  eligible draws are the giant fills. The per-draw sync burden lives
  almost entirely in **fog draws (560,658 draws, 21.12% of pixels)**.
- **All CMPR pixel weight is fog-gated** (19.07% of prog=0 px, 0% of it
  phase-1-eligible): a CMPR decoder buys nothing until fog ships.
- Palette format C8 is the 4th-largest eligible format (6.63%) — TLUT
  upload is IN phase 1a, not deferred.
- Formats in eligible buckets: {RGBA8, IA8, I4, C8, IA4, RGB5A3, C4, I8}
  = the full 78% (RGBA8+IA8+I4+C8 alone = 75.13%).
- ztex: 0.88% (one draw); indirect: 0 px; texgens>2: ~0%; mip: 2.41%.

### Phasing (revised on the measurement)

- **Phase 1a** (pixels): general combiner/alpha-test/blend/logic/dither/
  ztest, ≤2 texgens, formats {RGBA8, IA8, I4, C8, IA4, RGB5A3, C4, I8}
  with a TLUT upload path for C4/C8. Target: 78% of software pixels.
- **Phase 1b** (syncs): fog — a bit-exact port of `apply_fog`
  (gx_raster.c:1866-1935) incl. range-adjust table — plus a CMPR
  decoder. Target: the ~560K fog-draw syncs + remaining ~21% of pixels.
- **Phase 2+**: mip sampling, z-texture, indirect stages + bump-alpha
  ras channels, >2 texgens. Each remains a loud per-draw fallback with
  its census bucket until moved.

## Verification (all mandatory before promotion)

1. **Brute-force transcription sweeps** (scratchpad, like verify_yz_aa_ab.c
   pattern): the GLSL combiner/blend/alpha-test transcriptions compiled as
   C (share source via a small #include-able core if practical) vs
   gx_raster.c originals over the full operand domains: signed-11-bit
   Reg/Konst, 0-255 tex/ras, all bias/op/scale/clamp/dest, both compare
   modes, all 16 logic ops, all 8 blend factors both directions. Zero
   mismatches required.
2. **Per-triangle differential**: `GCN_GX_VK_DRAW_VALIDATE=1
   GCN_GX_VK_DRAW_PROGRAM=31` (gx_vulkan.c:910-929 knobs +
   gx_vulkan_shadow_triangle:1716-1769) across the full headed route —
   every general draw shaded on GPU AND software, planes compared
   byte-for-byte. Zero divergences required.
3. **Corun** (`s_corun`) full-route: 0 divergent tiles.
4. **XFB hash gate**: chain `ed27f20acbdfe1d0` / 1338 publications / 1015
   frames / poison=0, general program on and off.
5. **Headed timing**: interleaved BASE,CAND,CAND,BASE at 110M blocks;
   report fps; a neutral-or-worse result is recorded as a rejection, per
   ENHANCEMENTS.md discipline.

## Follow-on phases

Phase 2: texture formats per census (CMPR/16-bit/TLUT), mip sampling.
Phase 3: indirect stages + bump-alpha ras channels, fog, z-texture,
full 8 texgens / second color channel everywhere. Each phase moves its
feature from the eligibility gate's reject list to the shader, with the
same five gates.
