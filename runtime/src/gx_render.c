/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gx/gx_render.h"
#include "gx/gx_vulkan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    GX_RENDER_SOFTWARE = 0,
    GX_RENDER_VULKAN_SHADOW,
    GX_RENDER_VULKAN_RESIDENT
} GxRenderMode;

static GxRenderMode s_mode;
static int s_initialized;
static const u32* s_bp;
static CPUState* s_cpu;

static int vulkan_triangle_sink(void* user, const GxRasterTriangleJob* job,
                                int after_software) {
    (void)user;
    if (s_mode == GX_RENDER_VULKAN_RESIDENT) {
        int result = gx_vulkan_resident_triangle(job, after_software);
        if (result < 0) {
            fprintf(stderr, "gx_render: Vulkan resident triangle failed\n");
            abort();
        }
        return result;
    }
    if (!gx_vulkan_shadow_triangle(job, after_software)) {
        fprintf(stderr, "gx_render: Vulkan triangle snapshot rejected\n");
        abort();
    }
    return 0;
}

void gx_render_init(CPUState* cpu, const u32* bp, const u32* xf) {
    /* Software always initializes first and remains authoritative. */
    gx_raster_init(cpu, bp, xf);
    s_cpu = cpu;
    s_bp = bp;
    s_mode = GX_RENDER_SOFTWARE;
    s_initialized = 1;

    const char* requested = getenv("GCN_GX_BACKEND");
    /* Interactive use should select the already-validated resident renderer
     * without requiring a second, easy-to-miss environment variable.  Keep
     * headless/oracle runs on the byte-exact software baseline, and retain an
     * explicit `software` override for debugging and differential work. */
    const char* window = getenv("GCN_WINDOW");
    if ((!requested || !*requested) && window && *window && *window != '0')
        requested = "vulkan";
    if (!requested || !*requested || strcmp(requested, "software") == 0)
        return;

    if (strcmp(requested, "vulkan-shadow") == 0) {
        if (gx_vulkan_shadow_init()) {
            s_mode = GX_RENDER_VULKAN_SHADOW;
            gx_raster_set_triangle_sink(vulkan_triangle_sink, NULL);
            fprintf(stderr,
                "gx_render: Vulkan shadow resources active; software EFB remains authoritative\n");
        } else {
            fprintf(stderr,
                "gx_render: Vulkan shadow unavailable; using exact software renderer\n");
        }
        return;
    }

    if (strcmp(requested, "vulkan") == 0) {
        if (gx_vulkan_resident_init()) {
            s_mode = GX_RENDER_VULKAN_RESIDENT;
            gx_raster_set_triangle_sink(vulkan_triangle_sink, NULL);
            fprintf(stderr,
                "gx_render: exact Vulkan A--S renderer active with synchronized "
                "software fallback\n");
        } else {
            fprintf(stderr,
                "gx_render: Vulkan renderer unavailable; using exact software renderer\n");
        }
        return;
    }

    fprintf(stderr, "gx_render: unknown GCN_GX_BACKEND='%s'; using software\n", requested);
}

void gx_render_shutdown(void) {
    if (!s_initialized)
        return;
    gx_raster_print_census();
    gx_raster_set_triangle_sink(NULL, NULL);
    if (s_mode == GX_RENDER_VULKAN_SHADOW ||
        s_mode == GX_RENDER_VULKAN_RESIDENT)
        gx_vulkan_shadow_shutdown();
    s_mode = GX_RENDER_SOFTWARE;
    s_bp = NULL;
    s_cpu = NULL;
    s_initialized = 0;
}

void gx_render_draw(const GxCpState* cp, u32 prim, u32 vat,
                    const u8* verts, u32 nverts, u32 vstride) {
    /* Until a GPU draw passes copy-boundary shadow comparison, this direct
     * software call is the only authoritative implementation. */
    if ((s_mode == GX_RENDER_VULKAN_SHADOW ||
         s_mode == GX_RENDER_VULKAN_RESIDENT) &&
        (!s_cpu || !gx_vulkan_shadow_begin_draw(s_bp, s_cpu->ram,
                                                 s_cpu->ram_size))) {
        fprintf(stderr, "gx_render: Vulkan draw snapshot begin failed\n");
        abort();
    }
    gx_raster_draw(cp, prim, vat, verts, nverts, vstride);
    if ((s_mode == GX_RENDER_VULKAN_SHADOW ||
         s_mode == GX_RENDER_VULKAN_RESIDENT) &&
        !gx_vulkan_shadow_end_draw()) {
        fprintf(stderr, "gx_render: Vulkan draw snapshot end failed\n");
        abort();
    }
}

void gx_render_efb_copy(const GxCpState* cp) {
    const u32 *color = NULL, *depth = NULL;
    if (s_mode == GX_RENDER_VULKAN_RESIDENT) {
        int result = (s_bp && s_cpu) ?
            gx_vulkan_resident_efb_copy(s_bp, s_cpu->ram, s_cpu->ram_size) : -1;
        if (result > 0)
            return;
        if (result < 0) {
            fprintf(stderr, "gx_render: Vulkan resident EFB copy failed\n");
            abort();
        }
        /* result==0: the backend synchronized its resident EFB to the exact
         * software planes; execute the loud fallback below. */
    }
    if (s_mode == GX_RENDER_VULKAN_SHADOW) {
        u32 width = 0, height = 0;
        gx_raster_efb_data(&color, &depth, &width, &height);
        if (width != 640u || height != 528u || !s_bp ||
            !gx_vulkan_shadow_prepare_efb(s_bp, color, depth)) {
            fprintf(stderr, "gx_render: Vulkan EFB shadow prepare failed\n");
            abort();
        }
    }
    gx_raster_efb_copy(cp);
    if (s_mode == GX_RENDER_VULKAN_SHADOW &&
        (!s_cpu || !gx_vulkan_shadow_compare_efb(color, depth,
                                                  s_cpu->ram, s_cpu->ram_size))) {
        fprintf(stderr, "gx_render: Vulkan/software EFB/XFB differential mismatch\n");
        abort();
    }
}

void gx_render_flush(void) {
    if (s_mode == GX_RENDER_VULKAN_RESIDENT &&
        !gx_vulkan_resident_flush()) {
        fprintf(stderr, "gx_render: Vulkan resident flush failed\n");
        abort();
    }
}

void gx_render_sync_efb_to_software(void) {
    if (s_mode == GX_RENDER_VULKAN_RESIDENT &&
        !gx_vulkan_resident_sync_to_software()) {
        fprintf(stderr, "gx_render: resident EFB CPU-access sync failed\n");
        abort();
    }
}

const char* gx_render_backend_name(void) {
    if (s_mode == GX_RENDER_VULKAN_SHADOW)
        return "vulkan-shadow/software-authority";
    if (s_mode == GX_RENDER_VULKAN_RESIDENT)
        return "vulkan-resident/exact-A-F";
    return "software";
}
