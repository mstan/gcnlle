/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Headless Vulkan differential shadow for the packed software EFB. At each
 * ordered EFB-copy boundary it uploads the pre-copy color/depth planes, runs
 * independently decoded integer compute XFB encode and clear passes, reads the
 * results back, and compares them after the authoritative software copy/clear.
 */
#include "gx/gx_vulkan.h"
#include "gx/gx.h"       /* XFB RAM publication guard shared with VI */
#include "gx/gx_raster.h"      /* gx_raster_xf_word — phase 1b fog vp_wd() XF register */
#include "cpu/native_code.h"   /* content_dirty — XFB readback writes to RAM */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <x86intrin.h>

#ifdef GCN_HAVE_VULKAN
#include <vulkan/vulkan.h>

#define EFB_WIDTH       640u
#define EFB_HEIGHT      528u
#define EFB_PIXELS      (EFB_WIDTH * EFB_HEIGHT)
#define EFB_PLANE_BYTES ((VkDeviceSize)EFB_PIXELS * sizeof(u32))
#define XFB_SHADOW_BYTES ((VkDeviceSize)EFB_WIDTH * EFB_HEIGHT * 2u)
#define XFB_RING_SIZE 4u
#define XFB_SHADOW_TOTAL (XFB_SHADOW_BYTES * XFB_RING_SIZE)
#define XFB_SHADOW_OFFSET (EFB_PLANE_BYTES * 4u)
#define READBACK_XFB_OFFSET 0u
#define READBACK_COLOR_OFFSET XFB_SHADOW_TOTAL
#define READBACK_DEPTH_OFFSET (READBACK_COLOR_OFFSET + EFB_PLANE_BYTES)
/* EFB_TEX_SHADOW_BYTES is defined below (after TEXTURE_SHADOW_BYTES); the
 * readback buffer needs an equally-sized region to receive it back. */
#define READBACK_EFB_TEX_OFFSET (READBACK_DEPTH_OFFSET + EFB_PLANE_BYTES)
/* Pass D (docs/GX_GENERAL_TEV.md root-cause hunt): permanent, always-cheap
 * per-pixel debug dump for shade_pixel_general -- gated on a push-constant
 * (x,y) that defaults to (-1,-1) (never matches, so the shader's one `if`
 * compare is the only cost when off). GCN_GX_GENERAL_DEBUG_XY="x,y" arms it;
 * matching CPU-side dumps live in gx_raster.c under the same env knob. */
#define GENERAL_DEBUG_WORDS 64u
#define GENERAL_DEBUG_BYTES (GENERAL_DEBUG_WORDS * sizeof(u32))
#define READBACK_GENERAL_DEBUG_OFFSET (READBACK_EFB_TEX_OFFSET + EFB_TEX_SHADOW_BYTES)
#define READBACK_BYTES (READBACK_GENERAL_DEBUG_OFFSET + GENERAL_DEBUG_BYTES)
#define DRAW_JOB_OFFSET (XFB_SHADOW_OFFSET + XFB_SHADOW_TOTAL)
/* Phase 1a (general TEV program, docs/GX_GENERAL_TEV.md): grown from 128 to
 * 256 u32 words. Words 0..111 keep their EXACT pre-existing layout (every
 * program 1..30 reads only those); words 112..255 are the new "general
 * block", packed for every draw regardless of fused_program so the stride
 * change is exercised (and load-bearing) under the existing 30 programs
 * before anything ever classifies to the still-unused id 31. See the layout
 * table in snapshot_fused_draw's comment. */
/* Phase 1b (general TEV fog+CMPR, docs/GX_GENERAL_TEV.md): grown again from
 * 256 to 272 words to fit the raw fog BP window (0xE8-0xF2, 11 words) plus
 * the vp_wd() XF register (1 word) fog needs; words 0..255 keep their exact
 * phase-1a meaning. See the layout table in snapshot_fused_draw's comment. */
#define DRAW_PACKET_BYTES 1088u
/* Phase 1a general TEV program (docs/GX_GENERAL_TEV.md): id 31, the first
 * program compute_program_id (gx_raster.c) derives from an eligibility gate
 * rather than an exact per-shape signature match. */
#define GX_VK_DRAW_PROGRAM_COUNT 31u
#define DRAW_JOB_BYTES  (8u * 1024u * 1024u)
#define DRAW_PACKET_ARENA_BYTES (6u * 1024u * 1024u)
#define EFB_TILE_WIDTH 40u
#define EFB_TILE_HEIGHT 33u
#define EFB_TILE_COUNT (EFB_TILE_WIDTH * EFB_TILE_HEIGHT)
#define TILE_HEADER_BYTE_OFFSET DRAW_PACKET_ARENA_BYTES
#define TILE_HEADER_BYTES (EFB_TILE_COUNT * 2u * sizeof(u32))
#define TILE_INDEX_BYTE_OFFSET (TILE_HEADER_BYTE_OFFSET + TILE_HEADER_BYTES)
#define TILE_INDEX_CAPACITY ((DRAW_JOB_BYTES - TILE_INDEX_BYTE_OFFSET) / sizeof(u32))
#define TILE_JOBS_PER_TILE 1024u
#define TEXTURE_SHADOW_OFFSET (DRAW_JOB_OFFSET + DRAW_JOB_BYTES)
#define TEXTURE_SHADOW_BYTES  (4u * 1024u * 1024u)
/* EFB->texture resident copy shadow (RG8/RGBA8, half_scale): worst case is
 * half_scale RGBA8 across the whole EFB (320x264 out, 64-byte 4x4 blocks) --
 * 80*66*64 = 337,920 bytes; round up and give it a small ring so a handful of
 * copies can be in flight before the frame fence, same shape as XFB_RING_SIZE. */
#define EFB_TEX_RING_SIZE 4u
#define EFB_TEX_SHADOW_SLOT_BYTES (384u * 1024u)
#define EFB_TEX_SHADOW_BYTES (EFB_TEX_SHADOW_SLOT_BYTES * EFB_TEX_RING_SIZE)
#define EFB_TEX_SHADOW_OFFSET (TEXTURE_SHADOW_OFFSET + TEXTURE_SHADOW_BYTES)
/* Phase 1a general TEV program (docs/GX_GENERAL_TEV.md), pass C: TLUT bytes
 * for C4/C8 palette decode (gx_draw_f.comp's TlutData SSBO, binding=6 --
 * binding=5 is already EFB_TEX_SHADOW, so pass B's dead-code binding=5
 * declaration was wrong and is corrected alongside this). GX TLUTs are at
 * most 16384 entries * 2 bytes = 32KiB (C14X2, out of phase-1a scope); C4
 * needs <=32 bytes, C8 <=512 -- 64KiB total budget covers many distinct
 * small TLUTs with room to spare. */
#define TLUT_SHADOW_BYTES (64u * 1024u)
#define TLUT_SHADOW_OFFSET (EFB_TEX_SHADOW_OFFSET + EFB_TEX_SHADOW_BYTES)
#define STAGING_BYTES   (TLUT_SHADOW_OFFSET + TLUT_SHADOW_BYTES)
#define GPU_QUERY_MAX 16u

enum { GPU_TIME_DRAW = 0, GPU_TIME_XFB, GPU_TIME_CLEAR, GPU_TIME_COUNT };

static const u32 s_clear_spv[] =
#include "gx_clear.comp.inc"
;
static const u32 s_xfb_copy_spv[] =
#include "gx_xfb_copy.comp.inc"
;
static const u32 s_xfb_subgroup_spv[] =
#include "gx_xfb_subgroup.comp.inc"
;
static const u32 s_draw_f_spv[] =
#include "gx_draw_f.comp.inc"
;
static const u32 s_efb_tex_copy_spv[] =
#include "gx_efb_tex_copy.comp.inc"
;

typedef struct {
    u32 left, top, right, bottom;
    u32 clear_enable;
    u32 pixel_format;
    u32 color_update;
    u32 alpha_update;
    u32 depth_update;
    u32 clear_color;
    u32 clear_depth;
} GxVkClearPush;

/* Five uvec4s, matching gx_xfb_copy.comp's push layout exactly. */
typedef struct {
    u32 left, top, right, bottom;
    u32 src_width, dst_height, yscale_reg, scale_invert;
    u32 clamp_top, clamp_bottom, pixel_format, copy_enable;
    u32 w0, w1, w2, w3;
    u32 w4, w5, w6, output_words_per_row;
    u32 output_word_base, output_pad0, output_pad1, output_pad2;
} GxVkCopyPush;

typedef struct {
    u8* ram;
    u32 ram_size;
    u32 address, stride, width, height, gpu_stride;
    VkDeviceSize slot_offset;
} GxVkPendingXfb;

/* Six uvec4s, matching gx_efb_tex_copy.comp's push layout exactly. Reachable
 * only for the two exact bit-verified copy words (see decode_efb_tex_copy /
 * gx_vulkan_resident_efb_tex_copy) -- fmt is always 11 (RG8) or 6 (RGBA8),
 * half_scale is always 1, clamp_top/clamp_bottom are always 1. is_rgba6 and
 * is_depth come from the separate, runtime pixel_format BP state (bp[0x43]),
 * not from the copy word, so they are decoded fresh every call. */
typedef struct {
    u32 left, top, right, bottom;
    u32 out_w, out_h, tiles_x, tiles_y;
    u32 clamp_top, clamp_bottom, half_scale, is_depth;
    u32 is_rgba6, fmt, reserved0, block_bytes;
    u32 w0, w1, w2, w3;
    u32 w4, w5, w6, output_word_base;
} GxVkTexCopyPush;

typedef struct {
    u8* ram;
    u32 ram_size;
    u32 dest_addr, dest_stride;
    u32 tiles_x, tiles_y, block_bytes;
    u32 row_bytes;      /* tiles_x * block_bytes: valid bytes per output row,
                          * which may be less than dest_stride (padding). */
    VkDeviceSize slot_offset;
} GxVkPendingTexCopy;

typedef struct {
    u32 word_base;
    u32 batch_mode;
    u32 tile_header_base;
    u32 tile_index_base;
    /* Pass D debug hook (docs/GX_GENERAL_TEV.md): -1,-1 (default) never
     * matches any real pixel, so this is a single dead compare per
     * invocation when unarmed. See GENERAL_DEBUG_* above. */
    int32_t dbg_x;
    int32_t dbg_y;
} GxVkDrawPush;

typedef struct {
    int used;
    u32 address, format, width, height, length;
    u8* bytes;
    u32 gpu_offset, gpu_capacity;
    int gpu_dirty;
    u64 stamp;
} GxVkTextureEntry;

#define GX_VK_TEXTURE_CACHE_ENTRIES 32u

/* Phase 1a general TEV program (docs/GX_GENERAL_TEV.md): TLUT bytes for
 * C4/C8, mirroring GxVkTextureEntry's content-compare/upload shape but keyed
 * by TMEM byte offset (from TX_SETTLUT, gx_raster.c:5741-5743) instead of
 * guest-RAM address, and read from gcn_gx_tmem() instead of guest RAM. */
typedef struct {
    int used;
    u32 tmem_offset, length;
    u8* bytes;
    u32 gpu_offset, gpu_capacity;
    int gpu_dirty;
    u64 stamp;
} GxVkTlutEntry;

#define GX_VK_TLUT_CACHE_ENTRIES 8u

/* Phase 1a general TEV program (docs/GX_GENERAL_TEV.md): the two texture
 * "slots" the general shader supports (shade_pixel_general's texmap==0 ->
 * slot 0 / words 79-90, texmap!=0 -> slot 1 / words 239-250 convention).
 * Every program 1..30 only ever needs slot 0 (tex[0] aliases the single
 * `texture_out` those programs' callers already use); program 31 can use
 * both. */
typedef struct {
    GxVkTextureEntry* tex[2];
    GxVkTlutEntry* tlut[2];
    u32 unit[2];   /* the actual texmap/unit index each slot resolved, in
                    * first-enabled-stage order (see resolve_fused_texture's
                    * program-31 branch) -- snapshot_fused_draw needs this to
                    * read the right unit's wrap/mip/lod-bias BP fields,
                    * since general draws are not guaranteed to sample
                    * texmap 0 at all, let alone at stage 0. */
} GxVkGeneralTex;

typedef struct {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    VkFence resident_fence;
    u32 queue_family;
    int subgroup_xfb;
    VkCommandPool command_pool;
    VkCommandBuffer command_buffers[XFB_RING_SIZE];
    VkCommandBuffer command_buffer;
    VkQueryPool query_pool;
    VkImage image[2];
    VkDeviceMemory memory[2];
    VkImageView view[2];
    VkBuffer staging;
    VkDeviceMemory staging_memory;
    u8* staging_map;
    int staging_coherent;
    int staging_cached;
    VkBuffer readback;
    VkDeviceMemory readback_memory;
    u8* readback_map;
    int readback_coherent;
    int readback_cached;
    VkDescriptorSetLayout descriptor_layout;
    VkDescriptorPool descriptor_pool;
    VkDescriptorSet descriptor_set;
    VkPipelineLayout clear_pipeline_layout;
    VkPipeline clear_pipeline;
    VkPipelineLayout copy_pipeline_layout;
    VkPipeline copy_pipeline;
    VkPipelineLayout draw_f_pipeline_layout;
    VkPipeline draw_f_pipeline;
    VkPipelineLayout efb_tex_copy_pipeline_layout;
    VkPipeline efb_tex_copy_pipeline;
    int images_general;
    int prepared;
    int xfb_present;
    u32 xfb_address;
    u32 xfb_stride;
    u32 xfb_width;
    u32 xfb_height;
    u32 xfb_output_words_per_row;
    u32 draw_bp[256];
    const u8* draw_ram;
    u32 draw_ram_size;
    int draw_textures_validated;
    /* Pass D general-TEV debug hook (docs/GX_GENERAL_TEV.md), GCN_GX_GENERAL_DEBUG_XY. */
    int32_t general_debug_x, general_debug_y;
    int general_debug_armed;
    int draw_active;
    u64 draws;
    u64 triangles;
    u64 fused_triangles[GX_VK_DRAW_PROGRAM_COUNT + 1u];
    u32 draw_validate_remaining[GX_VK_DRAW_PROGRAM_COUNT + 1u];
    u32 draw_validate_program; /* 0=all A--X, otherwise exact program id */
    u32 draw_validation_program;
    int draw_validation_pending;
    u64 draw_validations[GX_VK_DRAW_PROGRAM_COUNT + 1u];
    u8 texture_reject_logged[GX_VK_DRAW_PROGRAM_COUNT + 1u];
    GxVkTextureEntry texture_cache[GX_VK_TEXTURE_CACHE_ENTRIES];
    GxVkTextureEntry* last_texture_binding;
    u64 texture_stamp;
    u64 texture_hits;
    u64 texture_misses;
    u64 texture_bytes_compared;
    u64 texture_bytes_changed;
    GxVkTlutEntry tlut_cache[GX_VK_TLUT_CACHE_ENTRIES];
    u64 tlut_stamp;
    u32 tlut_arena_used;   /* bytes consumed so far in TLUT_SHADOW_BYTES */
    int resident_mode;
    int resident_recording;
    u32 resident_inflight;
    int resident_efb_valid;
    u32 resident_job_count;
    u32 resident_tile_count[EFB_TILE_COUNT];
    u32 resident_tile_jobs[EFB_TILE_COUNT][TILE_JOBS_PER_TILE];
    u32 resident_tile_indices;
    u32 resident_draw_arena_used;
    GxVkPendingXfb resident_pending[XFB_RING_SIZE];
    u32 resident_pending_count;
    GxVkPendingTexCopy resident_pending_tex[EFB_TEX_RING_SIZE];
    u32 resident_pending_tex_count;
    int efb_copy_verify;         /* GCN_GX_EFB_COPY_VERIFY=1 */
    u64 efb_copy_verify_compares;
    u32 texture_arena_used;
    u64 resident_batches;
    u64 resident_triangles;
    u64 resident_fallbacks;
    u64 resident_submit_cpu_tsc;
    u64 resident_wait_tsc;
    u64 resident_copy_total_tsc;
    u64 resident_copy_memcpy_tsc;
    u32 corun_tile_programs[EFB_TILE_COUNT]; /* 1<<program per tile since the
                                              * last corun compare (bit 0 =
                                              * general/unsupported) — names
                                              * the culprit on divergence */
    int gpu_stats;
    u32 gpu_query_count;
    u8 gpu_query_kind[GPU_QUERY_MAX / 2u];
    float timestamp_period;
    double gpu_time_ns[GPU_TIME_COUNT];
    u64 gpu_time_calls[GPU_TIME_COUNT];
    u64 comparisons;
    u64 xfb_comparisons;
} GxVkShadow;

static GxVkShadow s_vk;

static int compare_plane(const char* name, const u32* software,
                         const u32* gpu);
static u32 texture_encoded_size(u32 format, u32 width, u32 height);
static int resident_submit_batch(void);

static int resident_wait_fence(void) {
    /* Resident batches are normally tens of microseconds. Avoid a scheduler
     * sleep/wake round trip for that fast path, but bound host CPU use and
     * retain an infinite blocking wait for slow or preempted GPU work. */
    for (u32 spin = 0; spin < 8192u; ++spin) {
        VkResult result = vkGetFenceStatus(s_vk.device, s_vk.resident_fence);
        if (result == VK_SUCCESS)
            return 1;
        if (result != VK_NOT_READY)
            return 0;
        _mm_pause();
    }
    return vkWaitForFences(s_vk.device, 1, &s_vk.resident_fence,
                           VK_TRUE, UINT64_MAX) == VK_SUCCESS;
}

static u32 gpu_time_begin(u32 kind) {
    if (!s_vk.gpu_stats || !s_vk.query_pool ||
        s_vk.gpu_query_count + 2u > GPU_QUERY_MAX)
        return UINT32_MAX;
    u32 query = s_vk.gpu_query_count;
    s_vk.gpu_query_kind[query / 2u] = (u8)kind;
    vkCmdWriteTimestamp(s_vk.command_buffer,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        s_vk.query_pool, query);
    return query;
}

static void gpu_time_end(u32 query) {
    if (query == UINT32_MAX)
        return;
    vkCmdWriteTimestamp(s_vk.command_buffer,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        s_vk.query_pool, query + 1u);
    s_vk.gpu_query_count = query + 2u;
}

static u32 choose_memory_type(u32 bits, VkMemoryPropertyFlags required,
                              VkMemoryPropertyFlags preferred,
                              const VkPhysicalDeviceMemoryProperties* props) {
    for (u32 pass = 0; pass < 2; ++pass) {
        VkMemoryPropertyFlags wanted = required | (pass == 0 ? preferred : 0u);
        for (u32 i = 0; i < props->memoryTypeCount; ++i) {
            if ((bits & (1u << i)) &&
                (props->memoryTypes[i].propertyFlags & wanted) == wanted)
                return i;
        }
    }
    return UINT32_MAX;
}

static int create_efb_image(u32 index) {
    VkImageCreateInfo ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R32_UINT;
    ci.extent = (VkExtent3D){EFB_WIDTH, EFB_HEIGHT, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(s_vk.device, &ci, NULL, &s_vk.image[index]) != VK_SUCCESS)
        return 0;

    VkMemoryRequirements req;
    VkPhysicalDeviceMemoryProperties props;
    vkGetImageMemoryRequirements(s_vk.device, s_vk.image[index], &req);
    vkGetPhysicalDeviceMemoryProperties(s_vk.physical, &props);
    u32 type = choose_memory_type(req.memoryTypeBits, 0,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &props);
    if (type == UINT32_MAX)
        return 0;
    VkMemoryAllocateInfo ai = {0};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (vkAllocateMemory(s_vk.device, &ai, NULL, &s_vk.memory[index]) != VK_SUCCESS ||
        vkBindImageMemory(s_vk.device, s_vk.image[index], s_vk.memory[index], 0) !=
            VK_SUCCESS)
        return 0;

    VkImageViewCreateInfo vi = {0};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = s_vk.image[index];
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R32_UINT;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    return vkCreateImageView(s_vk.device, &vi, NULL, &s_vk.view[index]) == VK_SUCCESS;
}

