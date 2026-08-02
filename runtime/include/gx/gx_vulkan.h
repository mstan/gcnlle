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
int gx_vulkan_resident_flush(void);
int gx_vulkan_resident_sync_to_software(void);

#endif /* GCN_GX_GX_VULKAN_H */
