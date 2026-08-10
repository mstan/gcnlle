/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * API-neutral GX render execution boundary.  FIFO decoding and authoritative
 * CP/XF/BP state remain in gx.c; implementations receive immutable draw/copy
 * inputs in guest order.  Software is the correctness baseline and fallback.
 */
#ifndef GCN_GX_GX_RENDER_H
#define GCN_GX_GX_RENDER_H

#include "gx/gx_raster.h"

typedef enum {
    GX_RENDER_FLUSH_DRAWDONE = 0,
    GX_RENDER_FLUSH_PE_TOKEN,
    GX_RENDER_FLUSH_PE_TOKEN_INT,
    GX_RENDER_FLUSH_PIPELINE_DRAIN,
    GX_RENDER_FLUSH_COUNT
} GxRenderFlushReason;

void gx_render_init(CPUState* cpu, const u32* bp, const u32* xf);
void gx_render_shutdown(void);

void gx_render_draw(const GxCpState* cp, u32 prim, u32 vat,
                    const u8* verts, u32 nverts, u32 vstride);
void gx_render_efb_copy(const GxCpState* cp);
void gx_render_flush(GxRenderFlushReason reason);

/* Make the software EFB planes current for a synchronous CPU peek. Software
 * and Vulkan-shadow modes are already current; resident Vulkan downloads its
 * ordered color/depth planes. */
void gx_render_sync_efb_to_software(void);

/* Diagnostic identity only; never use this to infer acceleration. */
const char* gx_render_backend_name(void);

#endif /* GCN_GX_GX_RENDER_H */
