/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Headless Vulkan differential shadow for the packed software EFB. At each
 * ordered EFB-copy boundary it uploads the pre-copy color/depth planes, runs
 * independently decoded integer compute XFB encode and clear passes, reads the
 * results back, and compares them after the authoritative software copy/clear.
 */
#include "gx/gx_vulkan.h"
#include "gx/gx.h"       /* XFB RAM publication guard shared with VI */

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
#define READBACK_BYTES (READBACK_DEPTH_OFFSET + EFB_PLANE_BYTES)
#define DRAW_JOB_OFFSET (XFB_SHADOW_OFFSET + XFB_SHADOW_TOTAL)
#define DRAW_PACKET_BYTES 512u
#define GX_VK_DRAW_PROGRAM_COUNT 19u
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
#define STAGING_BYTES   (TEXTURE_SHADOW_OFFSET + TEXTURE_SHADOW_BYTES)
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

typedef struct {
    u32 word_base;
    u32 batch_mode;
    u32 tile_header_base;
    u32 tile_index_base;
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
    int draw_active;
    u64 draws;
    u64 triangles;
    u64 fused_triangles[GX_VK_DRAW_PROGRAM_COUNT + 1u];
    u32 draw_validate_remaining[GX_VK_DRAW_PROGRAM_COUNT + 1u];
    u32 draw_validate_program; /* 0=all A--S, otherwise exact program id */
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
    u32 texture_arena_used;
    u64 resident_batches;
    u64 resident_triangles;
    u64 resident_fallbacks;
    u64 resident_submit_cpu_tsc;
    u64 resident_wait_tsc;
    u64 resident_copy_total_tsc;
    u64 resident_copy_memcpy_tsc;
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
    VkDescriptorSetLayoutBinding bindings[5] = {0};
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
    VkDescriptorSetLayoutCreateInfo dl = {0};
    dl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dl.bindingCount = 5;
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

    VkDescriptorPoolSize pool_sizes[2] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3}
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
    VkWriteDescriptorSet writes[5] = {0};
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
    vkUpdateDescriptorSets(s_vk.device, 5, writes, 0, NULL);

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
        fprintf(stderr,
                "gx_vulkan: captured %llu draws / %llu post-clip triangles "
                "(A=%llu B=%llu C=%llu D=%llu E=%llu F=%llu G=%llu "
                "H=%llu I=%llu J=%llu K=%llu L=%llu M=%llu N=%llu "
                "O=%llu P=%llu Q=%llu R=%llu S=%llu general=%llu)\n",
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
                (unsigned long long)s_vk.fused_triangles[0]);
    }
    for (u32 program = 1; program <= GX_VK_DRAW_PROGRAM_COUNT; ++program) {
        if (s_vk.draw_validations[program])
            fprintf(stderr,
                    "gx_vulkan: %llu fused-program-%c GPU triangle comparisons passed\n",
                    (unsigned long long)s_vk.draw_validations[program],
                    (int)('A' + program - 1u));
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
    if (s_vk.device && s_vk.draw_f_pipeline)
        vkDestroyPipeline(s_vk.device, s_vk.draw_f_pipeline, NULL);
    if (s_vk.device && s_vk.copy_pipeline)
        vkDestroyPipeline(s_vk.device, s_vk.copy_pipeline, NULL);
    if (s_vk.device && s_vk.clear_pipeline)
        vkDestroyPipeline(s_vk.device, s_vk.clear_pipeline, NULL);
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
    s_vk.subgroup_xfb =
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
      if (e && e[0] && !e[1] && e[0] >= 'A' && e[0] <= 'S')
          s_vk.draw_validate_program = (u32)(e[0] - 'A') + 1u;
      else if (e && strcmp(e, "ALL") == 0)
          s_vk.draw_validate_program = 0u;
      else if (e && *e)
          fprintf(stderr,
                  "gx_vulkan: invalid GCN_GX_VK_DRAW_PROGRAM='%s'; using F\n",
                  e); }
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
            "gx_vulkan: resident exact A--S compute path enabled; unsupported "
            "state synchronizes to software\n");
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

