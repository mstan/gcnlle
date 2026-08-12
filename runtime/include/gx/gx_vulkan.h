/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef GCN_GX_GX_VULKAN_H
#define GCN_GX_GX_VULKAN_H

#include "common/types.h"
#include "gx/gx_raster.h"

/* Headless Vulkan shadow resource lifecycle.  This is deliberately not a
 * renderer yet: no caller may substitute its images for the software EFB
 * until copy-boundary differential validation is implemented. */
int gx_vulkan_shadow_init(void);
int gx_vulkan_resident_init(void);
void gx_vulkan_shadow_shutdown(void);
int gx_vulkan_shadow_prepare_efb(const u32* bp, const u32* color,
                                 const u32* depth);
int gx_vulkan_shadow_compare_efb(const u32* color, const u32* depth,
                                 const u8* ram, u32 ram_size);
int gx_vulkan_shadow_begin_draw(const u32* bp, const u8* ram, u32 ram_size);
int gx_vulkan_shadow_triangle(const GxRasterTriangleJob* job,
                              int after_software);
int gx_vulkan_shadow_end_draw(void);
int gx_vulkan_resident_triangle(const GxRasterTriangleJob* job,
                                int after_software);
/* 1=handled on GPU, 0=synchronized software fallback required, -1=fatal. */
int gx_vulkan_resident_efb_copy(const u32* bp, u8* ram, u32 ram_size);
typedef enum {
    GX_VK_FLUSH_DRAWDONE = 0,
    GX_VK_FLUSH_PE_TOKEN,
    GX_VK_FLUSH_PE_TOKEN_INT,
    GX_VK_FLUSH_PIPELINE_DRAIN
} GxVkFlushReason;
int gx_vulkan_resident_flush(GxVkFlushReason reason);
int gx_vulkan_resident_sync_to_software(void);

/* SNAPSHOT_RESUME (docs/SNAPSHOT_RESUME.md) SAVE-side drain-assert: 1 iff the
 * resident backend has a batch mid-recording or submitted-but-unfenced
 * in-flight work (the same pair gx_vulkan_shadow_shutdown force-flushes
 * before teardown). Callers must gx_vulkan_resident_flush() first — this
 * only checks, it never flushes. 0 (never busy) when the resident backend
 * isn't active at all. */
int gx_vulkan_resident_busy(void);

/* beads-u2x.1 TLUT-COW corruption hunt: dumps the last `count` entries of the
 * always-on (opt-in GCN_GX_VK_TLUT_TRACE=1) TLUT/texture residency ring to
 * stderr. No-op (returns 0) if the trace wasn't enabled or nothing was
 * recorded yet. count==0 dumps everything currently held. */
int gx_vulkan_tlut_ring_dump(u32 count);

#endif /* GCN_GX_GX_VULKAN_H */
