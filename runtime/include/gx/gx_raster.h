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

#endif /* GCN_GX_GX_RASTER_H */