static int resolve_fused_texture(const GxRasterTriangleJob* job,
                                 GxVkTextureEntry** texture_out) {
    *texture_out = NULL;
    if (job->fused_program != 6u && job->fused_program != 10u &&
        job->fused_program != 13u && job->fused_program != 16u) {
        u32 order = s_vk.draw_bp[0x28];
        u32 unit = order & 7u;
        u32 mode0 = s_vk.draw_bp[0x80 + unit];
        GxVkTextureEntry* texture = find_texture_binding(s_vk.draw_bp, unit);
        if (!(order & (1u << 6)) || !texture ||
            (texture->format != 0u && texture->format != 1u &&
             texture->format != 6u) ||
            texture->length > TEXTURE_SHADOW_BYTES ||
            (mode0 & 3u) == 3u || ((mode0 >> 2) & 3u) == 3u ||
            ((mode0 >> 5) & 3u) != 0u) {
            if (!s_vk.texture_reject_logged[job->fused_program]) {
                s_vk.texture_reject_logged[job->fused_program] = 1;
                fprintf(stderr,
                        "gx_vulkan: fused-program-%c exact GPU path supports "
                        "only a resident stage-0 I4/I8/RGBA8 non-mipmapped texture "
                        "with supported wrap (unit=%u fmt=%u size=%ux%u "
                        "mode0=%06X)\n",
                        (int)('A' + job->fused_program - 1u), unit,
                        texture ? texture->format : 0xffffffffu,
                        texture ? texture->width : 0u,
                        texture ? texture->height : 0u, mode0);
            }
            return 0;
        }
        *texture_out = texture;
    }
    return 1;
}

static void snapshot_fused_draw(const GxRasterTriangleJob* job, u32* words,
                                const GxVkTextureEntry* texture) {
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
        u32 order = s_vk.draw_bp[0x28];
        u32 unit = order & 7u;
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
    /* Words 108..127 are alignment-only stride padding.  The shader never
     * indexes them, and resident packets are written directly into their
     * mapped final slot, so touching that padding is pure bandwidth. */
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
    if (!s_vk.resident_pending_count)
        return 1;
    if (!invalidate_readback())
        return 0;
    u64 t0 = __rdtsc();
    gcn_gx_xfb_write_begin();
    for (u32 i = 0; i < s_vk.resident_pending_count; ++i) {
        const GxVkPendingXfb* p = &s_vk.resident_pending[i];
        const u8* gpu = s_vk.readback_map + READBACK_XFB_OFFSET + p->slot_offset;
        for (u32 y = 0; y < p->height; ++y)
            memcpy(p->ram + p->address + (u64)y * p->stride,
                   gpu + (u64)y * p->gpu_stride, (size_t)p->width * 2u);
    }
    gcn_gx_xfb_write_end();
    s_vk.resident_copy_memcpy_tsc += __rdtsc() - t0;
    s_vk.resident_pending_count = 0;
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
        header_offset / sizeof(u32), index_offset / sizeof(u32)
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
    if (!resolve_fused_texture(job, &texture) ||
        !ensure_resident_texture(texture))
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
    snapshot_fused_draw(job, packet, texture);
    if (texture)
        packet[99] = texture->gpu_offset;
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
    if (!resolve_fused_texture(job, &texture))
        return 0;
    snapshot_fused_draw(job, words, texture);
    if (texture) {
        memcpy(s_vk.staging_map + TEXTURE_SHADOW_OFFSET,
               texture->bytes, texture->length);
        words[99] = 0u;
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
        char label[] = "draw-A color";
        u32 program = s_vk.draw_validation_program;
        label[5] = (char)('A' + program - 1u);
        if (!compare_plane(label, color, gpu))
            return 0;
        if (program == 11u || program == 12u) {
            const u32* gpu_depth = (const u32*)(s_vk.readback_map +
                                                READBACK_DEPTH_OFFSET);
            char depth_label[] = "draw-K depth";
            depth_label[5] = (char)('A' + program - 1u);
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
                "gx_vulkan: fused-program-%c validation submit failed\n",
                (int)('A' + job->fused_program - 1u));
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
    memcpy(color, s_vk.readback_map + READBACK_COLOR_OFFSET,
           (size_t)EFB_PLANE_BYTES);
    memcpy(depth, s_vk.readback_map + READBACK_DEPTH_OFFSET,
           (size_t)EFB_PLANE_BYTES);
    return 1;
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
    if (supported)
        return 1;

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
    GxVkCopyPush copy_push;
    u32 copy_word = bp[0x52];
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

#endif