static int create_staging(void) {
    VkBufferCreateInfo bi = {0};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = STAGING_BYTES;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(s_vk.device, &bi, NULL, &s_vk.staging) != VK_SUCCESS)
        return 0;

    VkMemoryRequirements req;
    VkPhysicalDeviceMemoryProperties props;
    vkGetBufferMemoryRequirements(s_vk.device, s_vk.staging, &req);
    vkGetPhysicalDeviceMemoryProperties(s_vk.physical, &props);
    u32 type = choose_memory_type(req.memoryTypeBits,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &props);
    if (type == UINT32_MAX)
        return 0;
    s_vk.staging_coherent =
        (props.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    s_vk.staging_cached =
        (props.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0;
    VkMemoryAllocateInfo ai = {0};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (vkAllocateMemory(s_vk.device, &ai, NULL, &s_vk.staging_memory) != VK_SUCCESS ||
        vkBindBufferMemory(s_vk.device, s_vk.staging, s_vk.staging_memory, 0) != VK_SUCCESS ||
        vkMapMemory(s_vk.device, s_vk.staging_memory, 0, VK_WHOLE_SIZE, 0,
                    (void**)&s_vk.staging_map) != VK_SUCCESS)
        return 0;
    return 1;
}

static int create_readback(void) {
    VkBufferCreateInfo bi = {0};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = READBACK_BYTES;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(s_vk.device, &bi, NULL, &s_vk.readback) != VK_SUCCESS)
        return 0;
    VkMemoryRequirements req;
    VkPhysicalDeviceMemoryProperties props;
    vkGetBufferMemoryRequirements(s_vk.device, s_vk.readback, &req);
    vkGetPhysicalDeviceMemoryProperties(s_vk.physical, &props);
    u32 type = choose_memory_type(req.memoryTypeBits,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                  VK_MEMORY_PROPERTY_HOST_CACHED_BIT, &props);
    if (type == UINT32_MAX)
        return 0;
    VkMemoryPropertyFlags flags = props.memoryTypes[type].propertyFlags;
    s_vk.readback_coherent =
        (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    s_vk.readback_cached =
        (flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0;
    VkMemoryAllocateInfo ai = {0};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (vkAllocateMemory(s_vk.device, &ai, NULL, &s_vk.readback_memory) != VK_SUCCESS ||
        vkBindBufferMemory(s_vk.device, s_vk.readback, s_vk.readback_memory, 0) !=
            VK_SUCCESS ||
        vkMapMemory(s_vk.device, s_vk.readback_memory, 0, VK_WHOLE_SIZE, 0,
                    (void**)&s_vk.readback_map) != VK_SUCCESS)
        return 0;
    return 1;
}

static int create_compute_state(void) {
    VkDescriptorSetLayoutBinding bindings[8] = {0};
    for (u32 i = 0; i < 2; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[4].binding = 4;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[5].binding = 5;
    bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[5].descriptorCount = 1;
    bindings[5].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    /* Phase 1a general TEV program (docs/GX_GENERAL_TEV.md): TLUT bytes for
     * C4/C8, gx_draw_f.comp's TlutData SSBO. */
    bindings[6].binding = 6;
    bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[6].descriptorCount = 1;
    bindings[6].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    /* Pass D root-cause hunt (docs/GX_GENERAL_TEV.md): permanent, always-on
     * (env-armed) per-pixel debug dump SSBO, gx_draw_f.comp's DebugData. */
    bindings[7].binding = 7;
    bindings[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[7].descriptorCount = 1;
    bindings[7].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo dl = {0};
    dl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dl.bindingCount = 8;
    dl.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(s_vk.device, &dl, NULL,
                                    &s_vk.descriptor_layout) != VK_SUCCESS)
        return 0;

    VkPushConstantRange range = {VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                 sizeof(GxVkClearPush)};
    VkPipelineLayoutCreateInfo pl = {0};
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &s_vk.descriptor_layout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(s_vk.device, &pl, NULL,
                               &s_vk.clear_pipeline_layout) !=
        VK_SUCCESS)
        return 0;

    VkShaderModuleCreateInfo sm = {0};
    sm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sm.codeSize = sizeof s_clear_spv;
    sm.pCode = s_clear_spv;
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(s_vk.device, &sm, NULL, &module) != VK_SUCCESS)
        return 0;
    VkComputePipelineCreateInfo pi = {0};
    pi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pi.stage.module = module;
    pi.stage.pName = "main";
    pi.layout = s_vk.clear_pipeline_layout;
    VkResult pipeline_result = vkCreateComputePipelines(
        s_vk.device, VK_NULL_HANDLE, 1, &pi, NULL, &s_vk.clear_pipeline);
    vkDestroyShaderModule(s_vk.device, module, NULL);
    if (pipeline_result != VK_SUCCESS)
        return 0;

    range.size = sizeof(GxVkCopyPush);
    if (vkCreatePipelineLayout(s_vk.device, &pl, NULL,
                               &s_vk.copy_pipeline_layout) != VK_SUCCESS)
        return 0;
    sm.codeSize = s_vk.subgroup_xfb ? sizeof s_xfb_subgroup_spv :
                                      sizeof s_xfb_copy_spv;
    sm.pCode = s_vk.subgroup_xfb ? s_xfb_subgroup_spv : s_xfb_copy_spv;
    module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(s_vk.device, &sm, NULL, &module) != VK_SUCCESS)
        return 0;
    pi.stage.module = module;
    pi.layout = s_vk.copy_pipeline_layout;
    pipeline_result = vkCreateComputePipelines(
        s_vk.device, VK_NULL_HANDLE, 1, &pi, NULL, &s_vk.copy_pipeline);
    vkDestroyShaderModule(s_vk.device, module, NULL);
    if (pipeline_result != VK_SUCCESS)
        return 0;

    range.size = sizeof(GxVkDrawPush);
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(s_vk.device, &pl, NULL,
                               &s_vk.draw_f_pipeline_layout) != VK_SUCCESS)
        return 0;
    sm.codeSize = sizeof s_draw_f_spv;
    sm.pCode = s_draw_f_spv;
    module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(s_vk.device, &sm, NULL, &module) != VK_SUCCESS)
        return 0;
    pi.stage.module = module;
    pi.layout = s_vk.draw_f_pipeline_layout;
    pipeline_result = vkCreateComputePipelines(
        s_vk.device, VK_NULL_HANDLE, 1, &pi, NULL, &s_vk.draw_f_pipeline);
    vkDestroyShaderModule(s_vk.device, module, NULL);
    if (pipeline_result != VK_SUCCESS)
        return 0;

    range.size = sizeof(GxVkTexCopyPush);
    if (vkCreatePipelineLayout(s_vk.device, &pl, NULL,
                               &s_vk.efb_tex_copy_pipeline_layout) != VK_SUCCESS)
        return 0;
    sm.codeSize = sizeof s_efb_tex_copy_spv;
    sm.pCode = s_efb_tex_copy_spv;
    module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(s_vk.device, &sm, NULL, &module) != VK_SUCCESS)
        return 0;
    pi.stage.module = module;
    pi.layout = s_vk.efb_tex_copy_pipeline_layout;
    pipeline_result = vkCreateComputePipelines(
        s_vk.device, VK_NULL_HANDLE, 1, &pi, NULL, &s_vk.efb_tex_copy_pipeline);
    vkDestroyShaderModule(s_vk.device, module, NULL);
    if (pipeline_result != VK_SUCCESS)
        return 0;

    VkDescriptorPoolSize pool_sizes[2] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6}
    };
    VkDescriptorPoolCreateInfo dp = {0};
    dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dp.maxSets = 1;
    dp.poolSizeCount = 2;
    dp.pPoolSizes = pool_sizes;
    if (vkCreateDescriptorPool(s_vk.device, &dp, NULL, &s_vk.descriptor_pool) !=
        VK_SUCCESS)
        return 0;
    VkDescriptorSetAllocateInfo da = {0};
    da.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    da.descriptorPool = s_vk.descriptor_pool;
    da.descriptorSetCount = 1;
    da.pSetLayouts = &s_vk.descriptor_layout;
    if (vkAllocateDescriptorSets(s_vk.device, &da, &s_vk.descriptor_set) != VK_SUCCESS)
        return 0;
    VkDescriptorImageInfo images[2] = {0};
    VkWriteDescriptorSet writes[8] = {0};
    for (u32 i = 0; i < 2; ++i) {
        images[i].imageView = s_vk.view[i];
        images[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = s_vk.descriptor_set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo = &images[i];
    }
    VkDescriptorBufferInfo xfb_output = {
        s_vk.staging, XFB_SHADOW_OFFSET, XFB_SHADOW_TOTAL
    };
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = s_vk.descriptor_set;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].pBufferInfo = &xfb_output;
    VkDescriptorBufferInfo draw_job = {
        s_vk.staging, DRAW_JOB_OFFSET, DRAW_JOB_BYTES
    };
    writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet = s_vk.descriptor_set;
    writes[3].dstBinding = 3;
    writes[3].descriptorCount = 1;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[3].pBufferInfo = &draw_job;
    VkDescriptorBufferInfo texture_shadow = {
        s_vk.staging, TEXTURE_SHADOW_OFFSET, TEXTURE_SHADOW_BYTES
    };
    writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[4].dstSet = s_vk.descriptor_set;
    writes[4].dstBinding = 4;
    writes[4].descriptorCount = 1;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[4].pBufferInfo = &texture_shadow;
    VkDescriptorBufferInfo efb_tex_output = {
        s_vk.staging, EFB_TEX_SHADOW_OFFSET, EFB_TEX_SHADOW_BYTES
    };
    writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[5].dstSet = s_vk.descriptor_set;
    writes[5].dstBinding = 5;
    writes[5].descriptorCount = 1;
    writes[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[5].pBufferInfo = &efb_tex_output;
    VkDescriptorBufferInfo tlut_shadow = {
        s_vk.staging, TLUT_SHADOW_OFFSET, TLUT_SHADOW_BYTES
    };
    writes[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[6].dstSet = s_vk.descriptor_set;
    writes[6].dstBinding = 6;
    writes[6].descriptorCount = 1;
    writes[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[6].pBufferInfo = &tlut_shadow;
    VkDescriptorBufferInfo general_debug = {
        s_vk.readback, READBACK_GENERAL_DEBUG_OFFSET, GENERAL_DEBUG_BYTES
    };
    writes[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[7].dstSet = s_vk.descriptor_set;
    writes[7].dstBinding = 7;
    writes[7].descriptorCount = 1;
    writes[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[7].pBufferInfo = &general_debug;
    vkUpdateDescriptorSets(s_vk.device, 8, writes, 0, NULL);

    VkCommandBufferAllocateInfo ca = {0};
    ca.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ca.commandPool = s_vk.command_pool;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = XFB_RING_SIZE;
    if (vkAllocateCommandBuffers(s_vk.device, &ca,
                                 s_vk.command_buffers) != VK_SUCCESS)
        return 0;
    s_vk.command_buffer = s_vk.command_buffers[0];
    return 1;
}

/* GCN_GX_VK_CORUN: differential co-run instrument. Software rasterizes every
 * triangle (the resident sink records accepted ones but still returns them to
 * the software scan), so whenever the GPU planes are downloaded they must
 * byte-match the software planes. Downloads happen at every fallback sync
 * AND — corun only — at every EFB copy, so the compare cadence is at least
 * once per guest frame even on screens the resident program set fully covers
 * (a fallback-only cadence compared NOTHING on zero-fallback screens, which
 * silently voided the instrument exactly where the resident path does all
 * the work). A mismatch is a resident-path bug caught at the next boundary;
 * within a frame the GPU submission cadence (async submits, fence points,
 * arena reuse) is left as shipped, so timing-dependent corruption inside a
 * frame stays reproducible under the instrument. */
static int s_corun = -1;
static u64 s_corun_checks, s_corun_hits;
/* Census of every divergence by (first-tile progmask, plane) so muted hits
 * (print gate passes only the first 64 + every 256th) still get ATTRIBUTED —
 * a class hiding between sampled prints was possible before this table. */
#define CORUN_CENSUS_MAX 32
static struct { u32 mask; u8 plane; u64 hits; u64 px; } s_corun_census[CORUN_CENSUS_MAX];
static u32 s_corun_census_used;

static void corun_census_add(u32 mask, u32 plane, u32 px) {
    for (u32 i = 0; i < s_corun_census_used; ++i) {
        if (s_corun_census[i].mask == mask && s_corun_census[i].plane == plane) {
            s_corun_census[i].hits++;
            s_corun_census[i].px += px;
            return;
        }
    }
    if (s_corun_census_used < CORUN_CENSUS_MAX) {
        s_corun_census[s_corun_census_used].mask = mask;
        s_corun_census[s_corun_census_used].plane = (u8)plane;
        s_corun_census[s_corun_census_used].hits = 1;
        s_corun_census[s_corun_census_used].px = px;
        s_corun_census_used++;
    }
}

/* Program id (1-based) -> letter label, A..Z then AA/AB/... (spreadsheet-
 * column style) -- the single-char ('A'+id-1) scheme used through program X
 * (24) runs out at Z (26); GX_VK_DRAW_PROGRAM_COUNT is now 30 (Y/Z/AA/AB/
 * AC/AD), so labels 27-30 need two letters. Returns a pointer into a small
 * static table; callers never hold more than one label at a time (both call
 * sites format-and-print immediately), so reuse across calls is safe. */
static const char* gx_vk_program_label(u32 program) {
    static char buf[4];
    if (program == 0 || program > 26u * 27u) return "?";
    if (program <= 26u) {
        buf[0] = (char)('A' + program - 1u);
        buf[1] = '\0';
    } else {
        u32 rest = program - 27u;   /* AA=27 -> rest 0 */
        buf[0] = (char)('A' + rest / 26u);
        buf[1] = (char)('A' + rest % 26u);
        buf[2] = '\0';
    }
    return buf;
}

int gx_vulkan_resident_busy(void) {
    return s_vk.resident_mode &&
           (s_vk.resident_recording || s_vk.resident_inflight != 0);
}

void gx_vulkan_shadow_shutdown(void) {
    if (s_vk.resident_mode &&
        (s_vk.resident_recording || s_vk.resident_inflight) &&
        !resident_submit_batch())
        fprintf(stderr, "gx_vulkan: final resident batch submission failed\n");
    if (s_vk.comparisons)
        fprintf(stderr,
                "gx_vulkan: %llu EFB and %llu XFB copy-boundary comparisons passed\n",
                (unsigned long long)s_vk.comparisons,
                (unsigned long long)s_vk.xfb_comparisons);
    if (s_vk.draws) {
        /* Program-letter labels A..Z then AA.. (see gx_vk_program_label
         * below) -- the fixed A..X arity predates this Y-AD extension; the
         * six new ones are appended rather than folded into the format
         * string so every existing count keeps its position. */
        fprintf(stderr,
                "gx_vulkan: captured %llu draws / %llu post-clip triangles "
                "(A=%llu B=%llu C=%llu D=%llu E=%llu F=%llu G=%llu "
                "H=%llu I=%llu J=%llu K=%llu L=%llu M=%llu N=%llu "
                "O=%llu P=%llu Q=%llu R=%llu S=%llu T=%llu U=%llu "
                "V=%llu W=%llu X=%llu Y=%llu Z=%llu AA=%llu AB=%llu "
                "AC=%llu AD=%llu GEN=%llu general=%llu)\n",
                (unsigned long long)s_vk.draws,
                (unsigned long long)s_vk.triangles,
                (unsigned long long)s_vk.fused_triangles[1],
                (unsigned long long)s_vk.fused_triangles[2],
                (unsigned long long)s_vk.fused_triangles[3],
                (unsigned long long)s_vk.fused_triangles[4],
                (unsigned long long)s_vk.fused_triangles[5],
                (unsigned long long)s_vk.fused_triangles[6],
                (unsigned long long)s_vk.fused_triangles[7],
                (unsigned long long)s_vk.fused_triangles[8],
                (unsigned long long)s_vk.fused_triangles[9],
                (unsigned long long)s_vk.fused_triangles[10],
                (unsigned long long)s_vk.fused_triangles[11],
                (unsigned long long)s_vk.fused_triangles[12],
                (unsigned long long)s_vk.fused_triangles[13],
                (unsigned long long)s_vk.fused_triangles[14],
                (unsigned long long)s_vk.fused_triangles[15],
                (unsigned long long)s_vk.fused_triangles[16],
                (unsigned long long)s_vk.fused_triangles[17],
                (unsigned long long)s_vk.fused_triangles[18],
                (unsigned long long)s_vk.fused_triangles[19],
                (unsigned long long)s_vk.fused_triangles[20],
                (unsigned long long)s_vk.fused_triangles[21],
                (unsigned long long)s_vk.fused_triangles[22],
                (unsigned long long)s_vk.fused_triangles[23],
                (unsigned long long)s_vk.fused_triangles[24],
                (unsigned long long)s_vk.fused_triangles[25],
                (unsigned long long)s_vk.fused_triangles[26],
                (unsigned long long)s_vk.fused_triangles[27],
                (unsigned long long)s_vk.fused_triangles[28],
                (unsigned long long)s_vk.fused_triangles[29],
                (unsigned long long)s_vk.fused_triangles[30],
                (unsigned long long)s_vk.fused_triangles[31],
                (unsigned long long)s_vk.fused_triangles[0]);
    }
    for (u32 program = 1; program <= GX_VK_DRAW_PROGRAM_COUNT; ++program) {
        if (s_vk.draw_validations[program])
            fprintf(stderr,
                    "gx_vulkan: %llu fused-program-%s GPU triangle comparisons passed\n",
                    (unsigned long long)s_vk.draw_validations[program],
                    gx_vk_program_label(program));
    }
    if (s_vk.texture_hits || s_vk.texture_misses)
        fprintf(stderr,
                "gx_vulkan: exact texture cache hit=%llu miss/update=%llu "
                "compared=%llu MiB changed=%llu MiB\n",
                (unsigned long long)s_vk.texture_hits,
                (unsigned long long)s_vk.texture_misses,
                (unsigned long long)(s_vk.texture_bytes_compared >> 20),
                (unsigned long long)(s_vk.texture_bytes_changed >> 20));
    if (s_vk.resident_mode)
        fprintf(stderr,
                "gx_vulkan: resident path queued %llu triangles in %llu batches; "
                "%llu synchronized fallbacks\n",
                (unsigned long long)s_vk.resident_triangles,
                (unsigned long long)s_vk.resident_batches,
                (unsigned long long)s_vk.resident_fallbacks);
    if (s_corun == 1) {
        fprintf(stderr,
                "gx_vulkan: co-run differential: %llu plane checks, "
                "%llu divergences\n",
                (unsigned long long)s_corun_checks,
                (unsigned long long)s_corun_hits);
        for (u32 i = 0; i < s_corun_census_used; ++i)
            fprintf(stderr,
                    "gx_vulkan: co-run census: progmask=%08X %s hits=%llu "
                    "px=%llu\n",
                    s_corun_census[i].mask,
                    s_corun_census[i].plane ? "depth" : "color",
                    (unsigned long long)s_corun_census[i].hits,
                    (unsigned long long)s_corun_census[i].px);
    }
    if (s_vk.resident_mode && s_vk.resident_copy_total_tsc)
        fprintf(stderr,
                "gx_vulkan: resident timing submit-cpu=%.1f%% wait=%.1f%% "
                "xfb-memcpy=%.1f%% of EFB-copy wall\n",
                100.0 * (double)s_vk.resident_submit_cpu_tsc /
                    (double)s_vk.resident_copy_total_tsc,
                100.0 * (double)s_vk.resident_wait_tsc /
                    (double)s_vk.resident_copy_total_tsc,
                100.0 * (double)s_vk.resident_copy_memcpy_tsc /
                    (double)s_vk.resident_copy_total_tsc);
    if (s_vk.efb_copy_verify)
        fprintf(stderr,
                "gx_vulkan: EFB->texture copy verify: %llu compare(s), 0 "
                "mismatches (any mismatch aborts immediately, see above)\n",
                (unsigned long long)s_vk.efb_copy_verify_compares);
    if (s_vk.gpu_stats) {
        static const char* names[GPU_TIME_COUNT] = {"draw", "xfb", "clear"};
        for (u32 i = 0; i < GPU_TIME_COUNT; ++i) {
            if (s_vk.gpu_time_calls[i])
                fprintf(stderr,
                        "gx_vulkan: GPU %s %.3f ms total, %.3f ms/call "
                        "(%llu calls)\n",
                        names[i], s_vk.gpu_time_ns[i] / 1.0e6,
                        s_vk.gpu_time_ns[i] / 1.0e6 /
                            (double)s_vk.gpu_time_calls[i],
                        (unsigned long long)s_vk.gpu_time_calls[i]);
        }
    }
    for (u32 i = 0; i < GX_VK_TEXTURE_CACHE_ENTRIES; ++i) {
        const GxVkTextureEntry* entry = &s_vk.texture_cache[i];
        if (entry->used)
            fprintf(stderr,
                    "gx_vulkan: texture[%u] addr=%08X fmt=%u size=%ux%u bytes=%u\n",
                    i, entry->address, entry->format, entry->width,
                    entry->height, entry->length);
    }
    if (s_vk.device)
        vkDeviceWaitIdle(s_vk.device);
    if (s_vk.device && s_vk.staging_map)
        vkUnmapMemory(s_vk.device, s_vk.staging_memory);
    if (s_vk.device && s_vk.readback_map)
        vkUnmapMemory(s_vk.device, s_vk.readback_memory);
    if (s_vk.device && s_vk.efb_tex_copy_pipeline)
        vkDestroyPipeline(s_vk.device, s_vk.efb_tex_copy_pipeline, NULL);
    if (s_vk.device && s_vk.draw_f_pipeline)
        vkDestroyPipeline(s_vk.device, s_vk.draw_f_pipeline, NULL);
    if (s_vk.device && s_vk.copy_pipeline)
        vkDestroyPipeline(s_vk.device, s_vk.copy_pipeline, NULL);
    if (s_vk.device && s_vk.clear_pipeline)
        vkDestroyPipeline(s_vk.device, s_vk.clear_pipeline, NULL);
    if (s_vk.device && s_vk.efb_tex_copy_pipeline_layout)
        vkDestroyPipelineLayout(s_vk.device, s_vk.efb_tex_copy_pipeline_layout, NULL);
    if (s_vk.device && s_vk.draw_f_pipeline_layout)
        vkDestroyPipelineLayout(s_vk.device, s_vk.draw_f_pipeline_layout, NULL);
    if (s_vk.device && s_vk.copy_pipeline_layout)
        vkDestroyPipelineLayout(s_vk.device, s_vk.copy_pipeline_layout, NULL);
    if (s_vk.device && s_vk.clear_pipeline_layout)
        vkDestroyPipelineLayout(s_vk.device, s_vk.clear_pipeline_layout, NULL);
    if (s_vk.device && s_vk.descriptor_pool)
        vkDestroyDescriptorPool(s_vk.device, s_vk.descriptor_pool, NULL);
    if (s_vk.device && s_vk.descriptor_layout)
        vkDestroyDescriptorSetLayout(s_vk.device, s_vk.descriptor_layout, NULL);
    if (s_vk.device && s_vk.staging)
        vkDestroyBuffer(s_vk.device, s_vk.staging, NULL);
    if (s_vk.device && s_vk.staging_memory)
        vkFreeMemory(s_vk.device, s_vk.staging_memory, NULL);
    if (s_vk.device && s_vk.readback)
        vkDestroyBuffer(s_vk.device, s_vk.readback, NULL);
    if (s_vk.device && s_vk.readback_memory)
        vkFreeMemory(s_vk.device, s_vk.readback_memory, NULL);
    for (u32 i = 0; i < 2; ++i) {
        if (s_vk.device && s_vk.view[i])
            vkDestroyImageView(s_vk.device, s_vk.view[i], NULL);
        if (s_vk.device && s_vk.image[i])
            vkDestroyImage(s_vk.device, s_vk.image[i], NULL);
        if (s_vk.device && s_vk.memory[i])
            vkFreeMemory(s_vk.device, s_vk.memory[i], NULL);
    }
    if (s_vk.device && s_vk.query_pool)
        vkDestroyQueryPool(s_vk.device, s_vk.query_pool, NULL);
    if (s_vk.device && s_vk.resident_fence)
        vkDestroyFence(s_vk.device, s_vk.resident_fence, NULL);
    if (s_vk.device && s_vk.command_pool)
        vkDestroyCommandPool(s_vk.device, s_vk.command_pool, NULL);
    if (s_vk.device)
        vkDestroyDevice(s_vk.device, NULL);
    if (s_vk.instance)
        vkDestroyInstance(s_vk.instance, NULL);
    for (u32 i = 0; i < GX_VK_TEXTURE_CACHE_ENTRIES; ++i)
        free(s_vk.texture_cache[i].bytes);
    memset(&s_vk, 0, sizeof s_vk);
}

int gx_vulkan_shadow_init(void) {
    gx_vulkan_shadow_shutdown();
    /* Pass D general-TEV debug hook (docs/GX_GENERAL_TEV.md): parse once,
     * defaults to -1,-1 (armed nowhere, so shade_pixel_general's guard
     * compare never matches and the dump path costs nothing). */
    s_vk.general_debug_x = -1;
    s_vk.general_debug_y = -1;
    s_vk.general_debug_armed = 0;
    { const char* e = getenv("GCN_GX_GENERAL_DEBUG_XY");
      int gx = -1, gy = -1;
      if (e && sscanf(e, "%d,%d", &gx, &gy) == 2 && gx >= 0 && gy >= 0) {
          s_vk.general_debug_x = gx;
          s_vk.general_debug_y = gy;
          s_vk.general_debug_armed = 1;
          fprintf(stderr, "gx_vulkan: general-TEV debug armed at (%d,%d)\n", gx, gy);
      } }
    VkApplicationInfo app = {0};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "gcnrecomp GX shadow";
    app.applicationVersion = VK_MAKE_VERSION(0, 2, 0);
    app.pEngineName = "gcnrecomp";
    app.engineVersion = VK_MAKE_VERSION(0, 2, 0);
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = {0};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    if (vkCreateInstance(&ici, NULL, &s_vk.instance) != VK_SUCCESS)
        goto fail;

    u32 physical_count = 0;
    if (vkEnumeratePhysicalDevices(s_vk.instance, &physical_count, NULL) != VK_SUCCESS ||
        physical_count == 0)
        goto fail;
    VkPhysicalDevice physicals[16];
    if (physical_count > 16) physical_count = 16;
    if (vkEnumeratePhysicalDevices(s_vk.instance, &physical_count, physicals) != VK_SUCCESS)
        goto fail;
    int best_score = -1;
    for (u32 p = 0; p < physical_count; ++p) {
        u32 qcount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physicals[p], &qcount, NULL);
        VkQueueFamilyProperties qprops[64];
        if (qcount > 64) qcount = 64;
        vkGetPhysicalDeviceQueueFamilyProperties(physicals[p], &qcount, qprops);
        for (u32 q = 0; q < qcount; ++q) {
            if (!(qprops[q].queueFlags & VK_QUEUE_COMPUTE_BIT))
                continue;
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(physicals[p], &props);
            int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
            if (score > best_score) {
                best_score = score;
                s_vk.physical = physicals[p];
                s_vk.queue_family = q;
            }
        }
    }
    if (!s_vk.physical)
        goto fail;

    VkPhysicalDeviceSubgroupProperties subgroup = {0};
    subgroup.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
    VkPhysicalDeviceProperties2 props2 = {0};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &subgroup;
    vkGetPhysicalDeviceProperties2(s_vk.physical, &props2);
    /* The subgroup XFB-copy variant pulls the previous pixel's filtered value
     * via subgroupShuffleUp() instead of recomputing it — but main() early-
     * returns for out-of-bounds invocations, and shuffling from an inactive
     * lane is UNDEFINED, which splatters garbage chroma across the frame
     * (intermittent pink/yellow flood).  Faithfulness first: the portable path
     * is the byte-exact default; the subgroup variant is opt-in until its
     * lane-activity bug is fixed (ENHANCEMENTS Rule 1: default is byte-exact,
     * enhancement off). */
    s_vk.subgroup_xfb =
        getenv("GCN_GX_SUBGROUP_XFB") != NULL &&
        (subgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) != 0u &&
        (subgroup.supportedOperations &
         VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT) != 0u;
    VkPhysicalDeviceProperties chosen_props;
    vkGetPhysicalDeviceProperties(s_vk.physical, &chosen_props);
    s_vk.timestamp_period = chosen_props.limits.timestampPeriod;
    s_vk.gpu_stats = getenv("GCN_GX_GPU_STATS") ? 1 : 0;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {0};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = s_vk.queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci = {0};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    if (vkCreateDevice(s_vk.physical, &dci, NULL, &s_vk.device) != VK_SUCCESS)
        goto fail;
    vkGetDeviceQueue(s_vk.device, s_vk.queue_family, 0, &s_vk.queue);
    VkFenceCreateInfo fci = {0};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(s_vk.device, &fci, NULL,
                      &s_vk.resident_fence) != VK_SUCCESS)
        goto fail;
    if (s_vk.gpu_stats) {
        VkQueryPoolCreateInfo qpi = {0};
        qpi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpi.queryCount = GPU_QUERY_MAX;
        if (vkCreateQueryPool(s_vk.device, &qpi, NULL,
                              &s_vk.query_pool) != VK_SUCCESS) {
            fprintf(stderr,
                    "gx_vulkan: timestamp query pool unavailable; GPU stats disabled\n");
            s_vk.gpu_stats = 0;
        }
    }
    VkCommandPoolCreateInfo pci = {0};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = s_vk.queue_family;
    if (vkCreateCommandPool(s_vk.device, &pci, NULL, &s_vk.command_pool) != VK_SUCCESS ||
        !create_efb_image(0) || !create_efb_image(1) ||
        !create_staging() ||
        !create_readback() ||
        !create_compute_state())
        goto fail;

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(s_vk.physical, &props);
    fprintf(stderr,
        "gx_vulkan: differential EFB/XFB ready on %s (API %u.%u.%u, queue family %u, "
        "upload coherent=%d cached=%d, readback coherent=%d cached=%d, "
        "XFB=%s)\n",
        props.deviceName, VK_VERSION_MAJOR(props.apiVersion),
        VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion),
        s_vk.queue_family, s_vk.staging_coherent, s_vk.staging_cached,
        s_vk.readback_coherent, s_vk.readback_cached,
        s_vk.subgroup_xfb ? "subgroup" : "portable");
    u32 draw_validate_count = 0;
    { const char* e = getenv("GCN_GX_VK_DRAW_VALIDATE");
      if (e && *e) draw_validate_count = (u32)strtoul(e, NULL, 0); }
    s_vk.draw_validate_program = 6u;
    { const char* e = getenv("GCN_GX_VK_DRAW_PROGRAM");
      if (e && e[0] && !e[1] && e[0] >= 'A' && e[0] <= 'X')
          s_vk.draw_validate_program = (u32)(e[0] - 'A') + 1u;
      else if (e && strcmp(e, "ALL") == 0)
          s_vk.draw_validate_program = 0u;
      else if (e && *e) {
          /* Numeric fallback: the single-letter scheme predates Y-AD
           * (25-30) and general (31, docs/GX_GENERAL_TEV.md), neither of
           * which fits the "one letter" A-X convention. */
          char* end = NULL;
          unsigned long v = strtoul(e, &end, 10);
          if (end && *end == '\0' && v >= 1 && v <= GX_VK_DRAW_PROGRAM_COUNT)
              s_vk.draw_validate_program = (u32)v;
          else
              fprintf(stderr,
                      "gx_vulkan: invalid GCN_GX_VK_DRAW_PROGRAM='%s'; using F\n",
                      e);
      } }
    if (s_vk.draw_validate_program) {
        s_vk.draw_validate_remaining[s_vk.draw_validate_program] =
            draw_validate_count;
    } else {
        for (u32 program = 1; program <= GX_VK_DRAW_PROGRAM_COUNT; ++program)
            s_vk.draw_validate_remaining[program] = draw_validate_count;
    }
    return 1;
fail:
    fprintf(stderr, "gx_vulkan: differential device/EFB initialization failed\n");
    gx_vulkan_shadow_shutdown();
    return 0;
}

int gx_vulkan_resident_init(void) {
    if (!gx_vulkan_shadow_init())
        return 0;
    s_vk.resident_mode = 1;
    memset(s_vk.draw_validate_remaining, 0,
           sizeof s_vk.draw_validate_remaining);
    fprintf(stderr,
            "gx_vulkan: resident exact A--X compute path enabled; unsupported "
            "state synchronizes to software\n");
    s_corun = getenv("GCN_GX_VK_CORUN") != NULL;
    if (s_corun)
        fprintf(stderr,
                "gx_vulkan: CO-RUN differential active — software rasterizes "
                "everything; GPU planes byte-checked at every fallback sync\n");
    s_vk.efb_copy_verify = getenv("GCN_GX_EFB_COPY_VERIFY") ? 1 : 0;
    if (s_vk.efb_copy_verify)
        fprintf(stderr,
                "gx_vulkan: EFB->texture copy verify active — every resident "
                "RG8/RGBA8 copy is also encoded on the CPU from a downloaded "
                "EFB snapshot and memcmp'd against the GPU result (forces a "
                "synchronous submit per copy; not for perf runs)\n");
    return 1;
}

static GxVkClearPush decode_clear(const u32* bp) {
    GxVkClearPush p = {0};
    p.left = bp[0x49] & 0x3ffu;
    p.top = (bp[0x49] >> 10) & 0x3ffu;
    p.right = p.left + (bp[0x4a] & 0x3ffu);
    p.bottom = p.top + ((bp[0x4a] >> 10) & 0x3ffu);
    if (p.right >= EFB_WIDTH) p.right = EFB_WIDTH - 1u;
    if (p.bottom >= EFB_HEIGHT) p.bottom = EFB_HEIGHT - 1u;
    p.clear_enable = (bp[0x52] >> 11) & 1u;
    p.pixel_format = bp[0x43] & 7u;
    p.color_update = (bp[0x41] >> 3) & 1u;
    p.alpha_update = (bp[0x41] >> 4) & 1u;
    p.depth_update = (bp[0x40] >> 4) & 1u;
    u32 ar = bp[0x4f], gb = bp[0x50];
    p.clear_color = ((ar & 0xffu) << 24) | (gb << 8) | ((ar & 0xff00u) >> 8);
    p.clear_depth = bp[0x51];
    return p;
}

static GxVkTextureEntry* find_texture_binding(const u32* bp, u32 unit) {
    u32 image0 = bp[0x88 + unit];
    u32 image3 = bp[0x94 + unit];
    u32 width = (image0 & 0x3ffu) + 1u;
    u32 height = ((image0 >> 10) & 0x3ffu) + 1u;
    u32 format = (image0 >> 20) & 0xfu;
    u32 address = ((image3 & 0x00ffffffu) << 5) & 0x1fffffffu;
    u32 length = texture_encoded_size(format, width, height);
    GxVkTextureEntry* last = s_vk.last_texture_binding;
    if (last && last->used && last->address == address &&
        last->format == format && last->width == width &&
        last->height == height && last->length == length)
        return last;
    for (u32 i = 0; i < GX_VK_TEXTURE_CACHE_ENTRIES; ++i) {
        GxVkTextureEntry* entry = &s_vk.texture_cache[i];
        if (entry->used && entry->address == address &&
            entry->format == format && entry->width == width &&
            entry->height == height && entry->length == length) {
            s_vk.last_texture_binding = entry;
            return entry;
        }
    }
    return NULL;
}

/* Phase 1a general TEV program (docs/GX_GENERAL_TEV.md): TLUT lookup for
 * C4/C8, mirroring find_texture_binding but keyed by TMEM byte offset. */
static GxVkTlutEntry* find_tlut_binding(u32 tmem_offset, u32 length) {
    for (u32 i = 0; i < GX_VK_TLUT_CACHE_ENTRIES; ++i) {
        GxVkTlutEntry* entry = &s_vk.tlut_cache[i];
        if (entry->used && entry->tmem_offset == tmem_offset && entry->length == length)
            return entry;
    }
    return NULL;
}

/* Phase 1a general TEV program (docs/GX_GENERAL_TEV.md): the 8 eligible
 * texture formats (I4/I8/IA4/IA8/RGB5A3/RGBA8/C4/C8), same set the shader's
 * decode_texture_g supports. */
static int general_texture_format_ok(u32 format) {
    return format == 0u || format == 1u || format == 2u || format == 3u ||
           format == 5u || format == 6u || format == 8u || format == 9u ||
           format == 0xEu;   /* CMPR, phase 1b: docs/GX_GENERAL_TEV.md */
}

/* One texture unit's resident-eligibility check, shared by programs 1..30
 * (formats {I4=0,I8=1,RGBA8=6} only, unchanged) and program 31 (the 8-format
 * general set, docs/GX_GENERAL_TEV.md). Returns the resolved texture (NULL
 * on any rejection) and, via *tlut_out, the TLUT entry for C4/C8 (NULL for
 * every other format, including a successful non-paletted resolve). */
static GxVkTextureEntry* resolve_texture_unit(u32 unit, int general,
                                              GxVkTlutEntry** tlut_out) {
    *tlut_out = NULL;
    u32 mode0 = s_vk.draw_bp[0x80 + unit];
    GxVkTextureEntry* texture = find_texture_binding(s_vk.draw_bp, unit);
    if (!texture)
        return NULL;
    int fmt_ok = general ? general_texture_format_ok(texture->format)
                         : (texture->format == 0u || texture->format == 1u ||
                            texture->format == 6u);
    if (!fmt_ok || texture->length > TEXTURE_SHADOW_BYTES ||
        (mode0 & 3u) == 3u || ((mode0 >> 2) & 3u) == 3u ||
        ((mode0 >> 5) & 3u) != 0u)   /* mipmap_filter != none (no mip, either family) */
        return NULL;
    if (texture->format == 8u || texture->format == 9u) {
        u32 tlut_reg = (unit < 4u) ? s_vk.draw_bp[0x98 + unit]
                                   : s_vk.draw_bp[0xB8 + (unit - 4u)];
        u32 tmem_offset = (tlut_reg & 0x3FFu) << 9;
        u32 tlut_len = (texture->format == 8u) ? 32u : 512u;
        GxVkTlutEntry* tlut = find_tlut_binding(tmem_offset, tlut_len);
        if (!tlut)
            return NULL;
        *tlut_out = tlut;
    }
    return texture;
}

static int resolve_fused_texture(const GxRasterTriangleJob* job,
                                 GxVkTextureEntry** texture_out,
                                 GxVkGeneralTex* general_out) {
    *texture_out = NULL;
    if (general_out) memset(general_out, 0, sizeof *general_out);

    if (job->fused_program == 31u) {
        /* General TEV (docs/GX_GENERAL_TEV.md): the shader supports at most
         * two texture "slots". Slot assignment is first-enabled-stage order
         * (slot 0 = the first distinct texmap an enabled stage references,
         * slot 1 = the next distinct one, if any) -- NOT "texmap==0", since
         * a general draw is not guaranteed to sample texmap 0 at all, let
         * alone at stage 0; this also matches what the shared words-79..90
         * packer below needs (see snapshot_fused_draw's gen->unit[] use).
         * gx_raster.c's eligibility gate already enforces <=2 distinct
         * texmaps, but re-derive defensively here rather than trust the job
         * to have gotten it right -- a gate/resolver disagreement must fail
         * closed. */
        u32 numtexgens = s_vk.draw_bp[0x00] & 0xfu;
        if (numtexgens == 0u)
            return 1;   /* no sampling needed at all */
        u32 last_stage = (s_vk.draw_bp[0x00] >> 10) & 0xfu;
        u32 distinct[2];
        u32 distinct_count = 0;
        for (u32 stage = 0; stage <= last_stage; ++stage) {
            u32 order = s_vk.draw_bp[0x28 + (stage >> 1)];
            u32 odd = stage & 1u;
            u32 enabled = odd ? ((order >> 18) & 1u) : ((order >> 6) & 1u);
            u32 unit = odd ? ((order >> 12) & 7u) : (order & 7u);
            if (!enabled)
                continue;
            u32 k;
            for (k = 0; k < distinct_count; ++k)
                if (distinct[k] == unit)
                    break;
            if (k == distinct_count) {
                if (distinct_count >= 2)
                    return 0;   /* >2 distinct texmaps: gate/resolver disagreement */
                distinct[distinct_count++] = unit;
            }
        }
        for (u32 slot = 0; slot < distinct_count; ++slot) {
            u32 unit = distinct[slot];
            GxVkTlutEntry* tlut = NULL;
            GxVkTextureEntry* texture = resolve_texture_unit(unit, 1, &tlut);
            if (!texture) {
                if (!s_vk.texture_reject_logged[31u]) {
                    s_vk.texture_reject_logged[31u] = 1;
                    fprintf(stderr,
                            "gx_vulkan: fused-program-general exact GPU path "
                            "rejected unit=%u (fmt/mip/wrap/TLUT outside the "
                            "8-format phase-1a set)\n", unit);
                }
                return 0;
            }
            general_out->tex[slot] = texture;
            general_out->tlut[slot] = tlut;
            general_out->unit[slot] = unit;
        }
        *texture_out = general_out->tex[0];
        return 1;
    }

    /* Y/Z/AA/AB/AC/AD (25-30, see gx_raster.c's gpu_program_Y_match
     * derivation) are all texgens==0/en==0 like F/J/M/P/T -- none of the six
     * new census shapes samples a texture at all, so they join this
     * no-texture-required list. */
    if (job->fused_program != 6u && job->fused_program != 10u &&
        job->fused_program != 13u && job->fused_program != 16u &&
        job->fused_program != 20u && job->fused_program != 23u &&
        job->fused_program != 25u && job->fused_program != 26u &&
        job->fused_program != 27u && job->fused_program != 28u &&
        job->fused_program != 29u && job->fused_program != 30u) {
        u32 order = s_vk.draw_bp[0x28];
        u32 unit = order & 7u;
        GxVkTlutEntry* tlut_unused = NULL;
        GxVkTextureEntry* texture = (order & (1u << 6)) ?
            resolve_texture_unit(unit, 0, &tlut_unused) : NULL;
        if (!texture) {
            if (!s_vk.texture_reject_logged[job->fused_program]) {
                s_vk.texture_reject_logged[job->fused_program] = 1;
                GxVkTextureEntry* logged = find_texture_binding(s_vk.draw_bp, unit);
                u32 mode0 = s_vk.draw_bp[0x80 + unit];
                fprintf(stderr,
                        "gx_vulkan: fused-program-%s exact GPU path supports "
                        "only a resident stage-0 I4/I8/RGBA8 non-mipmapped texture "
                        "with supported wrap (unit=%u fmt=%u size=%ux%u "
                        "mode0=%06X)\n",
                        gx_vk_program_label(job->fused_program), unit,
                        logged ? logged->format : 0xffffffffu,
                        logged ? logged->width : 0u,
                        logged ? logged->height : 0u, mode0);
            }
            return 0;
        }
        *texture_out = texture;
        /* Regression fix: callers (resident_record_draw) now call
         * ensure_resident_texture(gen.tex[0]/[1]) uniformly instead of
         * ensure_resident_texture(texture) directly, so gen.tex[0] must
         * mirror *texture_out here too -- otherwise programs 1..30's
         * texture never gets uploaded/refreshed to the resident arena
         * (gen stays all-NULL from the memset above, since only the
         * program==31 branch used to fill it), and the GPU silently
         * samples stale/garbage bytes. Caught by corun: gpu stuck at an
         * old value while software correctly re-drew every frame. */
        if (general_out) general_out->tex[0] = texture;
    }
    return 1;
}

static void snapshot_fused_draw(const GxRasterTriangleJob* job, u32* words,
                                const GxVkTextureEntry* texture,
                                const GxVkGeneralTex* gen) {
    _Static_assert(DRAW_PACKET_BYTES == 272u * sizeof(u32),
                   "general block (words 112..267) layout assumes a 272-word packet");
    _Static_assert(sizeof(GxRasterTriScan) == 21u * sizeof(u32),
                   "GPU triangle scan packet layout changed");
    _Static_assert(sizeof(GxRasterSlope) == 7u * sizeof(u32),
                   "GPU triangle slope packet layout changed");
    memcpy(words, &job->scan, sizeof job->scan);
    words[21] = job->pixel_format;
    for (u32 comp = 0; comp < 4u; ++comp)
        memcpy(words + 22u + comp * 7u, &job->color[0][comp],
               sizeof(GxRasterSlope));
    words[50] = job->fused_program;
    memcpy(words + 51u, &job->w, sizeof job->w);
    for (u32 comp = 0; comp < 3u; ++comp)
        memcpy(words + 58u + comp * 7u, &job->tex[0][comp],
               sizeof(GxRasterSlope));

    if (texture) {
        /* Slot 0's unit: for the general program (id 31) this is
         * gen->unit[0] (first-enabled-stage order, see resolve_fused_texture's
         * program-31 branch -- a general draw is not guaranteed to sample
         * texmap 0, let alone at stage 0). Every other program keeps the
         * original stage-pair-0 order-register derivation unchanged. */
        u32 unit = (gen && job->fused_program == 31u) ? gen->unit[0]
                                                       : (s_vk.draw_bp[0x28] & 7u);
        u32 mode0 = s_vk.draw_bp[0x80 + unit];
        u32 mode1 = s_vk.draw_bp[0x84 + unit];
        words[79] = texture->format;
        words[80] = texture->width;
        words[81] = texture->height;
        words[82] = mode0 & 3u;
        words[83] = (mode0 >> 2) & 3u;
        words[84] = (mode0 >> 4) & 1u;
        words[85] = (mode0 >> 7) & 1u;
        words[86] = (mode0 >> 8) & 1u;
        words[87] = (u32)((s32)(s8)((mode0 >> 9) & 0xffu) >> 1);
        words[88] = mode1 & 0xffu;
        words[89] = (mode1 >> 8) & 0xffu;
        words[90] = texture->length;
    } else {
        memset(words + 79u, 0, 12u * sizeof(*words));
    }
    for (u32 comp = 0; comp < 3u; ++comp) {
        words[91u + comp] = (u32)job->tev_reg[1][comp];
        words[94u + comp] = (u32)job->tev_reg[2][comp];
    }
    words[97] = (u32)job->stage_konst[0][3];
    words[98] = (u32)job->stage_konst[1][3];
    words[99] = 0u;
    memcpy(words + 100u, &job->z, sizeof job->z);
    words[107] = (u32)job->tev_reg[1][3];
    /* AB/AC (28/29, see gx_raster.c's gpu_program_AB_match derivation) need
     * StageKonst.rgb (stage 0's konst r/g/b) -- the only three fields this
     * whole fused family reads that were never packed before (every prior
     * program only ever needed stage_konst[*].a, packed above at 97/98).
     * Reuses three of the previously-untouched stride-padding words. */
    words[108] = (u32)job->stage_konst[0][0];
    words[109] = (u32)job->stage_konst[0][1];
    words[110] = (u32)job->stage_konst[0][2];
    /* Y/Z (25/26) read this live instead of a matcher-pinned constant -- see
     * GxRasterTriangleJob::bm_au's comment. */
    words[111] = (u32)job->bm_au;

    /* General block (docs/GX_GENERAL_TEV.md), words 112..267. Phase 1a
     * (program 31, live since 11ae141) added 112..255; phase 1b adds
     * 256..267 for fog. Layout (156 words total):
     *
     *   112        raw GenMode            BP 0x00 (numtexgens/numtevstages/
     *                                     numcolchans bitfields, undecoded)
     *   113..144   raw TevStageCombiner cc/ac, 16 stages x 2   BP 0xC0-0xDF
     *   145..152   raw TRef (order/texmap/texcoord/enable/colorchan),
     *              stage-pairs 0-7                             BP 0x28-0x2F
     *   153..160   raw TevKSel (konst + swap-table selectors),
     *              stage-pairs 0-7                             BP 0xF6-0xFD
     *   161..176   tev_reg[4][4]: decoded Prev/Color0/Color1/Color2 RGBA
     *              (job->tev_reg -- CPU already sign-extends these once;
     *              only tev_reg[1]/[2] rgb + tev_reg[1].a were packed
     *              before, at 91-96/107, for the matched A..AD programs)
     *   177..184   raw TEV/konst registers (tev_load_registers input)
     *                                                          BP 0xE0-0xE7
     *   185        raw AlphaTest word                          BP 0xF3
     *   186        raw ZMode                                   BP 0x40
     *   187        raw BlendMode                                BP 0x41
     *   188        raw dest-alpha/ConstantAlpha                BP 0x42
     *   189        raw PEControl                                BP 0x43
     *   190..217   color[1] slopes: 4x GxRasterSlope (7 words each) --
     *              second Gouraud color channel (job->color[1])
     *   218..238   tex[1] slopes: 3x GxRasterSlope (7 words each) --
     *              second texgen (job->tex[1])
     *   239..250   second texture metadata block, mirrors the words-79..90
     *              schema (format/width/height/wrap-s/wrap-t/mipmap/
     *              edge-lod/lod-bias/min-linear/mag-linear/length) for
     *              gen->tex[1] (the second distinct texmap an enabled stage
     *              references, first-enabled-stage-order slot 1 -- see
     *              resolve_fused_texture's program-31 branch); zeroed when
     *              there is no second unit.
     *   251        primary slot's TLUT format (0=IA8,1=RGB565,2=RGB5A3),
     *              only meaningful for slot 0 formats C4(8)/C8(9)
     *   252        primary slot's TLUT byte offset into the TlutData SSBO
     *              (gx_vulkan.c's TLUT_SHADOW_OFFSET arena)
     *   253        secondary slot's TLUT format
     *   254        secondary slot's TLUT byte offset into the TlutData SSBO
     *   255        secondary slot's texture-bytes arena base word, mirrors
     *              word 99's role for slot 0 (both are patched by the
     *              caller after ensure_resident_texture/_tlut assign the
     *              real gpu_offset -- see resident_record_draw/
     *              submit_fused_draw)
     *   256..266   raw fog BP window                              BP 0xE8-0xF2
     *              (range_base, 5x K-table regs, fog-a, fog-proj/denom,
     *              fog-shift, fog-param3/fsel+c, fog color -- see
     *              apply_fog's exact field-by-field derivation, gx_raster.c
     *              ~1866-1935, and gen_apply_fog's mirrored comments)
     *   267        raw XF register 0x101a (vp_wd(), viewport width) --
     *              apply_fog's range-adjust path only, gx_raster.c:4592
     *
     * Raw BP windows (not pre-decoded fields) per the spec's auditability
     * rule: the eventual shader re-derives bitfields with a line-for-line
     * port of the CPU extraction, so every GLSL helper can cite the exact
     * C function it transcribes. */
    memcpy(words + 256u, s_vk.draw_bp + 0xE8, 11u * sizeof(*words));
    words[267] = gx_raster_xf_word(0x101au);
    memcpy(words + 113u, s_vk.draw_bp + 0xC0, 32u * sizeof(*words));
    memcpy(words + 145u, s_vk.draw_bp + 0x28, 8u * sizeof(*words));
    memcpy(words + 153u, s_vk.draw_bp + 0xF6, 8u * sizeof(*words));
    for (u32 reg = 0; reg < 4u; ++reg)
        for (u32 comp = 0; comp < 4u; ++comp)
            words[161u + reg * 4u + comp] = (u32)job->tev_reg[reg][comp];
    memcpy(words + 177u, s_vk.draw_bp + 0xE0, 8u * sizeof(*words));
    words[185] = s_vk.draw_bp[0xF3];
    words[186] = s_vk.draw_bp[0x40];
    words[187] = s_vk.draw_bp[0x41];
    words[188] = s_vk.draw_bp[0x42];
    words[189] = s_vk.draw_bp[0x43];
    words[112] = s_vk.draw_bp[0x00];
    for (u32 comp = 0; comp < 4u; ++comp)
        memcpy(words + 190u + comp * 7u, &job->color[1][comp],
               sizeof(GxRasterSlope));
    for (u32 comp = 0; comp < 3u; ++comp)
        memcpy(words + 218u + comp * 7u, &job->tex[1][comp],
               sizeof(GxRasterSlope));
    memset(words + 239u, 0, 17u * sizeof(*words)); /* 239..255: zero, then fill below */

    if (gen && gen->tex[0] && (gen->tex[0]->format == 8u || gen->tex[0]->format == 9u)) {
        u32 unit0 = gen->unit[0];
        u32 tlut_reg0 = (unit0 < 4u) ? s_vk.draw_bp[0x98 + unit0]
                                     : s_vk.draw_bp[0xB8 + (unit0 - 4u)];
        words[251] = (tlut_reg0 >> 10) & 3u;
        /* word 252 (TLUT arena byte offset) is patched by the caller once
         * ensure_resident_tlut assigns gen->tlut[0]->gpu_offset, same
         * two-step pattern word 99 already uses for the texture arena. */
    }
    if (gen && gen->tex[1]) {
        const GxVkTextureEntry* t2 = gen->tex[1];
        u32 unit1 = gen->unit[1];
        u32 mode0 = s_vk.draw_bp[0x80 + unit1];
        u32 mode1 = s_vk.draw_bp[0x84 + unit1];
        words[239] = t2->format;
        words[240] = t2->width;
        words[241] = t2->height;
        words[242] = mode0 & 3u;
        words[243] = (mode0 >> 2) & 3u;
        words[244] = (mode0 >> 4) & 1u;
        words[245] = (mode0 >> 7) & 1u;
        words[246] = (mode0 >> 8) & 1u;
        words[247] = (u32)((s32)(s8)((mode0 >> 9) & 0xffu) >> 1);
        words[248] = mode1 & 0xffu;
        words[249] = (mode1 >> 8) & 0xffu;
        words[250] = t2->length;
        if (t2->format == 8u || t2->format == 9u) {
            u32 tlut_reg1 = (unit1 < 4u) ? s_vk.draw_bp[0x98 + unit1]
                                         : s_vk.draw_bp[0xB8 + (unit1 - 4u)];
            words[253] = (tlut_reg1 >> 10) & 3u;
            /* word 254 patched by the caller, same as word 252/99/255. */
        }
    }
}

static int flush_staging(void) {
    if (s_vk.staging_coherent)
        return 1;
    VkMappedMemoryRange range = {0};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = s_vk.staging_memory;
    range.size = VK_WHOLE_SIZE;
    return vkFlushMappedMemoryRanges(s_vk.device, 1, &range) == VK_SUCCESS;
}

static int invalidate_readback(void) {
    if (s_vk.readback_coherent)
        return 1;
    VkMappedMemoryRange range = {0};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = s_vk.readback_memory;
    range.size = VK_WHOLE_SIZE;
    return vkInvalidateMappedMemoryRanges(s_vk.device, 1, &range) == VK_SUCCESS;
}

static int resident_materialize_pending(void) {
    if (!s_vk.resident_pending_count && !s_vk.resident_pending_tex_count)
        return 1;
    if (!invalidate_readback())
        return 0;
    u64 t0 = __rdtsc();
    if (s_vk.resident_pending_count) {
        gcn_gx_xfb_write_begin();
        for (u32 i = 0; i < s_vk.resident_pending_count; ++i) {
            const GxVkPendingXfb* p = &s_vk.resident_pending[i];
            const u8* gpu = s_vk.readback_map + READBACK_XFB_OFFSET + p->slot_offset;
            for (u32 y = 0; y < p->height; ++y)
                memcpy(p->ram + p->address + (u64)y * p->stride,
                       gpu + (u64)y * p->gpu_stride, (size_t)p->width * 2u);
            /* GCN_GX_XFB_HASH=1: same chain the software-raster path feeds
             * (gx_raster.c) -- one publication per completed EFB->XFB copy,
             * hashing the final guest-RAM bytes so the resident (GPU) and
             * software paths, and fused vs unfused programs, are compared by
             * output rather than code path. */
            gcn_gx_xfb_hash_feed(p->ram + p->address, p->stride, (u32)p->width * 2u, p->height);
            /* Device write to RAM: dirty the miss-CRC identity over the copied
             * span (see gx_raster.c's XFB copy). */
            gcn_native_code_content_dirty(p->address,
                                          (u32)((u64)p->height * p->stride));
            gcn_gx_xfb_hash_publish_done();
        }
        gcn_gx_xfb_write_end();
        s_vk.resident_pending_count = 0;
    }
    if (s_vk.resident_pending_tex_count) {
        /* EFB->texture resident copies (gx_vulkan_resident_efb_tex_copy):
         * same "device write to RAM" contract as the XFB copy above, but
         * these never feed the XFB hash chain -- the software EFB->texture
         * branch (gx_raster.c's gx_raster_efb_copy, copy_to_xfb==0 side)
         * doesn't either, only gcn_native_code_content_dirty. */
        for (u32 i = 0; i < s_vk.resident_pending_tex_count; ++i) {
            const GxVkPendingTexCopy* p = &s_vk.resident_pending_tex[i];
            const u8* gpu = s_vk.readback_map + READBACK_EFB_TEX_OFFSET +
                            p->slot_offset;
            for (u32 row = 0; row < p->tiles_y; ++row)
                memcpy(p->ram + p->dest_addr + (u64)row * p->dest_stride,
                       gpu + (u64)row * p->row_bytes, (size_t)p->row_bytes);
            gcn_native_code_content_dirty(
                p->dest_addr, (u32)((u64)p->tiles_y * p->dest_stride));
        }
        s_vk.resident_pending_tex_count = 0;
    }
    s_vk.resident_copy_memcpy_tsc += __rdtsc() - t0;
    return 1;
}

static int resident_begin_commands(void) {
    if (s_vk.resident_recording)
        return 1;
    if (s_vk.resident_inflight == XFB_RING_SIZE &&
        !resident_submit_batch())
        return 0;
    s_vk.command_buffer = s_vk.command_buffers[s_vk.resident_inflight];
    if (vkResetCommandBuffer(s_vk.command_buffer, 0) != VK_SUCCESS)
        return 0;
    VkCommandBufferBeginInfo begin = {0};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(s_vk.command_buffer, &begin) != VK_SUCCESS)
        return 0;
    s_vk.resident_recording = 1;
    s_vk.gpu_query_count = 0;
    if (s_vk.gpu_stats)
        vkCmdResetQueryPool(s_vk.command_buffer, s_vk.query_pool,
                            0, GPU_QUERY_MAX);

    if (!s_vk.resident_efb_valid) {
        const u32 *color = NULL, *depth = NULL;
        u32 width = 0, height = 0;
        gx_raster_efb_data(&color, &depth, &width, &height);
        if (!color || !depth || width != EFB_WIDTH || height != EFB_HEIGHT)
            return 0;
        memcpy(s_vk.staging_map, color, (size_t)EFB_PLANE_BYTES);
        memcpy(s_vk.staging_map + EFB_PLANE_BYTES, depth,
               (size_t)EFB_PLANE_BYTES);

        VkImageMemoryBarrier upload[2] = {0};
        VkBufferImageCopy copies[2] = {0};
        for (u32 i = 0; i < 2; ++i) {
            upload[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            upload[i].srcAccessMask = s_vk.images_general ?
                (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                 VK_ACCESS_TRANSFER_READ_BIT) : 0u;
            upload[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            upload[i].oldLayout = s_vk.images_general ?
                VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
            upload[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            upload[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            upload[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            upload[i].image = s_vk.image[i];
            upload[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            upload[i].subresourceRange.levelCount = 1;
            upload[i].subresourceRange.layerCount = 1;
            copies[i].bufferOffset = EFB_PLANE_BYTES * i;
            copies[i].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copies[i].imageSubresource.layerCount = 1;
            copies[i].imageExtent = (VkExtent3D){EFB_WIDTH, EFB_HEIGHT, 1};
        }
        vkCmdPipelineBarrier(s_vk.command_buffer,
            s_vk.images_general ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT :
                                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, upload);
        for (u32 i = 0; i < 2; ++i)
            vkCmdCopyBufferToImage(s_vk.command_buffer, s_vk.staging,
                                   s_vk.image[i], VK_IMAGE_LAYOUT_GENERAL,
                                   1, &copies[i]);
        for (u32 i = 0; i < 2; ++i) {
            upload[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            upload[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                      VK_ACCESS_SHADER_WRITE_BIT;
            upload[i].oldLayout = upload[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        }
        vkCmdPipelineBarrier(s_vk.command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                             0, NULL, 0, NULL, 2, upload);
        s_vk.resident_efb_valid = 1;
    } else {
        VkImageMemoryBarrier ordered[2] = {0};
        for (u32 i = 0; i < 2; ++i) {
            ordered[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            ordered[i].srcAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                       VK_ACCESS_SHADER_WRITE_BIT |
                                       VK_ACCESS_TRANSFER_READ_BIT;
            ordered[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                       VK_ACCESS_SHADER_WRITE_BIT;
            ordered[i].oldLayout = ordered[i].newLayout =
                VK_IMAGE_LAYOUT_GENERAL;
            ordered[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ordered[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ordered[i].image = s_vk.image[i];
            ordered[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            ordered[i].subresourceRange.levelCount = 1;
            ordered[i].subresourceRange.layerCount = 1;
        }
        vkCmdPipelineBarrier(s_vk.command_buffer,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                             0, NULL, 0, NULL, 2, ordered);
    }
    return 1;
}

static int resident_emit_draw_batch(void) {
    if (!s_vk.resident_job_count)
        return 1;
    u32 packet_bytes = s_vk.resident_job_count * DRAW_PACKET_BYTES;
    u32 header_offset = s_vk.resident_draw_arena_used + packet_bytes;
    u32 index_offset = header_offset + TILE_HEADER_BYTES;
    u64 batch_end = (u64)index_offset +
                    (u64)s_vk.resident_tile_indices * sizeof(u32);
    if (batch_end > DRAW_JOB_BYTES)
        return 0;
    u32* headers = (u32*)(s_vk.staging_map + DRAW_JOB_OFFSET + header_offset);
    u32* indices = (u32*)(s_vk.staging_map + DRAW_JOB_OFFSET + index_offset);
    u32 cursor = 0;
    for (u32 tile = 0; tile < EFB_TILE_COUNT; ++tile) {
        u32 count = s_vk.resident_tile_count[tile];
        headers[tile * 2u] = cursor;
        headers[tile * 2u + 1u] = count;
        memcpy(indices + cursor, s_vk.resident_tile_jobs[tile],
               (size_t)count * sizeof(u32));
        cursor += count;
    }
    if (cursor != s_vk.resident_tile_indices || cursor > TILE_INDEX_CAPACITY)
        return 0;

    u32 gpu_query = gpu_time_begin(GPU_TIME_DRAW);
    vkCmdBindPipeline(s_vk.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                      s_vk.draw_f_pipeline);
    vkCmdBindDescriptorSets(s_vk.command_buffer,
                            VK_PIPELINE_BIND_POINT_COMPUTE,
                            s_vk.draw_f_pipeline_layout, 0, 1,
                            &s_vk.descriptor_set, 0, NULL);
    GxVkDrawPush push = {
        s_vk.resident_draw_arena_used / sizeof(u32), 1u,
        header_offset / sizeof(u32), index_offset / sizeof(u32),
        s_vk.general_debug_x, s_vk.general_debug_y
    };
    vkCmdPushConstants(s_vk.command_buffer, s_vk.draw_f_pipeline_layout,
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
    vkCmdDispatch(s_vk.command_buffer, EFB_TILE_WIDTH, EFB_TILE_HEIGHT, 1);

    VkImageMemoryBarrier ordered[2] = {0};
    for (u32 i = 0; i < 2u; ++i) {
        ordered[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        ordered[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        ordered[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                   VK_ACCESS_SHADER_WRITE_BIT;
        ordered[i].oldLayout = ordered[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        ordered[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ordered[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ordered[i].image = s_vk.image[i];
        ordered[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ordered[i].subresourceRange.levelCount = 1;
        ordered[i].subresourceRange.layerCount = 1;
    }
    vkCmdPipelineBarrier(s_vk.command_buffer,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                         0, NULL, 0, NULL, 2, ordered);
    gpu_time_end(gpu_query);

    memset(s_vk.resident_tile_count, 0, sizeof s_vk.resident_tile_count);
    s_vk.resident_tile_indices = 0;
    s_vk.resident_job_count = 0;
    s_vk.resident_draw_arena_used = ((u32)batch_end + 255u) & ~255u;
    return 1;
}

static int resident_submit_current(int signal_fence) {
    if (!s_vk.resident_recording)
        return 1;
    u64 t0 = __rdtsc();
    if (!resident_emit_draw_batch() || !flush_staging() ||
        vkEndCommandBuffer(s_vk.command_buffer) != VK_SUCCESS)
        return 0;
    VkSubmitInfo submit = {0};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &s_vk.command_buffer;
    VkFence fence = VK_NULL_HANDLE;
    if (signal_fence) {
        if (vkResetFences(s_vk.device, 1, &s_vk.resident_fence) != VK_SUCCESS)
            return 0;
        fence = s_vk.resident_fence;
    }
    if (vkQueueSubmit(s_vk.queue, 1, &submit, fence) != VK_SUCCESS)
        return 0;
    u64 t1 = __rdtsc();
    s_vk.resident_submit_cpu_tsc += t1 - t0;
    s_vk.images_general = 1;
    s_vk.resident_recording = 0;
    s_vk.resident_inflight++;
    s_vk.resident_batches++;
    return 1;
}

static int resident_submit_async(void) {
    return resident_submit_current(0);
}

/* Pass D root-cause hunt (docs/GX_GENERAL_TEV.md): print + clear the
 * general-TEV debug SSBO if the armed pixel was touched by the batch just
 * completed. Word layout matches gx_draw_f.comp's shade_pixel_general dbg
 * writes exactly (see the comments there); pack_abgr's x|y<<8|z<<16|w<<24
 * applied to (r,g,b,a) unpacks the same way here. */
static void gx_vk_dump_general_debug(void) {
    if (!invalidate_readback())
        return;
    u32* d = (u32*)(s_vk.readback_map + READBACK_GENERAL_DEBUG_OFFSET);
    if (d[0] != 0xDEADBEEFu)
        return;
    u32 program = d[1];
    fprintf(stderr,
        "gx_vulkan: [gpu-debug] program=%u xy=(%u,%u) wraw=%08X uv=(%08X,%08X) invw=%08X q=%08X "
        "fixed=(%08X,%08X) int=(%d,%d) linear=%u is/it=(%d,%d) is1/it1=(%d,%d) fs/ft=(%d,%d)\n",
        program, d[2], d[3], d[36], d[4], d[5], d[6], d[7], d[8], d[9],
        (int32_t)d[10], (int32_t)d[11], d[12],
        (int32_t)d[13], (int32_t)d[14], (int32_t)d[15], (int32_t)d[16],
        (int32_t)d[17], (int32_t)d[18]);
    fprintf(stderr,
        "gx_vulkan: [gpu-debug] v00=%08X v10=%08X v01=%08X v11=%08X texel=%08X "
        "tex_color=%08X ras_color=%08X\n",
        d[19], d[20], d[21], d[22], d[23], d[24], d[25]);
    fprintf(stderr,
        "gx_vulkan: [gpu-debug] combiner_out(r,g,b,a packed)=%08X pf=%u old_efb=%08X "
        "dstv=%08X pre_dither=%08X post_dither=%08X stored=%08X\n",
        d[28], d[29], d[30], d[32], d[33], d[34], d[35]);
    memset(d, 0, GENERAL_DEBUG_BYTES);
}

static int resident_submit_batch(void) {
    int had_pending = s_vk.resident_pending_count != 0u;
    u64 t0 = __rdtsc();
    int signaled = s_vk.resident_recording;
    if (!resident_submit_current(signaled))
        return 0;
    if (!signaled && s_vk.resident_inflight) {
        if (vkResetFences(s_vk.device, 1, &s_vk.resident_fence) != VK_SUCCESS)
            return 0;
        VkSubmitInfo empty = {0};
        empty.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        if (vkQueueSubmit(s_vk.queue, 1, &empty, s_vk.resident_fence) != VK_SUCCESS)
            return 0;
        signaled = 1;
    }
    if (signaled && !resident_wait_fence())
        return 0;
    if (s_vk.general_debug_armed && signaled)
        gx_vk_dump_general_debug();
    u64 t2 = __rdtsc();
    if (s_vk.gpu_stats && s_vk.gpu_query_count) {
        u64 timestamps[GPU_QUERY_MAX] = {0};
        if (vkGetQueryPoolResults(s_vk.device, s_vk.query_pool, 0,
                                  s_vk.gpu_query_count,
                                  sizeof timestamps, timestamps, sizeof(u64),
                                  VK_QUERY_RESULT_64_BIT |
                                  VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
            for (u32 q = 0; q + 1u < s_vk.gpu_query_count; q += 2u) {
                u32 kind = s_vk.gpu_query_kind[q / 2u];
                if (kind < GPU_TIME_COUNT) {
                    s_vk.gpu_time_ns[kind] +=
                        (double)(timestamps[q + 1u] - timestamps[q]) *
                        (double)s_vk.timestamp_period;
                    s_vk.gpu_time_calls[kind]++;
                }
            }
        }
    }
    s_vk.resident_wait_tsc += t2 - t0;
    if (!resident_materialize_pending())
        return 0;
    if (had_pending)
        s_vk.resident_copy_total_tsc += __rdtsc() - t0;
    s_vk.resident_inflight = 0;
    s_vk.resident_draw_arena_used = 0;
    s_vk.command_buffer = s_vk.command_buffers[0];
    return 1;
}

static int ensure_resident_texture(GxVkTextureEntry* texture) {
    if (!texture)
        return 1;
    if (!texture->gpu_dirty && texture->gpu_capacity >= texture->length)
        return 1;
    /* A content update may target bytes referenced by already-recorded draws.
     * Complete those draws before reusing the arena range. */
    if ((s_vk.resident_recording || s_vk.resident_inflight) &&
        !resident_submit_batch())
        return 0;
    if (texture->gpu_capacity < texture->length) {
        u32 offset = (s_vk.texture_arena_used + 255u) & ~255u;
        u32 capacity = (texture->length + 255u) & ~255u;
        if ((u64)offset + capacity > TEXTURE_SHADOW_BYTES)
            return 0;
        texture->gpu_offset = offset;
        texture->gpu_capacity = capacity;
        s_vk.texture_arena_used = offset + capacity;
    }
    memcpy(s_vk.staging_map + TEXTURE_SHADOW_OFFSET + texture->gpu_offset,
           texture->bytes, texture->length);
    texture->gpu_dirty = 0;
    return 1;
}

/* Phase 1a general TEV program (docs/GX_GENERAL_TEV.md): mirrors
 * ensure_resident_texture for TLUT bytes. */
static int ensure_resident_tlut(GxVkTlutEntry* entry) {
    if (!entry)
        return 1;
    if (!entry->gpu_dirty && entry->gpu_capacity >= entry->length)
        return 1;
    if ((s_vk.resident_recording || s_vk.resident_inflight) &&
        !resident_submit_batch())
        return 0;
    if (entry->gpu_capacity < entry->length) {
        u32 offset = (s_vk.tlut_arena_used + 255u) & ~255u;
        u32 capacity = (entry->length + 255u) & ~255u;
        if ((u64)offset + capacity > TLUT_SHADOW_BYTES)
            return 0;
        entry->gpu_offset = offset;
        entry->gpu_capacity = capacity;
        s_vk.tlut_arena_used = offset + capacity;
    }
    memcpy(s_vk.staging_map + TLUT_SHADOW_OFFSET + entry->gpu_offset,
           entry->bytes, entry->length);
    entry->gpu_dirty = 0;
    return 1;
}

static int resident_record_draw(const GxRasterTriangleJob* job) {
    if (job->fused_program < 1u ||
        job->fused_program > GX_VK_DRAW_PROGRAM_COUNT)
        return 0;
    if (job->scan.minx < 0 || job->scan.miny < 0 ||
        job->scan.maxx <= job->scan.minx ||
        job->scan.maxy <= job->scan.miny ||
        job->scan.maxx > (int)EFB_WIDTH ||
        job->scan.maxy > (int)EFB_HEIGHT)
        return 0;
    GxVkTextureEntry* texture = NULL;
    GxVkGeneralTex gen = {0};
    if (!resolve_fused_texture(job, &texture, &gen) ||
        !ensure_resident_texture(gen.tex[0]) ||
        !ensure_resident_texture(gen.tex[1]) ||
        !ensure_resident_tlut(gen.tlut[0]) ||
        !ensure_resident_tlut(gen.tlut[1]))
        return 0;

    u32 tx0 = (u32)job->scan.minx / 16u;
    u32 ty0 = (u32)job->scan.miny / 16u;
    u32 tx1 = (u32)(job->scan.maxx - 1) / 16u;
    u32 ty1 = (u32)(job->scan.maxy - 1) / 16u;
    if (tx1 >= EFB_TILE_WIDTH) tx1 = EFB_TILE_WIDTH - 1u;
    if (ty1 >= EFB_TILE_HEIGHT) ty1 = EFB_TILE_HEIGHT - 1u;
    u32 needed = (tx1 - tx0 + 1u) * (ty1 - ty0 + 1u);
    u64 needed_end = (u64)s_vk.resident_draw_arena_used +
                     (u64)(s_vk.resident_job_count + 1u) * DRAW_PACKET_BYTES +
                     TILE_HEADER_BYTES +
                     (u64)(s_vk.resident_tile_indices + needed) * sizeof(u32);
    int full = needed_end > DRAW_JOB_BYTES ||
               (u64)s_vk.resident_tile_indices + needed > TILE_INDEX_CAPACITY;
    for (u32 ty = ty0; !full && ty <= ty1; ++ty)
        for (u32 tx = tx0; tx <= tx1; ++tx)
            if (s_vk.resident_tile_count[ty * EFB_TILE_WIDTH + tx] >=
                TILE_JOBS_PER_TILE) {
                full = 1;
                break;
            }
    if (full) {
        if (!s_vk.resident_job_count || !resident_submit_batch())
            return 0;
    }
    if (!resident_begin_commands())
        return 0;

    u32 packet_index = s_vk.resident_job_count;
    u32* packet = (u32*)(s_vk.staging_map + DRAW_JOB_OFFSET +
                         s_vk.resident_draw_arena_used +
                         (u64)packet_index * DRAW_PACKET_BYTES);
    snapshot_fused_draw(job, packet, texture, &gen);
    if (texture)
        packet[99] = texture->gpu_offset;
    if (gen.tlut[0])
        packet[252] = gen.tlut[0]->gpu_offset;
    if (gen.tex[1])
        packet[255] = gen.tex[1]->gpu_offset;
    if (gen.tlut[1])
        packet[254] = gen.tlut[1]->gpu_offset;
    ++s_vk.resident_job_count;
    for (u32 ty = ty0; ty <= ty1; ++ty) {
        for (u32 tx = tx0; tx <= tx1; ++tx) {
            u32 tile = ty * EFB_TILE_WIDTH + tx;
            s_vk.resident_tile_jobs[tile][s_vk.resident_tile_count[tile]++] =
                packet_index;
            s_vk.resident_tile_indices++;
        }
    }
    s_vk.resident_triangles++;
    return 1;
}

static int submit_fused_draw(const GxRasterTriangleJob* job) {
    const u32 *color = NULL, *depth = NULL;
    u32 width = 0, height = 0;
    gx_raster_efb_data(&color, &depth, &width, &height);
    if (!color || !depth || width != EFB_WIDTH || height != EFB_HEIGHT ||
        s_vk.prepared || s_vk.draw_validation_pending)
        return 0;

    memcpy(s_vk.staging_map, color, (size_t)EFB_PLANE_BYTES);
    memcpy(s_vk.staging_map + EFB_PLANE_BYTES, depth,
           (size_t)EFB_PLANE_BYTES);
    u32* words = (u32*)(s_vk.staging_map + DRAW_JOB_OFFSET);
    GxVkTextureEntry* texture = NULL;
    GxVkGeneralTex gen = {0};
    if (!resolve_fused_texture(job, &texture, &gen))
        return 0;
    snapshot_fused_draw(job, words, texture, &gen);
    /* This single-draw validate path has no arena/cache (unlike the resident
     * batched path) -- it always re-uploads at a fixed spot for exactly this
     * one triangle. General TEV (program 31, docs/GX_GENERAL_TEV.md) can
     * need a second texture + up to two TLUTs, so slot 1 / the TLUTs are
     * placed after slot 0 in the same arena rather than at offset 0. */
    u32 tex_arena_used = 0;
    if (texture) {
        memcpy(s_vk.staging_map + TEXTURE_SHADOW_OFFSET,
               texture->bytes, texture->length);
        words[99] = 0u;
        tex_arena_used = (texture->length + 255u) & ~255u;
    }
    if (gen.tex[1]) {
        u32 off = tex_arena_used;
        if ((u64)off + gen.tex[1]->length > TEXTURE_SHADOW_BYTES)
            return 0;
        memcpy(s_vk.staging_map + TEXTURE_SHADOW_OFFSET + off,
               gen.tex[1]->bytes, gen.tex[1]->length);
        words[255] = off;
    }
    u32 tlut_arena_used = 0;
    if (gen.tlut[0]) {
        memcpy(s_vk.staging_map + TLUT_SHADOW_OFFSET,
               gen.tlut[0]->bytes, gen.tlut[0]->length);
        words[252] = 0u;
        tlut_arena_used = (gen.tlut[0]->length + 255u) & ~255u;
    }
    if (gen.tlut[1]) {
        u32 off = tlut_arena_used;
        if ((u64)off + gen.tlut[1]->length > TLUT_SHADOW_BYTES)
            return 0;
        memcpy(s_vk.staging_map + TLUT_SHADOW_OFFSET + off,
               gen.tlut[1]->bytes, gen.tlut[1]->length);
        words[254] = off;
    }
    if (!s_vk.staging_coherent) {
        VkMappedMemoryRange range = {0};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = s_vk.staging_memory;
        range.size = VK_WHOLE_SIZE;
        if (vkFlushMappedMemoryRanges(s_vk.device, 1, &range) != VK_SUCCESS)
            return 0;
    }

    if (vkResetCommandBuffer(s_vk.command_buffer, 0) != VK_SUCCESS)
        return 0;
    VkCommandBufferBeginInfo begin = {0};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(s_vk.command_buffer, &begin) != VK_SUCCESS)
        return 0;

    VkImageMemoryBarrier upload[2] = {0};
    VkBufferImageCopy copy[2] = {0};
    for (u32 i = 0; i < 2u; ++i) {
        upload[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        upload[i].srcAccessMask = s_vk.images_general ?
            (VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT) : 0u;
        upload[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        upload[i].oldLayout = s_vk.images_general ? VK_IMAGE_LAYOUT_GENERAL :
                                                   VK_IMAGE_LAYOUT_UNDEFINED;
        upload[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        upload[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        upload[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        upload[i].image = s_vk.image[i];
        upload[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        upload[i].subresourceRange.levelCount = 1;
        upload[i].subresourceRange.layerCount = 1;
        copy[i].bufferOffset = (VkDeviceSize)i * EFB_PLANE_BYTES;
        copy[i].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy[i].imageSubresource.layerCount = 1;
        copy[i].imageExtent = (VkExtent3D){EFB_WIDTH, EFB_HEIGHT, 1};
    }
    vkCmdPipelineBarrier(s_vk.command_buffer,
        s_vk.images_general ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT :
                              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, upload);
    for (u32 i = 0; i < 2u; ++i)
        vkCmdCopyBufferToImage(s_vk.command_buffer, s_vk.staging, s_vk.image[i],
                               VK_IMAGE_LAYOUT_GENERAL, 1, &copy[i]);
    for (u32 i = 0; i < 2u; ++i) {
        upload[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        upload[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                  VK_ACCESS_SHADER_WRITE_BIT;
        upload[i].oldLayout = upload[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    vkCmdPipelineBarrier(s_vk.command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                         0, NULL, 0, NULL, 2, upload);
    vkCmdBindPipeline(s_vk.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                      s_vk.draw_f_pipeline);
    vkCmdBindDescriptorSets(s_vk.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            s_vk.draw_f_pipeline_layout, 0, 1,
                            &s_vk.descriptor_set, 0, NULL);
    GxVkDrawPush draw_push = {0};
    draw_push.dbg_x = s_vk.general_debug_x;
    draw_push.dbg_y = s_vk.general_debug_y;
    vkCmdPushConstants(s_vk.command_buffer, s_vk.draw_f_pipeline_layout,
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof draw_push,
                       &draw_push);
    u32 draw_width = (u32)(job->scan.maxx - job->scan.minx);
    u32 draw_height = (u32)(job->scan.maxy - job->scan.miny);
    vkCmdDispatch(s_vk.command_buffer, (draw_width + 15u) / 16u,
                  (draw_height + 15u) / 16u, 1);

    for (u32 i = 0; i < 2u; ++i) {
        upload[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        upload[i].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    }
    vkCmdPipelineBarrier(s_vk.command_buffer,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 0, NULL, 2, upload);
    copy[0].bufferOffset = READBACK_COLOR_OFFSET;
    copy[1].bufferOffset = READBACK_DEPTH_OFFSET;
    for (u32 i = 0; i < 2u; ++i)
        vkCmdCopyImageToBuffer(s_vk.command_buffer, s_vk.image[i],
                               VK_IMAGE_LAYOUT_GENERAL, s_vk.readback, 1,
                               &copy[i]);
    if (vkEndCommandBuffer(s_vk.command_buffer) != VK_SUCCESS)
        return 0;
    VkSubmitInfo submit = {0};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &s_vk.command_buffer;
    if (vkQueueSubmit(s_vk.queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS ||
        vkQueueWaitIdle(s_vk.queue) != VK_SUCCESS)
        return 0;
    if (!invalidate_readback())
        return 0;
    if (s_vk.general_debug_armed)
        gx_vk_dump_general_debug();
    s_vk.images_general = 1;
    s_vk.draw_validation_program = job->fused_program;
    s_vk.draw_validation_pending = 1;
    return 1;
}

static u32 texture_encoded_size(u32 format, u32 width, u32 height) {
    u32 block_w, block_h, bytes;
    switch (format) {
    case 0x0: case 0x8:             /* I4 / C4 */
        block_w = 8; block_h = 8; bytes = 32; break;
    case 0x1: case 0x2: case 0x9:   /* I8 / IA4 / C8 */
        block_w = 8; block_h = 4; bytes = 32; break;
    case 0x3: case 0x4: case 0x5: case 0xA: /* IA8/RGB565/RGB5A3/C14X2 */
        block_w = 4; block_h = 4; bytes = 32; break;
    case 0x6:                       /* RGBA8, two 32-byte planes */
        block_w = 4; block_h = 4; bytes = 64; break;
    case 0xE:                       /* CMPR, four DXT blocks */
        block_w = 8; block_h = 8; bytes = 32; break;
    default:
        return 0;
    }
    u64 blocks_x = (width + block_w - 1u) / block_w;
    u64 blocks_y = (height + block_h - 1u) / block_h;
    u64 total = blocks_x * blocks_y * bytes;
    return total <= UINT32_MAX ? (u32)total : 0u;
}

/* True if [address, address+length) overlaps the guest-RAM destination of
 * any not-yet-materialized pending copy (XFB ring or EFB->texture ring).
 * Texture snapshots read guest RAM at record time; a draw sampling a
 * pending copy's destination must materialize first or it uploads the
 * pre-copy bytes. EFB->texture copies exist precisely to be sampled by a
 * later draw, so this is a live ordering hazard, not a theoretical one. */
static int resident_pending_ram_overlap(u32 address, u32 length) {
    u64 lo = address, hi = (u64)address + length;
    for (u32 i = 0; i < s_vk.resident_pending_count; ++i) {
        const GxVkPendingXfb* p = &s_vk.resident_pending[i];
        u64 dlo = p->address, dhi = dlo + (u64)p->height * p->stride;
        if (lo < dhi && dlo < hi)
            return 1;
    }
    for (u32 i = 0; i < s_vk.resident_pending_tex_count; ++i) {
        const GxVkPendingTexCopy* p = &s_vk.resident_pending_tex[i];
        u64 dlo = p->dest_addr, dhi = dlo + (u64)p->tiles_y * p->dest_stride;
        if (lo < dhi && dlo < hi)
            return 1;
    }
    return 0;
}

static int validate_texture_binding(const u32* bp, const u8* ram, u32 ram_size,
                                    u32 unit) {
    u32 image0 = bp[0x88 + unit];
    u32 image3 = bp[0x94 + unit];
    u32 width = (image0 & 0x3ffu) + 1u;
    u32 height = ((image0 >> 10) & 0x3ffu) + 1u;
    u32 format = (image0 >> 20) & 0xfu;
    u32 address = ((image3 & 0x00ffffffu) << 5) & 0x1fffffffu;
    u32 length = texture_encoded_size(format, width, height);
    if (!length || !ram || (u64)address + length > ram_size) {
        fprintf(stderr,
                "gx_vulkan: texture snapshot outside exact cache scope "
                "(unit=%u fmt=%u %ux%u addr=%08X len=%u ram=%u)\n",
                unit, format, width, height, address, length, ram_size);
        return 0;
    }

    /* Materialize pending copies whose destination this snapshot would
     * read -- both the identity memcmp below and the upload memcpy consume
     * guest RAM, and a pending copy's bytes only land there at batch
     * completion (resident_materialize_pending). */
    if (resident_pending_ram_overlap(address, length) &&
        !resident_submit_batch()) {
        fprintf(stderr,
                "gx_vulkan: pending-copy flush before texture snapshot "
                "failed (unit=%u addr=%08X len=%u)\n", unit, address, length);
        return 0;
    }

    GxVkTextureEntry* entry = NULL;
    GxVkTextureEntry* victim = &s_vk.texture_cache[0];
    for (u32 i = 0; i < GX_VK_TEXTURE_CACHE_ENTRIES; ++i) {
        GxVkTextureEntry* candidate = &s_vk.texture_cache[i];
        if (candidate->used && candidate->address == address &&
            candidate->format == format && candidate->width == width &&
            candidate->height == height && candidate->length == length) {
            entry = candidate;
            break;
        }
        if (!candidate->used ||
            (victim->used && candidate->stamp < victim->stamp))
            victim = candidate;
    }
    s_vk.texture_stamp++;
    if (entry) {
        s_vk.texture_bytes_compared += length;
        if (memcmp(entry->bytes, ram + address, length) == 0) {
            entry->stamp = s_vk.texture_stamp;
            s_vk.texture_hits++;
            return 1;
        }
    } else {
        entry = victim;
    }

    u8* resized = (u8*)realloc(entry->bytes, length);
    if (!resized)
        return 0;
    entry->bytes = resized;
    memcpy(entry->bytes, ram + address, length);
    entry->used = 1;
    entry->address = address;
    entry->format = format;
    entry->width = width;
    entry->height = height;
    entry->length = length;
    entry->stamp = s_vk.texture_stamp;
    entry->gpu_dirty = 1;
    s_vk.texture_misses++;
    s_vk.texture_bytes_changed += length;
    return 1;
}

/* Phase 1a general TEV program (docs/GX_GENERAL_TEV.md): TLUT bytes for
 * C4/C8, content-compared every draw exactly like validate_texture_binding
 * does for texture bytes -- a stale TLUT is just as wrong as a stale
 * texture. Source is modeled TMEM (gcn_gx_tmem(), gx_raster.c:5736-5743),
 * not guest RAM: TX_SETTLUT's tmem_offset<<9 is always within the 1MB TMEM
 * array (same bound gx_raster.c's decode_texel comment cites), so no OOB
 * check is needed here either. */
static int validate_tlut_binding(u32 tmem_offset, u32 length) {
    const u8* tmem = gcn_gx_tmem();
    GxVkTlutEntry* entry = NULL;
    GxVkTlutEntry* victim = &s_vk.tlut_cache[0];
    for (u32 i = 0; i < GX_VK_TLUT_CACHE_ENTRIES; ++i) {
        GxVkTlutEntry* candidate = &s_vk.tlut_cache[i];
        if (candidate->used && candidate->tmem_offset == tmem_offset &&
            candidate->length == length) {
            entry = candidate;
            break;
        }
        if (!candidate->used ||
            (victim->used && candidate->stamp < victim->stamp))
            victim = candidate;
    }
    s_vk.tlut_stamp++;
    if (entry) {
        if (memcmp(entry->bytes, tmem + tmem_offset, length) == 0) {
            entry->stamp = s_vk.tlut_stamp;
            return 1;
        }
    } else {
        entry = victim;
    }
    u8* resized = (u8*)realloc(entry->bytes, length);
    if (!resized)
        return 0;
    entry->bytes = resized;
    memcpy(entry->bytes, tmem + tmem_offset, length);
    entry->used = 1;
    entry->tmem_offset = tmem_offset;
    entry->length = length;
    entry->stamp = s_vk.tlut_stamp;
    entry->gpu_dirty = 1;
    return 1;
}

static int validate_draw_textures(const u32* bp, const u8* ram, u32 ram_size) {
    u32 num_texgens = bp[0x00] & 0xfu;
    u32 last_stage = (bp[0x00] >> 10) & 0xfu;
    u32 seen = 0;
    if (!num_texgens)
        return 1;
    for (u32 stage = 0; stage <= last_stage; ++stage) {
        u32 order = bp[0x28 + (stage >> 1)];
        u32 odd = stage & 1u;
        u32 enabled = odd ? ((order >> 18) & 1u) : ((order >> 6) & 1u);
        u32 unit = odd ? ((order >> 12) & 7u) : (order & 7u);
        if (enabled && !(seen & (1u << unit))) {
            if (!validate_texture_binding(bp, ram, ram_size, unit))
                return 0;
            /* C4/C8 units also need their TLUT bytes content-compared every
             * draw (docs/GX_GENERAL_TEV.md phase 1a). */
            u32 image0 = bp[0x88 + unit];
            u32 format = (image0 >> 20) & 0xfu;
            if (format == 8u || format == 9u) {
                u32 tlut_reg = (unit < 4u) ? bp[0x98 + unit] : bp[0xB8 + (unit - 4u)];
                u32 tmem_offset = (tlut_reg & 0x3FFu) << 9;
                u32 tlut_len = (format == 8u) ? 32u : 512u;
                if (!validate_tlut_binding(tmem_offset, tlut_len))
                    return 0;
            }
            seen |= 1u << unit;
        }
    }
    return 1;
}

int gx_vulkan_shadow_begin_draw(const u32* bp, const u8* ram, u32 ram_size) {
    if (!s_vk.device || !bp || !ram || s_vk.draw_active)
        return 0;
    memcpy(s_vk.draw_bp, bp, sizeof s_vk.draw_bp);
    s_vk.draw_ram = ram;
    s_vk.draw_ram_size = ram_size;
    s_vk.draw_textures_validated = 0;
    s_vk.draw_active = 1;
    s_vk.draws++;
    return 1;
}

int gx_vulkan_shadow_triangle(const GxRasterTriangleJob* job,
                              int after_software) {
    if (!s_vk.draw_active || !job ||
        job->fused_program > GX_VK_DRAW_PROGRAM_COUNT)
        return 0;
    if (after_software) {
        if (!s_vk.draw_validation_pending)
            return 1;
        const u32 *color = NULL, *depth = NULL;
        gx_raster_efb_data(&color, &depth, NULL, NULL);
        s_vk.draw_validation_pending = 0;
        const u32* gpu = (const u32*)(s_vk.readback_map +
                                      READBACK_COLOR_OFFSET);
        char label[24];
        u32 program = s_vk.draw_validation_program;
        snprintf(label, sizeof label, "draw-%s color", gx_vk_program_label(program));
        if (!compare_plane(label, color, gpu)) {
            if (program == 31u) {
                fprintf(stderr,
                        "gx_vulkan: [general-debug] genmode=%06X TRef0=%06X "
                        "cc0=%06X ac0=%06X ksel0=%06X at=%06X bm=%06X\n",
                        s_vk.draw_bp[0x00], s_vk.draw_bp[0x28],
                        s_vk.draw_bp[0xC0], s_vk.draw_bp[0xC1],
                        s_vk.draw_bp[0xF6], s_vk.draw_bp[0xF3], s_vk.draw_bp[0x41]);
                for (u32 u = 0; u < 8; ++u) {
                    u32 image0 = s_vk.draw_bp[0x88 + u];
                    u32 fmt = (image0 >> 20) & 0xfu;
                    if (fmt || image0)
                        fprintf(stderr,
                                "gx_vulkan: [general-debug] unit=%u fmt=%u "
                                "w=%u h=%u mode0=%06X\n",
                                u, fmt, (image0 & 0x3ffu) + 1u,
                                ((image0 >> 10) & 0x3ffu) + 1u,
                                s_vk.draw_bp[0x80 + u]);
                }
            }
            return 0;
        }
        /* General (31, docs/GX_GENERAL_TEV.md) can also early-Z test/write
         * depth (shade_pixel_general derives zt_early from raw PEControl
         * itself, unlike the matched programs' pinned constants), so its
         * depth plane needs the same comparison K/L/X already get. */
        if (program == 11u || program == 12u || program == 24u || program == 31u) {
            const u32* gpu_depth = (const u32*)(s_vk.readback_map +
                                                READBACK_DEPTH_OFFSET);
            char depth_label[24];
            snprintf(depth_label, sizeof depth_label, "draw-%s depth", gx_vk_program_label(program));
            if (!compare_plane(depth_label, depth, gpu_depth))
                return 0;
        }
        s_vk.draw_validate_remaining[program]--;
        s_vk.draw_validations[program]++;
        s_vk.draw_validation_program = 0;
        return 1;
    }
    if (job->pixel_format != (s_vk.draw_bp[0x43] & 7u)) {
        fprintf(stderr,
                "gx_vulkan: triangle pixel-format snapshot mismatch (%u vs %u)\n",
                job->pixel_format, s_vk.draw_bp[0x43] & 7u);
        return 0;
    }
    s_vk.triangles++;
    s_vk.fused_triangles[job->fused_program]++;
    if (job->num_texgens && !s_vk.draw_textures_validated) {
        if (!validate_draw_textures(s_vk.draw_bp, s_vk.draw_ram,
                                    s_vk.draw_ram_size))
            return 0;
        s_vk.draw_textures_validated = 1;
    }
    if (s_vk.draw_validate_remaining[job->fused_program] &&
        !submit_fused_draw(job)) {
        fprintf(stderr,
                "gx_vulkan: fused-program-%s validation submit failed\n",
                gx_vk_program_label(job->fused_program));
        return 0;
    }
    return 1;
}

int gx_vulkan_shadow_end_draw(void) {
    if (!s_vk.draw_active || s_vk.draw_validation_pending)
        return 0;
    s_vk.draw_active = 0;
    s_vk.draw_ram = NULL;
    s_vk.draw_ram_size = 0;
    return 1;
}

static void corun_compare_planes(const u32* sw_color, const u32* sw_depth) {
    static const char* plane_name[2] = {"color", "depth"};
    const u32* sw[2] = {sw_color, sw_depth};
    const u32* gpu[2] = {
        (const u32*)(s_vk.readback_map + READBACK_COLOR_OFFSET),
        (const u32*)(s_vk.readback_map + READBACK_DEPTH_OFFSET)
    };
    s_corun_checks++;
    for (u32 plane = 0; plane < 2u; ++plane) {
        u32 count = 0, got = 0, want = 0;
        int fx = -1, fy = -1;
        int minx = EFB_WIDTH, miny = EFB_HEIGHT, maxx = -1, maxy = -1;
        for (u32 y = 0; y < EFB_HEIGHT; ++y) {
            const u32* s = sw[plane] + (size_t)y * EFB_WIDTH;
            const u32* g = gpu[plane] + (size_t)y * EFB_WIDTH;
            for (u32 x = 0; x < EFB_WIDTH; ++x) {
                if (s[x] == g[x])
                    continue;
                if (!count) { fx = (int)x; fy = (int)y; got = g[x]; want = s[x]; }
                count++;
                if ((int)x < minx) minx = (int)x;
                if ((int)x > maxx) maxx = (int)x;
                if ((int)y < miny) miny = (int)y;
                if ((int)y > maxy) maxy = (int)y;
            }
        }
        if (!count)
            continue;
        s_corun_hits++;
        corun_census_add(
            s_vk.corun_tile_programs[(u32)(fy / 16) * EFB_TILE_WIDTH +
                                     (u32)(fx / 16)], plane, count);
        if (s_corun_hits <= 64u || (s_corun_hits & 255u) == 0u) {
            u32 first_tile = (u32)(fy / 16) * EFB_TILE_WIDTH + (u32)(fx / 16);
            u32 bbox_mask = 0;
            for (int ty = miny / 16; ty <= maxy / 16; ++ty)
                for (int tx = minx / 16; tx <= maxx / 16; ++tx)
                    bbox_mask |=
                        s_vk.corun_tile_programs[ty * (int)EFB_TILE_WIDTH + tx];
            fprintf(stderr,
                    "gx_vulkan: CO-RUN DIVERGENCE #%llu %s at sync %llu "
                    "frame %llu: %u px, first (%d,%d) tile %u progmask=%08X "
                    "bboxmask=%08X gpu=%08X sw=%08X bbox=[%d,%d..%d,%d] "
                    "batches=%llu fallbacks=%llu\n",
                    (unsigned long long)s_corun_hits, plane_name[plane],
                    (unsigned long long)s_corun_checks,
                    (unsigned long long)gcn_gx_frame_count(), count, fx, fy,
                    first_tile, s_vk.corun_tile_programs[first_tile],
                    bbox_mask, got, want, minx, miny, maxx, maxy,
                    (unsigned long long)s_vk.resident_batches,
                    (unsigned long long)s_vk.resident_fallbacks);
        }
    }
    /* The mask window is "programs drawn since the last compare". */
    memset(s_vk.corun_tile_programs, 0, sizeof s_vk.corun_tile_programs);
}

static int resident_sync_to_software(void) {
    if (!s_vk.resident_efb_valid)
        return 1;
    if (!resident_begin_commands() || !resident_emit_draw_batch())
        return 0;

    VkImageMemoryBarrier download[2] = {0};
    VkBufferImageCopy copies[2] = {0};
    for (u32 i = 0; i < 2; ++i) {
        download[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        download[i].srcAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                    VK_ACCESS_SHADER_WRITE_BIT;
        download[i].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        download[i].oldLayout = download[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        download[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        download[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        download[i].image = s_vk.image[i];
        download[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        download[i].subresourceRange.levelCount = 1;
        download[i].subresourceRange.layerCount = 1;
        copies[i].bufferOffset = i ? READBACK_DEPTH_OFFSET :
                                     READBACK_COLOR_OFFSET;
        copies[i].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copies[i].imageSubresource.layerCount = 1;
        copies[i].imageExtent = (VkExtent3D){EFB_WIDTH, EFB_HEIGHT, 1};
    }
    vkCmdPipelineBarrier(s_vk.command_buffer,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 0, NULL, 2, download);
    for (u32 i = 0; i < 2; ++i)
        vkCmdCopyImageToBuffer(s_vk.command_buffer, s_vk.image[i],
                               VK_IMAGE_LAYOUT_GENERAL, s_vk.readback,
                               1, &copies[i]);
    VkBufferMemoryBarrier to_host = {0};
    to_host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    to_host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_host.buffer = s_vk.readback;
    to_host.offset = READBACK_COLOR_OFFSET;
    to_host.size = 2u * EFB_PLANE_BYTES;
    vkCmdPipelineBarrier(s_vk.command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0,
                         0, NULL, 1, &to_host, 0, NULL);
    if (!resident_submit_batch() || !invalidate_readback())
        return 0;
    u32 *color = NULL, *depth = NULL;
    gx_raster_efb_data_mutable(&color, &depth, NULL, NULL);
    if (!color || !depth)
        return 0;
    /* Co-run: software already holds every triangle, so the downloaded GPU
     * planes must match it byte-for-byte here. Compare before the overwrite
     * below (which then makes the two sides identical again, so each logged
     * divergence is one fresh corruption event, not an echo). */
    if (s_corun == 1)
        corun_compare_planes(color, depth);
    memcpy(color, s_vk.readback_map + READBACK_COLOR_OFFSET,
           (size_t)EFB_PLANE_BYTES);
    memcpy(depth, s_vk.readback_map + READBACK_DEPTH_OFFSET,
           (size_t)EFB_PLANE_BYTES);
    return 1;
}

int gx_vulkan_resident_sync_to_software(void) {
    return resident_sync_to_software();
}

int gx_vulkan_resident_triangle(const GxRasterTriangleJob* job,
                                int after_software) {
    if (!s_vk.resident_mode || !s_vk.draw_active || !job)
        return -1;
    if (after_software)
        return 0;
    if (job->pixel_format != (s_vk.draw_bp[0x43] & 7u))
        return -1;
    s_vk.triangles++;
    if (job->fused_program <= GX_VK_DRAW_PROGRAM_COUNT)
        s_vk.fused_triangles[job->fused_program]++;

    int supported = job->fused_program >= 1u &&
                    job->fused_program <= GX_VK_DRAW_PROGRAM_COUNT;
    if (supported && job->num_texgens && !s_vk.draw_textures_validated) {
        supported = validate_draw_textures(s_vk.draw_bp, s_vk.draw_ram,
                                           s_vk.draw_ram_size);
        s_vk.draw_textures_validated = supported;
    }
    if (supported)
        supported = resident_record_draw(job);

    /* Co-run tile census: record which program actually ends up responsible
     * for this tile's GPU-side pixels, not which program the triangle
     * nominally is. A GPU-eligible program (1..30) whose resident_record_draw
     * call above FAILED (tile/arena capacity, texture rejection, ...) falls
     * all the way through to the software-fallback path below exactly like
     * program 0 (general/unmatched) always does -- the GPU never queues, let
     * alone draws, this triangle anywhere. Marking the tile with that
     * program's bit regardless (the previous behavior, done unconditionally
     * before `supported` was even known) let a corun compare blame a
     * still-queued program's shader for a mismatch that was actually just an
     * expected, synchronized software-only draw: the tile's real story that
     * frame was "nothing GPU-side touched this pixel", which bit 0 already
     * means for every plain software triangle. Gating on the final
     * `supported` value makes the two cases (drew on GPU vs. fell back)
     * report identically instead of the fallback case impersonating the GPU
     * program it failed to become. */
    if (s_corun == 1 && job->scan.maxx > job->scan.minx &&
        job->scan.maxy > job->scan.miny &&
        job->scan.minx >= 0 && job->scan.miny >= 0) {
        u32 bit = 1u << (supported && job->fused_program <= GX_VK_DRAW_PROGRAM_COUNT ?
                         job->fused_program : 0u);
        u32 tx0 = (u32)job->scan.minx / 16u;
        u32 ty0 = (u32)job->scan.miny / 16u;
        u32 tx1 = (u32)(job->scan.maxx - 1) / 16u;
        u32 ty1 = (u32)(job->scan.maxy - 1) / 16u;
        if (tx1 >= EFB_TILE_WIDTH) tx1 = EFB_TILE_WIDTH - 1u;
        if (ty1 >= EFB_TILE_HEIGHT) ty1 = EFB_TILE_HEIGHT - 1u;
        for (u32 ty = ty0; ty <= ty1; ++ty)
            for (u32 tx = tx0; tx <= tx1; ++tx)
                s_vk.corun_tile_programs[ty * EFB_TILE_WIDTH + tx] |= bit;
    }

    if (supported)
        /* Co-run: the GPU batch keeps the triangle, but report it unhandled
         * so the software scan rasterizes it too — that reference is what
         * corun_compare_planes() diffs against at the next fallback sync. */
        return s_corun == 1 ? 0 : 1;

    if (!resident_sync_to_software())
        return -1;
    s_vk.resident_efb_valid = 0;
    s_vk.resident_fallbacks++;
    if (s_vk.resident_fallbacks == 1u)
        fprintf(stderr,
                "gx_vulkan: synchronized software fallback for unsupported "
                "triangle state (program=%u pf=%u texgens=%u)\n",
                job->fused_program, job->pixel_format, job->num_texgens);
    return 0;
}

static int decode_copy(const u32* bp, GxVkCopyPush* p) {
    memset(p, 0, sizeof *p);
    u32 copy = bp[0x52];
    p->left = bp[0x49] & 0x3ffu;
    p->top = (bp[0x49] >> 10) & 0x3ffu;
    p->src_width = (bp[0x4a] & 0x3ffu) + 1u;
    u32 src_height = ((bp[0x4a] >> 10) & 0x3ffu) + 1u;
    p->right = p->left + p->src_width;
    p->bottom = p->top + src_height;
    p->yscale_reg = bp[0x4e];
    p->scale_invert = (copy >> 10) & 1u;
    p->clamp_top = copy & 1u;
    p->clamp_bottom = (copy >> 1) & 1u;
    p->pixel_format = bp[0x43] & 7u;
    p->copy_enable = (copy >> 14) & 1u;
    u32 flow = bp[0x53], fhigh = bp[0x54];
    p->w0 = flow & 0x3fu;
    p->w1 = (flow >> 6) & 0x3fu;
    p->w2 = (flow >> 12) & 0x3fu;
    p->w3 = (flow >> 18) & 0x3fu;
    p->w4 = fhigh & 0x3fu;
    p->w5 = (fhigh >> 6) & 0x3fu;
    p->w6 = (fhigh >> 12) & 0x3fu;
    p->output_words_per_row = (p->src_width + 1u) / 2u;

    float yscale = p->scale_invert ?
        (p->yscale_reg ? 256.0f / (float)p->yscale_reg : 1.0f) :
        (float)p->yscale_reg / 256.0f;
    p->dst_height = (u32)((float)src_height * yscale);
    if (!p->copy_enable) {
        s_vk.xfb_present = 0;
        return 1;
    }
    if (!p->src_width || !src_height || !p->dst_height ||
        p->src_width > EFB_WIDTH || p->dst_height > EFB_HEIGHT ||
        p->pixel_format > 3u) {
        fprintf(stderr,
                "gx_vulkan: XFB shadow configuration out of exact scope "
                "(src=%ux%u dst_h=%u pf=%u)\n",
                p->src_width, src_height, p->dst_height, p->pixel_format);
        return 0;
    }
    s_vk.xfb_present = 1;
    s_vk.xfb_address = (bp[0x4b] << 5) & 0x1fffffffu;
    s_vk.xfb_stride = bp[0x4d] << 5;
    s_vk.xfb_width = p->src_width;
    s_vk.xfb_height = p->dst_height;
    s_vk.xfb_output_words_per_row = p->output_words_per_row;
    return 1;
}

/* ============================================================================
 * EFB->texture resident copy (RG8/RGBA8, half_scale, 3-tap vertical filter).
 *
 * Reachable ONLY for the two exact, individually bit-verified copy words
 * gx_vulkan_resident_efb_copy() gates on before ever calling into this code:
 *   0x01023B -> target=7  -> fmt=11 (RG8),   half_scale=1, clamp_top/bottom=1
 *   0x010263 -> target=12 -> fmt=6  (RGBA8), half_scale=1, clamp_top/bottom=1
 * Every other EFB->texture copy word keeps the synchronized software
 * fallback below, unchanged (see gx_vulkan_resident_efb_copy).
 *
 * pixel_format (bp[0x43]&7, is_rgba6/is_depth below) is separate runtime BP
 * state, NOT part of the copy word, so it is decoded fresh every call and
 * fully supported (RGB8_Z24/RGB565_Z16 direct, RGBA6_Z24, and Z24 depth-as-
 * color) -- exactly gx_raster.c's efb_copy_texture_sample/write_block, only
 * restricted to the two fmt values above. Any other pixel_format (4..7,
 * never legitimately reached -- see GetPixelColor's own TRAPF) is rejected
 * here so it falls into the loud software fallback instead of silently
 * mishandling an unverified state on the GPU path.
 * ========================================================================= */

typedef struct {
    GxVkTexCopyPush push;
    u32 dest_addr, dest_stride, tiles_x, tiles_y, block_bytes, row_bytes;
    u32 pixel_format;
} EfbTexCopyDecoded;

static int decode_efb_tex_copy(const u32* bp, u32 copy_word, u32 ram_size,
                               EfbTexCopyDecoded* d) {
    memset(d, 0, sizeof *d);
    u32 left = bp[0x49] & 0x3ffu;
    u32 top = (bp[0x49] >> 10) & 0x3ffu;
    u32 src_w = (bp[0x4a] & 0x3ffu) + 1u;
    u32 src_h = ((bp[0x4a] >> 10) & 0x3ffu) + 1u;
    u32 right = left + src_w, bottom = top + src_h;
    u32 clamp_top = copy_word & 1u;
    u32 clamp_bottom = (copy_word >> 1) & 1u;
    u32 half_scale = (copy_word >> 9) & 1u;
    u32 intensity = (copy_word >> 15) & 1u;
    u32 target = (copy_word >> 3) & 0xFu;
    u32 fmt = target / 2u + (target & 1u) * 8u;
    u32 out_w = half_scale ? (src_w + 1u) >> 1 : src_w;
    u32 out_h = half_scale ? (src_h + 1u) >> 1 : src_h;
    u32 pixel_format = bp[0x43] & 7u;
    u32 flow = bp[0x53], fhigh = bp[0x54];
    u32 w0 = flow & 0x3fu, w1 = (flow >> 6) & 0x3fu;
    u32 w2 = (flow >> 12) & 0x3fu, w3 = (flow >> 18) & 0x3fu;
    u32 w4 = fhigh & 0x3fu, w5 = (fhigh >> 6) & 0x3fu, w6 = (fhigh >> 12) & 0x3fu;

    /* Exactly the two bit-verified configurations. The copy_word ==
     * 0x01023B/0x010263 gate at the call site already forces fmt to 11 or 6
     * and half_scale/clamp_top/clamp_bottom to 1 and intensity to 0 -- these
     * checks are a defensive second source of truth, never loosened
     * independently of that gate. */
    if ((fmt != 11u && fmt != 6u) || !half_scale || !clamp_top ||
        !clamp_bottom || intensity || pixel_format > 3u ||
        out_w == 0u || out_h == 0u || out_w > EFB_WIDTH || out_h > EFB_HEIGHT)
        return 0;

    u32 block_bytes = fmt == 6u ? 64u : 32u;
    u32 tiles_x = (out_w + 3u) / 4u;
    u32 tiles_y = (out_h + 3u) / 4u;
    u32 row_bytes = tiles_x * block_bytes;
    if ((u64)row_bytes * tiles_y > EFB_TEX_SHADOW_SLOT_BYTES)
        return 0; /* would overrun the shadow arena -- never observed, but
                   * never silently truncate a copy either. */
    u32 dest_addr_raw = bp[0x4b] << 5;
    u32 dest_stride = bp[0x4d] << 5;
    u32 phys = dest_addr_raw & 0x1FFFFFFFu;
    u64 last = (u64)phys + (tiles_y ? (u64)(tiles_y - 1u) * dest_stride : 0u) +
               (u64)row_bytes;
    if (!dest_stride || last > (u64)ram_size)
        return 0;

    d->push.left = left; d->push.top = top;
    d->push.right = right; d->push.bottom = bottom;
    d->push.out_w = out_w; d->push.out_h = out_h;
    d->push.tiles_x = tiles_x; d->push.tiles_y = tiles_y;
    d->push.clamp_top = clamp_top; d->push.clamp_bottom = clamp_bottom;
    d->push.half_scale = half_scale;
    d->push.is_depth = (pixel_format == 3u);
    d->push.is_rgba6 = (pixel_format == 1u);
    d->push.fmt = fmt;
    d->push.reserved0 = 0;
    d->push.block_bytes = block_bytes;
    d->push.w0 = w0; d->push.w1 = w1; d->push.w2 = w2; d->push.w3 = w3;
    d->push.w4 = w4; d->push.w5 = w5; d->push.w6 = w6;
    d->push.output_word_base = 0; /* filled in by the caller, per ring slot */
    d->dest_addr = phys;
    d->dest_stride = dest_stride;
    d->tiles_x = tiles_x;
    d->tiles_y = tiles_y;
    d->block_bytes = block_bytes;
    d->row_bytes = row_bytes;
    d->pixel_format = pixel_format;
    return 1;
}

static void record_efb_tex_copy(const GxVkTexCopyPush* push) {
    vkCmdBindPipeline(s_vk.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                      s_vk.efb_tex_copy_pipeline);
    vkCmdBindDescriptorSets(s_vk.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            s_vk.efb_tex_copy_pipeline_layout, 0, 1,
                            &s_vk.descriptor_set, 0, NULL);
    vkCmdPushConstants(s_vk.command_buffer, s_vk.efb_tex_copy_pipeline_layout,
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof *push, push);
    vkCmdDispatch(s_vk.command_buffer, push->tiles_x, push->tiles_y, 1);
}

/* ---- GCN_GX_EFB_COPY_VERIFY=1 CPU reference (mirrors GCN_GX_CFG_CACHE_VERIFY
 * in gx_raster.c: default off, own getenv-cached knob, abort loudly on any
 * mismatch). Operates on a freshly-downloaded EFB color/depth snapshot (NOT
 * gx_raster.c's own s_efb_color/s_efb_depth, which go stale for any pixel a
 * resident triangle drew without a corun/fallback sync) so the reference is
 * checked against exactly what the GPU shader read. Transcribed bit-for-bit
 * from gx_raster.c's efb_copy_texture_sample/efb_copy_texture_write_block,
 * restricted to fmt 11 (RG8) and fmt 6 (RGBA8). ---- */

typedef struct { u8 r, g, b, a; } RefTexel;

static int ref_clampi(int value, int low, int high) {
    return value < low ? low : (value > high ? high : value);
}

static void ref_extract(const u32* color, const u32* depth, int x, int y,
                        u32 pixel_format, u8* r, u8* g, u8* b, u8* a) {
    u32 off = (u32)y * EFB_WIDTH + (u32)x;
    if (pixel_format == 3u) { /* PF_Z24: depth substitutes for color */
        u32 z = depth[off] & 0xFFFFFFu;
        *r = (u8)(z >> 16); *g = (u8)(z >> 8); *b = (u8)z; *a = 0xFFu;
        return;
    }
    u32 raw = color[off];
    if (pixel_format == 1u) { /* PF_RGBA6_Z24 */
        *a = (u8)(((raw & 0x3Fu) << 2) | ((raw & 0x3Fu) >> 4));
        *b = (u8)((((raw >> 6) & 0x3Fu) << 2) | (((raw >> 6) & 0x3Fu) >> 4));
        *g = (u8)((((raw >> 12) & 0x3Fu) << 2) | (((raw >> 12) & 0x3Fu) >> 4));
        *r = (u8)((((raw >> 18) & 0x3Fu) << 2) | (((raw >> 18) & 0x3Fu) >> 4));
    } else { /* PF_RGB8_Z24 / PF_RGB565_Z16 direct */
        *r = (u8)(raw >> 16); *g = (u8)(raw >> 8); *b = (u8)raw; *a = 0xFFu;
    }
}

static RefTexel ref_sample(const u32* color, const u32* depth, int x, int y,
                           int left, int top, int bottom, int clamp_top,
                           int clamp_bottom, int half_scale, u32 pixel_format,
                           int wab, int wcde, int wfg) {
    if (half_scale) {
        x = left + (x - left) * 2;
        y = top + (y - top) * 2;
    }
    x = ref_clampi(x, 0, (int)EFB_WIDTH - 1);
    y = ref_clampi(y, 0, (int)EFB_HEIGHT - 1);
    int yprev = y - 1, ynext = y + 1;
    if (clamp_top && yprev < top) yprev = top;
    if (clamp_bottom && ynext >= bottom) ynext = bottom - 1;
    yprev = ref_clampi(yprev, 0, (int)EFB_HEIGHT - 1);
    ynext = ref_clampi(ynext, 0, (int)EFB_HEIGHT - 1);

    u8 rr[3], gg[3], bb[3], aa[3];
    int ys[3] = { yprev, y, ynext };
    for (int i = 0; i < 3; i++)
        ref_extract(color, depth, x, ys[i], pixel_format,
                   &rr[i], &gg[i], &bb[i], &aa[i]);
    RefTexel out;
    out.r = (u8)ref_clampi(((int)rr[0] * wab + (int)rr[1] * wcde + (int)rr[2] * wfg) >> 6, 0, 255);
    out.g = (u8)ref_clampi(((int)gg[0] * wab + (int)gg[1] * wcde + (int)gg[2] * wfg) >> 6, 0, 255);
    out.b = (u8)ref_clampi(((int)bb[0] * wab + (int)bb[1] * wcde + (int)bb[2] * wfg) >> 6, 0, 255);
    out.a = aa[1];
    return out;
}

static void ref_write_block(u8* dst, u32 fmt, int bx, int by, int out_w,
                            int out_h, int left, int top, int bottom,
                            int clamp_top, int clamp_bottom, int half_scale,
                            u32 pixel_format, int wab, int wcde, int wfg,
                            const u32* color, const u32* depth) {
    RefTexel px[16];
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            int ox = bx * 4 + x, oy = by * 4 + y;
            int sx = left + (ox < out_w ? ox : out_w - 1);
            int sy = top + (oy < out_h ? oy : out_h - 1);
            px[y * 4 + x] = ref_sample(color, depth, sx, sy, left, top, bottom,
                                       clamp_top, clamp_bottom, half_scale,
                                       pixel_format, wab, wcde, wfg);
        }
    }
    if (fmt == 11u) { /* RG8 */
        for (int i = 0; i < 16; i++) { dst[i * 2] = px[i].g; dst[i * 2 + 1] = px[i].r; }
    } else { /* RGBA8: AR plane, then GB plane */
        for (int i = 0; i < 16; i++) {
            dst[i * 2] = px[i].a; dst[i * 2 + 1] = px[i].r;
            dst[32 + i * 2] = px[i].g; dst[33 + i * 2] = px[i].b;
        }
    }
}

static int resident_efb_copy_verify_one(const EfbTexCopyDecoded* d, u8* ram) {
    if (!invalidate_readback())
        return 0;
    const u32* color = (const u32*)(s_vk.readback_map + READBACK_COLOR_OFFSET);
    const u32* depth = (const u32*)(s_vk.readback_map + READBACK_DEPTH_OFFSET);
    static u8 scratch[EFB_TEX_SHADOW_SLOT_BYTES];
    u64 total = (u64)d->tiles_y * d->row_bytes;
    if (total > sizeof scratch)
        return 0;
    int wab = (int)(d->push.w0 + d->push.w1);
    int wcde = (int)(d->push.w2 + d->push.w3 + d->push.w4);
    int wfg = (int)(d->push.w5 + d->push.w6);
    for (u32 by = 0; by < d->tiles_y; ++by)
        for (u32 bx = 0; bx < d->tiles_x; ++bx)
            ref_write_block(scratch + (u64)by * d->row_bytes + (u64)bx * d->block_bytes,
                            d->push.fmt, (int)bx, (int)by,
                            (int)d->push.out_w, (int)d->push.out_h,
                            (int)d->push.left, (int)d->push.top, (int)d->push.bottom,
                            (int)d->push.clamp_top, (int)d->push.clamp_bottom,
                            (int)d->push.half_scale, d->pixel_format,
                            wab, wcde, wfg, color, depth);
    for (u32 row = 0; row < d->tiles_y; ++row) {
        const u8* got = ram + d->dest_addr + (u64)row * d->dest_stride;
        const u8* want = scratch + (u64)row * d->row_bytes;
        if (memcmp(got, want, d->row_bytes) != 0) {
            u32 first = 0;
            for (; first < d->row_bytes; ++first)
                if (got[first] != want[first]) break;
            fprintf(stderr,
                    "gx_vulkan: EFB->TEXTURE COPY VERIFY MISMATCH row=%u "
                    "byte=%u got=%02X want=%02X dest=%08X fmt=%u %ux%u "
                    "(compare #%llu)\n",
                    row, first, got[first], want[first], d->dest_addr,
                    d->push.fmt, d->push.out_w, d->push.out_h,
                    (unsigned long long)s_vk.efb_copy_verify_compares);
            fflush(stderr);
            abort();
        }
    }
    s_vk.efb_copy_verify_compares++;
    return 1;
}

/* Records the color/depth EFB image download into the readback buffer's
 * color/depth region (same offsets resident_sync_to_software uses), so
 * resident_efb_copy_verify_one() can read back exactly what this copy's
 * shader invocation saw. Only recorded under GCN_GX_EFB_COPY_VERIFY=1 --
 * correctness instrumentation, not part of the default resident path. */
static void record_efb_verify_download(void) {
    VkImageMemoryBarrier download[2] = {0};
    VkBufferImageCopy copies[2] = {0};
    for (u32 i = 0; i < 2; ++i) {
        download[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        download[i].srcAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                    VK_ACCESS_SHADER_WRITE_BIT;
        download[i].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        download[i].oldLayout = download[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        download[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        download[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        download[i].image = s_vk.image[i];
        download[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        download[i].subresourceRange.levelCount = 1;
        download[i].subresourceRange.layerCount = 1;
        copies[i].bufferOffset = i ? READBACK_DEPTH_OFFSET : READBACK_COLOR_OFFSET;
        copies[i].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copies[i].imageSubresource.layerCount = 1;
        copies[i].imageExtent = (VkExtent3D){EFB_WIDTH, EFB_HEIGHT, 1};
    }
    vkCmdPipelineBarrier(s_vk.command_buffer,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 0, NULL, 2, download);
    for (u32 i = 0; i < 2; ++i)
        vkCmdCopyImageToBuffer(s_vk.command_buffer, s_vk.image[i],
                               VK_IMAGE_LAYOUT_GENERAL, s_vk.readback,
                               1, &copies[i]);
    VkBufferMemoryBarrier to_host = {0};
    to_host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    to_host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_host.buffer = s_vk.readback;
    to_host.offset = READBACK_COLOR_OFFSET;
    to_host.size = 2u * EFB_PLANE_BYTES;
    vkCmdPipelineBarrier(s_vk.command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0,
                         0, NULL, 1, &to_host, 0, NULL);
}

/* 1=handled on GPU (materialized, and verified if GCN_GX_EFB_COPY_VERIFY=1),
 * 0=this exact state was rejected by decode_efb_tex_copy (should not happen
 * given the copy_word gate at the call site, but never silently continue a
 * stale co-run/resident state -- fall into the ordinary synchronized
 * fallback), -1=fatal. */
static int gx_vulkan_resident_efb_tex_copy(const u32* bp, u32 copy_word,
                                           u8* ram, u32 ram_size) {
    EfbTexCopyDecoded d;
    if (!decode_efb_tex_copy(bp, copy_word, ram_size, &d))
        return 0;

    if (s_vk.resident_pending_tex_count == EFB_TEX_RING_SIZE &&
        !resident_submit_batch())
        return -1;
    if (!resident_begin_commands())
        return -1;
    if (!resident_emit_draw_batch())
        return -1;

    u32 slot = s_vk.resident_pending_tex_count;
    VkDeviceSize slot_offset = (VkDeviceSize)slot * EFB_TEX_SHADOW_SLOT_BYTES;
    d.push.output_word_base = (u32)(slot_offset / sizeof(u32));

    record_efb_tex_copy(&d.push);

    VkDeviceSize copy_bytes = (VkDeviceSize)d.tiles_y * d.row_bytes;
    VkBufferMemoryBarrier tex_to_transfer = {0};
    tex_to_transfer.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    tex_to_transfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    tex_to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    tex_to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    tex_to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    tex_to_transfer.buffer = s_vk.staging;
    tex_to_transfer.offset = EFB_TEX_SHADOW_OFFSET + slot_offset;
    tex_to_transfer.size = copy_bytes;
    vkCmdPipelineBarrier(s_vk.command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 1, &tex_to_transfer, 0, NULL);
    VkBufferCopy tex_copy = {
        EFB_TEX_SHADOW_OFFSET + slot_offset,
        READBACK_EFB_TEX_OFFSET + slot_offset, copy_bytes
    };
    vkCmdCopyBuffer(s_vk.command_buffer, s_vk.staging, s_vk.readback,
                    1, &tex_copy);
    VkBufferMemoryBarrier tex_to_host = tex_to_transfer;
    tex_to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    tex_to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    tex_to_host.buffer = s_vk.readback;
    tex_to_host.offset = READBACK_EFB_TEX_OFFSET + slot_offset;
    vkCmdPipelineBarrier(s_vk.command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0,
                         0, NULL, 1, &tex_to_host, 0, NULL);

    if (s_vk.efb_copy_verify)
        record_efb_verify_download();

    GxVkPendingTexCopy* pending = &s_vk.resident_pending_tex[slot];
    pending->ram = ram; pending->ram_size = ram_size;
    pending->dest_addr = d.dest_addr; pending->dest_stride = d.dest_stride;
    pending->tiles_x = d.tiles_x; pending->tiles_y = d.tiles_y;
    pending->block_bytes = d.block_bytes; pending->row_bytes = d.row_bytes;
    pending->slot_offset = slot_offset;
    s_vk.resident_pending_tex_count++;

    if ((s_vk.gpu_stats || s_vk.efb_copy_verify) && !resident_submit_batch())
        return -1;

    if (s_vk.efb_copy_verify && !resident_efb_copy_verify_one(&d, ram))
        return -1;

    return 1;
}

static void record_xfb_copy(const GxVkCopyPush* push) {
    vkCmdBindPipeline(s_vk.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                      s_vk.copy_pipeline);
    vkCmdBindDescriptorSets(s_vk.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            s_vk.copy_pipeline_layout, 0, 1,
                            &s_vk.descriptor_set, 0, NULL);
    vkCmdPushConstants(s_vk.command_buffer, s_vk.copy_pipeline_layout,
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof *push, push);
    vkCmdDispatch(s_vk.command_buffer,
                  (push->output_words_per_row + 15u) / 16u,
                  (push->dst_height + 15u) / 16u, 1);
}

int gx_vulkan_resident_efb_copy(const u32* bp, u8* ram, u32 ram_size) {
    u64 copy_t0 = __rdtsc();
    if (!s_vk.resident_mode || !bp || !ram)
        return -1;
    /* Co-run: byte-compare the GPU planes against the software reference at
     * every EFB copy, then hand the copy itself to the software path (return
     * 0 WITHOUT counting a fallback).  Software performs the authoritative
     * XFB encode and the copy-time clear on its own planes, keeping the two
     * sides aligned across copies — the GPU-side copy+clear would clear only
     * the GPU planes, and since the software reference never sees BP-copy
     * clears in resident mode, the first validated corun run flagged exactly
     * that phantom divergence at every sync.  The GPU XFB encode/materialize
     * machinery is deliberately not exercised under corun; the bounded
     * golden sw-vs-vk runs cover it. */
    if (s_corun == 1) {
        if (s_vk.resident_efb_valid) {
            if (!resident_sync_to_software())
                return -1;
            s_vk.resident_efb_valid = 0;
        }
        return 0;
    }
    u32 copy_word = bp[0x52];
    /* Two exact, individually bit-verified EFB->texture copy configurations
     * (RG8 and RGBA8, both half_scale + 3-tap vertical filter -- see
     * gx_efb_tex_copy.comp / decode_efb_tex_copy). Every other copy_word,
     * including every other EFB->texture variant, falls through unchanged to
     * the ordinary synchronized-fallback check below. */
    if (((copy_word >> 7) & 3u) == 0u &&
        (copy_word == 0x01023Bu || copy_word == 0x010263u)) {
        int tex_result = gx_vulkan_resident_efb_tex_copy(bp, copy_word, ram,
                                                         ram_size);
        if (tex_result != 0)
            return tex_result; /* 1 = handled, -1 = fatal */
        /* tex_result==0: decode_efb_tex_copy rejected the exact state it was
         * given (should not happen given the copy_word gate above, but never
         * silently fall through to a stale co-run/resident state) -- drop
         * into the ordinary synchronized fallback below. */
    }
    GxVkCopyPush copy_push;
    if (((copy_word >> 7) & 3u) != 0u ||
        ((copy_word >> 14) & 1u) == 0u ||
        !decode_copy(bp, &copy_push) || !copy_push.copy_enable) {
        if (!resident_sync_to_software())
            return -1;
        s_vk.resident_efb_valid = 0;
        s_vk.resident_fallbacks++;
        fprintf(stderr,
                "gx_vulkan: synchronized software fallback for unsupported "
                "EFB copy state (copy=%06X)\n", copy_word);
        return 0;
    }
    if (s_vk.resident_pending_count == XFB_RING_SIZE &&
        !resident_submit_batch())
        return -1;
    if (!resident_begin_commands())
        return -1;
    if (!resident_emit_draw_batch())
        return -1;

    u32 pending_index = s_vk.resident_pending_count;
    VkDeviceSize slot_offset = (VkDeviceSize)pending_index * XFB_SHADOW_BYTES;
    copy_push.output_word_base = (u32)(slot_offset / sizeof(u32));
    u32 gpu_stride = copy_push.output_words_per_row * sizeof(u32);
    VkDeviceSize xfb_bytes = (VkDeviceSize)gpu_stride * copy_push.dst_height;
    u64 row_bytes = (u64)s_vk.xfb_width * 2u;
    u64 last = (u64)s_vk.xfb_address +
               (u64)(s_vk.xfb_height - 1u) * s_vk.xfb_stride + row_bytes;
    if (!s_vk.xfb_stride || last > ram_size || !xfb_bytes ||
        xfb_bytes > XFB_SHADOW_BYTES)
        return -1;

    u32 xfb_query = gpu_time_begin(GPU_TIME_XFB);
    record_xfb_copy(&copy_push);
    gpu_time_end(xfb_query);

    VkImageMemoryBarrier copy_to_clear[2] = {0};
    for (u32 i = 0; i < 2; ++i) {
        copy_to_clear[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        copy_to_clear[i].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        copy_to_clear[i].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        copy_to_clear[i].oldLayout = copy_to_clear[i].newLayout =
            VK_IMAGE_LAYOUT_GENERAL;
        copy_to_clear[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        copy_to_clear[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        copy_to_clear[i].image = s_vk.image[i];
        copy_to_clear[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy_to_clear[i].subresourceRange.levelCount = 1;
        copy_to_clear[i].subresourceRange.layerCount = 1;
    }
    vkCmdPipelineBarrier(s_vk.command_buffer,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                         0, NULL, 0, NULL, 2, copy_to_clear);

    GxVkClearPush clear_push = decode_clear(bp);
    u32 clear_query = gpu_time_begin(GPU_TIME_CLEAR);
    vkCmdBindPipeline(s_vk.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                      s_vk.clear_pipeline);
    vkCmdBindDescriptorSets(s_vk.command_buffer,
                            VK_PIPELINE_BIND_POINT_COMPUTE,
                            s_vk.clear_pipeline_layout, 0, 1,
                            &s_vk.descriptor_set, 0, NULL);
    vkCmdPushConstants(s_vk.command_buffer, s_vk.clear_pipeline_layout,
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof clear_push,
                       &clear_push);
    if (clear_push.clear_enable)
        vkCmdDispatch(s_vk.command_buffer,
                      (clear_push.right - clear_push.left + 16u) / 16u,
                      (clear_push.bottom - clear_push.top + 16u) / 16u, 1);
    gpu_time_end(clear_query);

    /* The resident path may keep recording after this EFB copy until the
     * frame fence.  Make the clear visible to those following draw dispatches
     * inside the same command buffer.  The old submit-after-every-copy path
     * got this dependency from resident_begin_commands() in the next command
     * buffer; frame batching needs it here explicitly. */
    if (clear_push.clear_enable) {
        VkImageMemoryBarrier clear_to_draw[2] = {0};
        for (u32 i = 0; i < 2u; ++i) {
            clear_to_draw[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            clear_to_draw[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            clear_to_draw[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                             VK_ACCESS_SHADER_WRITE_BIT;
            clear_to_draw[i].oldLayout = clear_to_draw[i].newLayout =
                VK_IMAGE_LAYOUT_GENERAL;
            clear_to_draw[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            clear_to_draw[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            clear_to_draw[i].image = s_vk.image[i];
            clear_to_draw[i].subresourceRange.aspectMask =
                VK_IMAGE_ASPECT_COLOR_BIT;
            clear_to_draw[i].subresourceRange.levelCount = 1;
            clear_to_draw[i].subresourceRange.layerCount = 1;
        }
        vkCmdPipelineBarrier(s_vk.command_buffer,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                             0, NULL, 0, NULL, 2, clear_to_draw);
    }

    VkBufferMemoryBarrier xfb_to_transfer = {0};
    xfb_to_transfer.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    xfb_to_transfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    xfb_to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    xfb_to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    xfb_to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    xfb_to_transfer.buffer = s_vk.staging;
    xfb_to_transfer.offset = XFB_SHADOW_OFFSET + slot_offset;
    xfb_to_transfer.size = xfb_bytes;
    vkCmdPipelineBarrier(s_vk.command_buffer,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 1, &xfb_to_transfer, 0, NULL);
    VkBufferCopy xfb_copy = {
        XFB_SHADOW_OFFSET + slot_offset,
        READBACK_XFB_OFFSET + slot_offset, xfb_bytes
    };
    vkCmdCopyBuffer(s_vk.command_buffer, s_vk.staging, s_vk.readback,
                    1, &xfb_copy);
    VkBufferMemoryBarrier xfb_to_host = xfb_to_transfer;
    xfb_to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    xfb_to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    xfb_to_host.buffer = s_vk.readback;
    xfb_to_host.offset = READBACK_XFB_OFFSET + slot_offset;
    vkCmdPipelineBarrier(s_vk.command_buffer,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0,
                         0, NULL, 1, &xfb_to_host, 0, NULL);
    GxVkPendingXfb* pending = &s_vk.resident_pending[pending_index];
    pending->ram = ram;
    pending->ram_size = ram_size;
    pending->address = s_vk.xfb_address;
    pending->stride = s_vk.xfb_stride;
    pending->width = s_vk.xfb_width;
    pending->height = s_vk.xfb_height;
    pending->gpu_stride = gpu_stride;
    pending->slot_offset = slot_offset;
    s_vk.resident_pending_count++;
    if (s_vk.gpu_stats) {
        if (!resident_submit_batch())
            return -1;
    }
    /* Keep the copy and all following draws in this command buffer until the
     * frame fence (GXSetDrawDone -> gx_vulkan_resident_flush).  The resident
     * path has one host-mapped staging arena: submitting here and immediately
     * recording the next frame let the CPU overwrite draw packets while the
     * GPU was still reading them, producing intermittent stretched geometry
     * and whole-object flicker.  A full XFB ring still forces an earlier
     * submit above, and texture updates explicitly submit before reusing
     * their arena bytes. */
    s_vk.resident_copy_total_tsc += __rdtsc() - copy_t0;
    return 1;
}

int gx_vulkan_resident_flush(void) {
    if (!s_vk.resident_mode)
        return 1;
    return resident_submit_batch();
}

int gx_vulkan_shadow_prepare_efb(const u32* bp, const u32* color,
                                 const u32* depth) {
    if (!s_vk.device || !bp || !color || !depth || !s_vk.staging_map) {
        fprintf(stderr, "gx_vulkan: prepare missing initialized state/input\n");
        return 0;
    }
    GxVkCopyPush copy_push;
    if (!decode_copy(bp, &copy_push))
        return 0;
    memcpy(s_vk.staging_map, color, (size_t)EFB_PLANE_BYTES);
    memcpy(s_vk.staging_map + EFB_PLANE_BYTES, depth, (size_t)EFB_PLANE_BYTES);
    if (!s_vk.staging_coherent) {
        VkMappedMemoryRange range = {0};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = s_vk.staging_memory;
        range.size = VK_WHOLE_SIZE;
        VkResult result = vkFlushMappedMemoryRanges(s_vk.device, 1, &range);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "gx_vulkan: prepare flush failed (%d)\n", result);
            return 0;
        }
    }

    VkResult result = vkResetCommandBuffer(s_vk.command_buffer, 0);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "gx_vulkan: command reset failed (%d)\n", result);
        return 0;
    }
    VkCommandBufferBeginInfo begin = {0};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(s_vk.command_buffer, &begin);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "gx_vulkan: command begin failed (%d)\n", result);
        return 0;
    }

    VkImageMemoryBarrier upload[2] = {0};
    for (u32 i = 0; i < 2; ++i) {
        upload[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        upload[i].srcAccessMask = s_vk.images_general ?
            (VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT) : 0u;
        upload[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        upload[i].oldLayout = s_vk.images_general ? VK_IMAGE_LAYOUT_GENERAL :
                                                   VK_IMAGE_LAYOUT_UNDEFINED;
        upload[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        upload[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        upload[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        upload[i].image = s_vk.image[i];
        upload[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        upload[i].subresourceRange.levelCount = 1;
        upload[i].subresourceRange.layerCount = 1;
    }
    vkCmdPipelineBarrier(s_vk.command_buffer,
        s_vk.images_general ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT :
                              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, upload);
    VkBufferImageCopy copies[2] = {0};
    for (u32 i = 0; i < 2; ++i) {
        copies[i].bufferOffset = EFB_PLANE_BYTES * i;
        copies[i].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copies[i].imageSubresource.layerCount = 1;
        copies[i].imageExtent = (VkExtent3D){EFB_WIDTH, EFB_HEIGHT, 1};
        vkCmdCopyBufferToImage(s_vk.command_buffer, s_vk.staging, s_vk.image[i],
                               VK_IMAGE_LAYOUT_GENERAL, 1, &copies[i]);
    }
    VkImageMemoryBarrier compute[2] = {upload[0], upload[1]};
    for (u32 i = 0; i < 2; ++i) {
        compute[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        compute[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                   VK_ACCESS_SHADER_WRITE_BIT;
        compute[i].oldLayout = compute[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    vkCmdPipelineBarrier(s_vk.command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                         0, NULL, 0, NULL, 2, compute);

    /* XFB encode observes the pre-clear EFB, exactly matching BP copy order. */
    if (copy_push.copy_enable) {
        record_xfb_copy(&copy_push);

        VkImageMemoryBarrier copy_to_clear[2] = {compute[0], compute[1]};
        for (u32 i = 0; i < 2; ++i) {
            copy_to_clear[i].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            copy_to_clear[i].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        }
        vkCmdPipelineBarrier(s_vk.command_buffer,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                             0, NULL, 0, NULL, 2, copy_to_clear);
    }

    GxVkClearPush push = decode_clear(bp);
    vkCmdBindPipeline(s_vk.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                      s_vk.clear_pipeline);
    vkCmdBindDescriptorSets(s_vk.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            s_vk.clear_pipeline_layout, 0, 1,
                            &s_vk.descriptor_set, 0, NULL);
    vkCmdPushConstants(s_vk.command_buffer, s_vk.clear_pipeline_layout,
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
    if (push.clear_enable)
        vkCmdDispatch(s_vk.command_buffer,
                      (push.right - push.left + 16u) / 16u,
                      (push.bottom - push.top + 16u) / 16u, 1);
    VkImageMemoryBarrier download[2] = {compute[0], compute[1]};
    for (u32 i = 0; i < 2; ++i) {
        download[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        download[i].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    }
    VkBufferMemoryBarrier xfb_to_transfer = {0};
    xfb_to_transfer.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    xfb_to_transfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    xfb_to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    xfb_to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    xfb_to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    xfb_to_transfer.buffer = s_vk.staging;
    xfb_to_transfer.offset = XFB_SHADOW_OFFSET;
    xfb_to_transfer.size = XFB_SHADOW_BYTES;
    vkCmdPipelineBarrier(s_vk.command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 1, &xfb_to_transfer, 2, download);
    VkBufferCopy xfb_copy = {
        XFB_SHADOW_OFFSET, READBACK_XFB_OFFSET, XFB_SHADOW_BYTES
    };
    vkCmdCopyBuffer(s_vk.command_buffer, s_vk.staging, s_vk.readback,
                    1, &xfb_copy);
    for (u32 i = 0; i < 2; ++i) {
        copies[i].bufferOffset = i ? READBACK_DEPTH_OFFSET :
                                     READBACK_COLOR_OFFSET;
        vkCmdCopyImageToBuffer(s_vk.command_buffer, s_vk.image[i],
                               VK_IMAGE_LAYOUT_GENERAL, s_vk.readback, 1, &copies[i]);
    }
    VkBufferMemoryBarrier readback_to_host = {0};
    readback_to_host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    readback_to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    readback_to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    readback_to_host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    readback_to_host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    readback_to_host.buffer = s_vk.readback;
    readback_to_host.offset = 0;
    readback_to_host.size = READBACK_BYTES;
    vkCmdPipelineBarrier(s_vk.command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0,
                         0, NULL, 1, &readback_to_host, 0, NULL);
    result = vkEndCommandBuffer(s_vk.command_buffer);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "gx_vulkan: command end failed (%d)\n", result);
        return 0;
    }
    VkSubmitInfo submit = {0};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &s_vk.command_buffer;
    result = vkQueueSubmit(s_vk.queue, 1, &submit, VK_NULL_HANDLE);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "gx_vulkan: queue submit failed (%d)\n", result);
        return 0;
    }
    result = vkQueueWaitIdle(s_vk.queue);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "gx_vulkan: queue wait failed (%d)\n", result);
        return 0;
    }
    if (!invalidate_readback()) {
        fprintf(stderr, "gx_vulkan: prepare readback invalidate failed\n");
        return 0;
    }
    s_vk.images_general = 1;
    s_vk.prepared = 1;
    return 1;
}

static int compare_plane(const char* name, const u32* software,
                         const u32* gpu) {
    for (u32 i = 0; i < EFB_PIXELS; ++i) {
        if (software[i] != gpu[i]) {
            fprintf(stderr,
                "gx_vulkan: %s mismatch at (%u,%u): software=%08X gpu=%08X\n",
                name, i % EFB_WIDTH, i / EFB_WIDTH, software[i], gpu[i]);
            return 0;
        }
    }
    return 1;
}

static int compare_xfb(const u8* ram, u32 ram_size) {
    if (!s_vk.xfb_present)
        return 1;
    u64 row_bytes = (u64)s_vk.xfb_width * 2u;
    u64 last = (u64)s_vk.xfb_address +
               (u64)(s_vk.xfb_height - 1u) * s_vk.xfb_stride + row_bytes;
    if (!ram || !s_vk.xfb_stride || last > ram_size) {
        fprintf(stderr,
                "gx_vulkan: XFB comparison destination out of MEM1 "
                "(addr=%08X stride=%u size=%ux%u ram=%u)\n",
                s_vk.xfb_address, s_vk.xfb_stride, s_vk.xfb_width,
                s_vk.xfb_height, ram_size);
        return 0;
    }
    const u8* gpu = s_vk.readback_map + READBACK_XFB_OFFSET;
    u32 gpu_stride = s_vk.xfb_output_words_per_row * sizeof(u32);
    for (u32 y = 0; y < s_vk.xfb_height; ++y) {
        const u8* software_row = ram + s_vk.xfb_address +
                                 (u64)y * s_vk.xfb_stride;
        const u8* gpu_row = gpu + (u64)y * gpu_stride;
        for (u32 x = 0; x < (u32)row_bytes; ++x) {
            if (software_row[x] != gpu_row[x]) {
                fprintf(stderr,
                        "gx_vulkan: XFB mismatch at byte (%u,%u): "
                        "software=%02X gpu=%02X\n",
                        x, y, software_row[x], gpu_row[x]);
                return 0;
            }
        }
    }
    s_vk.xfb_comparisons++;
    return 1;
}

int gx_vulkan_shadow_compare_efb(const u32* color, const u32* depth,
                                 const u8* ram, u32 ram_size) {
    if (!s_vk.prepared || !color || !depth)
        return 0;
    s_vk.prepared = 0;
    const u32* gpu_color = (const u32*)(s_vk.readback_map +
                                        READBACK_COLOR_OFFSET);
    const u32* gpu_depth = (const u32*)(s_vk.readback_map +
                                        READBACK_DEPTH_OFFSET);
    int match = compare_plane("color", color, gpu_color) &&
                compare_plane("depth", depth, gpu_depth) &&
                compare_xfb(ram, ram_size);
    if (match)
        s_vk.comparisons++;
    return match;
}

#else

int gx_vulkan_shadow_init(void) {
    fprintf(stderr, "gx_vulkan: runtime was built without Vulkan SDK support\n");
    return 0;
}
int gx_vulkan_resident_init(void) { return gx_vulkan_shadow_init(); }
void gx_vulkan_shadow_shutdown(void) {}
int gx_vulkan_shadow_prepare_efb(const u32* bp, const u32* color,
                                 const u32* depth) {
    (void)bp; (void)color; (void)depth;
    return 0;
}
int gx_vulkan_shadow_compare_efb(const u32* color, const u32* depth,
                                 const u8* ram, u32 ram_size) {
    (void)color; (void)depth; (void)ram; (void)ram_size;
    return 0;
}
int gx_vulkan_shadow_begin_draw(const u32* bp, const u8* ram, u32 ram_size) {
    (void)bp; (void)ram; (void)ram_size; return 0;
}
int gx_vulkan_shadow_triangle(const GxRasterTriangleJob* job,
                              int after_software) {
    (void)job; (void)after_software; return 0;
}
int gx_vulkan_shadow_end_draw(void) { return 0; }
int gx_vulkan_resident_triangle(const GxRasterTriangleJob* job,
                                int after_software) {
    (void)job; (void)after_software; return -1;
}
int gx_vulkan_resident_efb_copy(const u32* bp, u8* ram, u32 ram_size) {
    (void)bp; (void)ram; (void)ram_size; return -1;
}
int gx_vulkan_resident_sync_to_software(void) { return 1; }
int gx_vulkan_resident_busy(void) { return 0; }

#endif
