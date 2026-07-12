/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * GX rasterizer — the minimal faithful software GPU that turns the IPL menu's
 * display-list geometry into EFB pixels, and presents them via the EFB->XFB
 * copy. It is a scoped transcription of Dolphin's Software video backend (our
 * independent oracle); every routine cites the file it is transcribed from and
 * everything the IPL frame does NOT exercise traps loudly (see gx_raster.c).
 *
 * Scope (docs/GX_INVENTORY.md sections e+f+g): TRIANGLE_FAN + QUADS, any VAT
 * index; Position/Normal/TexCoord0 Direct or Index8/16 (float/short paths);
 * Color0/Color1 vertex attributes in all 6 GX color formats (Direct/indexed)
 * with Dolphin's missing-color routing; XF transform (ortho + perspective) with
 * lit color channels and vertex-material color/alpha; multi-stage regular TEV;
 * texture sampling from MEM1 in I4/I8/IA4/IA8/RGB565/RGB5A3/RGBA8/CMPR;
 * RGB8_Z24 / RGBA6_Z24 / RGB565_Z16 EFB formats; z-buffered edge-function
 * rasterizer; EFB->XFB YUY2 encode. Everything else (paletted textures,
 * indirect TEV, ztex, fog, mip levels, ...) traps loudly.
 *
 * Transcription map:
 *   Transform         VideoBackends/Software/TransformUnit.cpp
 *   Clip / cull       VideoBackends/Software/Clipper.cpp
 *   Primitive assembly VideoBackends/Software/SetupUnit.cpp
 *   Rasterizer        VideoBackends/Software/Rasterizer.cpp
 *   TEV               VideoBackends/Software/Tev.cpp (+ Tev.h LUTs)
 *   Texture sample    VideoBackends/Software/TextureSampler.cpp + texel decode
 *                     VideoCommon/TextureDecoder_Common.cpp:300-640
 *   EFB pixel ops     VideoBackends/Software/SWEfbInterface.cpp
 *   EFB clear         VideoBackends/Software/EfbCopy.cpp
 *   Scissor rects     VideoCommon/BPFunctions.cpp ComputeScissorRects
 *   Vertex dequant    VideoCommon/VertexLoader_{Normal,Position,TextCoord}.cpp
 *   Vertex colors     VideoCommon/VertexLoader_Color.cpp + SWVertexLoader.cpp
 *                     ParseColorAttributes:164-192 (missing-color routing)
 *   Register layouts  VideoCommon/{BPMemory,XFMemory,CPMemory}.h
 */
#ifndef GCN_GX_GX_RASTER_H
#define GCN_GX_GX_RASTER_H

#include "cpu/cpu.h"

/* CP vertex-descriptor state needed by the vertex loader + array fetch. Mirror
 * of the size-affecting subset of Dolphin CPState (CPMemory.h). Shared with
 * gx.c which owns the authoritative copy. */
typedef struct {
    u32 vtx_desc_lo;     /* TVtxDesc::Low  (VCD_LO) */
    u32 vtx_desc_hi;     /* TVtxDesc::High (VCD_HI) */
    u32 vat_g0[8];       /* UVAT_group0 per format index */
    u32 vat_g1[8];       /* UVAT_group1 */
    u32 vat_g2[8];       /* UVAT_group2 */
    u32 array_bases[16];
    u32 array_strides[16];
    u32 matrix_index_a;
    u32 matrix_index_b;
} GxCpState;

/* Bind the persistent register files + guest CPU (called once from gcn_gx_init).
 * bp is the u32[256] BP register file, xf the u32[0x1058] XF memory; both live in
 * gx.c and never move, so binding pointers once is sufficient. Also clears the
 * EFB model. */
void gx_raster_init(CPUState* cpu, const u32* bp, const u32* xf);

/* Execute one primitive draw (OpcodeDecoder RunCommand -> SWVertexLoader ->
 * TransformUnit -> Clipper -> Rasterizer -> Tev). `prim` is the GX primitive
 * index ((opcode>>3)&7; 4 = TRIANGLE_FAN), `vat` the VAT index (opcode&7).
 * `verts` points at nverts contiguous vertices of `vstride` bytes. */
void gx_raster_draw(const GxCpState* cp, u32 prim, u32 vat,
                    const u8* verts, u32 nverts, u32 vstride);

/* Handle BPMEM_TRIGGER_EFB_COPY (BPStructs.cpp:240-395): encode the EFB source
 * rect to the XFB as YUY2 (if copy_to_xfb) and/or clear the EFB (if clear),
 * preserving Dolphin's copy-then-clear order. */
void gx_raster_efb_copy(const GxCpState* cp);

/* GCN_GX_STATS=1 (same knob gx.c reads): gx_raster_draw's own internal time
 * split between vertex load+transform+clip and triangle scan/pixel, plus a
 * pixels_shaded counter (tev_draw invocations). gx.c calls this at the same
 * 2^20-tick cadence as its own "[gx-stats]" summary and prints a
 * "[gx-draw-stats]" line alongside it. All outputs are cumulative since
 * process start (never reset), matching gx.c's own s_gx_tsc/s_gx_* counters;
 * *draw_calls == 0 means the stat gate was never turned on. Any output
 * pointer may be NULL. */
void gx_raster_get_draw_stats(u64* tsc_vtx, u64* tsc_tri, u64* pixels_shaded, u64* draw_calls);

/* GCN_GX_PIXEL_STATS=1 (own knob, separate from GCN_GX_STATS — see gx_raster.c
 * for why they must not share a cache/branch). Splits the triangle-scan wall
 * time (GCN_GX_STATS' own "tri" bucket) five ways: BLOCK (build_block per-2x2
 * setup), SLOPE (raster_pixel's pre-tev_draw work: z slope + early-Z + color
 * slopes + UV fixed-point), TEX (tex_sample total), COMB (the rest of
 * tev_draw: combiner stages + alpha test), BLEND (late-Z, if it lives after
 * alpha test, + BlendTev). All tsc_* fields and counters are cumulative since
 * process start; all zero if the knob was never turned on. */
/* texel_cache_hits/misses: the per-draw direct-mapped decode_texel cache (see
 * gx_raster.c's "Per-draw texel cache" comment, right before tex_sample) —
 * every decode_texel call tex_sample makes (4 bilinear taps + 1 point tap)
 * either hits an already-decoded (texmap,iS,iT) or misses and decodes+fills
 * it. Always active (the cache is exact, not a heuristic — no separate env
 * knob), but the hit/miss tally itself is only counted under this same
 * GCN_GX_PIXEL_STATS gate as everything else here, so it costs nothing when
 * the knob is off. hits+misses == tex_calls*4 (linear) + tex_calls*1 (point)
 * i.e. == tex_linear*4 + tex_point, since every decode_texel call site is
 * counted exactly once each. */
typedef struct {
    u64 tsc_block, tsc_slope, tsc_tex, tsc_comb, tsc_blend;
    u64 tex_calls, tex_linear, tex_point;
    u64 earlyz_rejected, shaded, blend_writes;
    u64 texel_cache_hits, texel_cache_misses;
} GxPixelStats;

void gx_raster_get_pixel_stats(GxPixelStats* out);

/* GCN_GX_TEV_CENSUS=1: print the distinct-shading-config census (draws +
 * shaded pixels per config) — decides fused-path specialization targets.
 * No-op unless the knob is on. Called from gx.c's shared stats cadence. */
void gx_raster_print_census(void);

#endif /* GCN_GX_GX_RASTER_H */
