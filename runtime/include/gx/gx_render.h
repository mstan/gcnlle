/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * API-neutral GX render execution boundary.  FIFO decoding and authoritative
 * CP/XF/BP state remain in gx.c; implementations receive immutable draw/copy
 * inputs in guest order.  Software is the correctness baseline and fallback.
 */
#ifndef GCN_GX_GX_RENDER_H
#define GCN_GX_GX_RENDER_H

#include "gx/gx_raster.h"

void gx_render_init(CPUState* cpu, const u32* bp, const u32* xf);
void gx_render_shutdown(void);

void gx_render_draw(const GxCpState* cp, u32 prim, u32 vat,
                    const u8* verts, u32 nverts, u32 vstride);
void gx_render_efb_copy(const GxCpState* cp);
void gx_render_flush(void);

/* Diagnostic identity only; never use this to infer acceleration. */
const char* gx_render_backend_name(void);

#endif /* GCN_GX_GX_RENDER_H */
