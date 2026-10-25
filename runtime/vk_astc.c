// ASTC textures on the GPU: the TOTK_ASTC knob, a compute decoder that turns raw blocks into the RGBA8 image the CPU
// decoder (tex_astc.c) would have written, and the native ASTC formats where the device samples them itself.
//
// TOTK_ASTC = auto (default): native when the device reports textureCompressionASTC_LDR and the format's features,
// else gpu. gpu: compute decode into RGBA8 (runtime/shaders/astc_decode.comp). native: like auto, but logs when the
// device cannot. cpu: astc_decode_ldr_mt on the uploading thread, as before. check: gpu, and every upload is also
// decoded on the CPU and compared (waits for the GPU; for testing only).
//
// The decode is recorded into the upload batch (vk_memory.c): the raw blocks go from staging to a device-local scratch
// buffer, the dispatch writes RGBA8 texels behind them, and a buffer-to-image copy puts them into the image. The
// scratch buffer is freed once the batch completes (vk_defer_buffer).
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "astc_decode_spv.h"
#include "cfg.h"
#include "host.h"
#include "tex.h"
#include "vk.h"

// tex_astc.c's unquantization tables, as the shader reads them (Tables in astc_decode.comp): 12 + 16 row offsets in
// bytes, then the bytes. Kept in step with tex_astc.c by the oracle (--probe-astc), which compares every mode.
static const uint8_t wunq_data[] = {
    0,64,
    0,32,64,
    0,21,43,64,
    0,16,32,48,64,
    0,64,12,52,25,39,
    0,9,18,27,37,46,55,64,
    0,64,7,57,14,50,21,43,28,36,
    0,64,17,47,5,59,23,41,11,53,28,36,
    0,4,8,12,17,21,25,29,35,39,43,47,52,56,60,64,
    0,64,16,48,3,61,19,45,6,58,23,41,9,55,26,38,13,51,29,35,
    0,64,8,56,16,48,24,40,2,62,11,53,19,45,27,37,5,59,13,51,22,42,30,34,
    0,2,4,6,8,10,12,14,16,18,20,22,24,26,28,30,34,36,38,40,42,44,46,48,50,52,54,56,58,60,62,64,
};
static const uint16_t wunq_off[12] = {0,2,5,9,14,20,28,38,50,66,86,110};
static const uint8_t cunq_data[] = {
    0,255,51,204,102,153,
    0,36,73,109,146,182,219,255,
    0,255,28,227,56,199,84,171,113,142,
    0,255,69,186,23,232,92,163,46,209,116,139,
    0,17,34,51,68,85,102,119,136,153,170,187,204,221,238,255,
    0,255,67,188,13,242,80,175,27,228,94,161,40,215,107,148,54,201,121,134,
    0,255,33,222,66,189,99,156,11,244,44,211,77,178,110,145,22,233,55,200,88,167,121,134,
    0,8,16,24,33,41,49,57,66,74,82,90,99,107,115,123,132,140,148,156,165,173,181,189,198,206,214,222,
    231,239,247,255,
    0,255,32,223,65,190,97,158,6,249,39,216,71,184,104,151,13,242,45,210,78,177,110,145,19,236,52,203,
    84,171,117,138,26,229,58,197,91,164,123,132,
    0,255,16,239,32,223,48,207,65,190,81,174,97,158,113,142,5,250,21,234,38,217,54,201,70,185,86,169,
    103,152,119,136,11,244,27,228,43,212,59,196,76,179,92,163,108,147,124,131,
    0,4,8,12,16,20,24,28,32,36,40,44,48,52,56,60,65,69,73,77,81,85,89,93,97,101,105,109,113,117,121,125,
    130,134,138,142,146,150,154,158,162,166,170,174,178,182,186,190,195,199,203,207,211,215,219,223,227,
    231,235,239,243,247,251,255,
    0,255,16,239,32,223,48,207,64,191,80,175,96,159,112,143,3,252,19,236,35,220,51,204,67,188,83,172,
    100,155,116,139,6,249,22,233,38,217,54,201,71,184,87,168,103,152,119,136,9,246,25,230,42,213,58,197,
    74,181,90,165,106,149,122,133,13,242,29,226,45,210,61,194,77,178,93,162,109,146,125,130,
    0,255,8,247,16,239,24,231,32,223,40,215,48,207,56,199,64,191,72,183,80,175,88,167,96,159,104,151,
    112,143,120,135,2,253,10,245,18,237,26,229,35,220,43,212,51,204,59,196,67,188,75,180,83,172,91,164,
    99,156,107,148,115,140,123,132,5,250,13,242,21,234,29,226,37,218,45,210,53,202,61,194,70,185,78,177,
    86,169,94,161,102,153,110,145,118,137,126,129,
    0,2,4,6,8,10,12,14,16,18,20,22,24,26,28,30,32,34,36,38,40,42,44,46,48,50,52,54,56,58,60,62,64,66,68,
    70,72,74,76,78,80,82,84,86,88,90,92,94,96,98,100,102,104,106,108,110,112,114,116,118,120,122,124,
    126,129,131,133,135,137,139,141,143,145,147,149,151,153,155,157,159,161,163,165,167,169,171,173,175,
    177,179,181,183,185,187,189,191,193,195,197,199,201,203,205,207,209,211,213,215,217,219,221,223,225,
    227,229,231,233,235,237,239,241,243,245,247,249,251,253,255,
    0,255,8,247,16,239,24,231,32,223,40,215,48,207,56,199,64,191,72,183,80,175,88,167,96,159,104,151,
    112,143,120,135,1,254,9,246,17,238,25,230,33,222,41,214,49,206,57,198,65,190,73,182,81,174,89,166,
    97,158,105,150,113,142,121,134,3,252,11,244,19,236,27,228,35,220,43,212,51,204,59,196,67,188,75,180,
    83,172,91,164,99,156,107,148,115,140,123,132,4,251,12,243,20,235,28,227,36,219,44,211,52,203,60,195,
    68,187,76,179,84,171,92,163,100,155,108,147,116,139,124,131,6,249,14,241,22,233,30,225,38,217,46,
    209,54,201,62,193,70,185,78,177,86,169,94,161,102,153,110,145,118,137,126,129,
    0,255,4,251,8,247,12,243,16,239,20,235,24,231,28,227,32,223,36,219,40,215,44,211,48,207,52,203,56,
    199,60,195,64,191,68,187,72,183,76,179,80,175,84,171,88,167,92,163,96,159,100,155,104,151,108,147,
    112,143,116,139,120,135,124,131,1,254,5,250,9,246,13,242,17,238,21,234,25,230,29,226,33,222,37,218,
    41,214,45,210,49,206,53,202,57,198,61,194,65,190,69,186,73,182,77,178,81,174,85,170,89,166,93,162,
    97,158,101,154,105,150,109,146,113,142,117,138,121,134,125,130,2,253,6,249,10,245,14,241,18,237,22,
    233,26,229,30,225,34,221,38,217,42,213,46,209,50,205,54,201,58,197,62,193,66,189,70,185,74,181,78,
    177,82,173,86,169,90,165,94,161,98,157,102,153,106,149,110,145,114,141,118,137,122,133,126,129,
};
static const uint16_t cunq_off[16] = {0,6,14,24,36,52,72,96,128,168,216,280,360,456,584,744};
_Static_assert(sizeof wunq_data == 142 && sizeof cunq_data == 936, "unquantization table size");

enum { TAB_BYTES = 28 * 4, TAB_WUNQ = TAB_BYTES, TAB_CUNQ = TAB_BYTES + 144, TAB_SIZE = TAB_CUNQ + 936 };

typedef struct {
    uint32_t in_base, out_base, width, height, bw, bh, bx, bslice, srgb;
} Push;

static SRWLOCK g_lock = SRWLOCK_INIT;
static struct {
    VkDevice dev;                 // the device the objects below belong to (probes recreate it)
    int mode;                     // VK_ASTC_*; -1 before the first vk_astc_mode with a device
    int failed;                   // pipeline creation failed on dev: gpu falls back to cpu
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipe;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkBuffer tab;                 // device-local copy of the tables
    VmaAllocation tab_alloc;
    int tab_uploaded;
} G = {.mode = -1};
static uint64_t n_dispatch, n_texels;

static void fill_tables(uint8_t *p) {
    uint32_t off[28];
    for (int i = 0; i < 12; i++) off[i] = TAB_WUNQ + wunq_off[i];
    for (int i = 0; i < 16; i++) off[12 + i] = TAB_CUNQ + cunq_off[i];
    memset(p, 0, TAB_SIZE);
    memcpy(p, off, sizeof off);
    memcpy(p + TAB_WUNQ, wunq_data, sizeof wunq_data);
    memcpy(p + TAB_CUNQ, cunq_data, sizeof cunq_data);
}

// Objects of the current device; under g_lock (exclusive).
static void reset_for(VkCtx *c) {
    if (G.dev == c->device) return;
    memset(&G.dsl, 0, sizeof G - offsetof(__typeof__(G), dsl));   // the old device's objects went with it
    G.dev = c->device;
    G.failed = 0;
    G.mode = -1;
}

static int create_pipeline(VkCtx *c) {
    G.push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(c->device, "vkCmdPushDescriptorSetKHR");
    VkDescriptorSetLayoutBinding b[3];
    for (int i = 0; i < 3; i++)
        b[i] = (VkDescriptorSetLayoutBinding){.binding = (uint32_t)i, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                              .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 3, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof astc_decode_spv,
                                   .pCode = astc_decode_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    if (!G.push || vkCreateDescriptorSetLayout(c->device, &dci, NULL, &G.dsl) != VK_SUCCESS) return 0;
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &G.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    if (vkCreatePipelineLayout(c->device, &li, NULL, &G.layout) != VK_SUCCESS) return 0;
    if (vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS) return 0;
    LARGE_INTEGER t0, t1, fq;
    QueryPerformanceCounter(&t0);
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = G.layout};
    VkResult r = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &G.pipe);
    vkDestroyShaderModule(c->device, m, NULL);
    if (r != VK_SUCCESS) return 0;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = TAB_SIZE,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateBuffer(c->vma, &bi, &aci, &G.tab, &G.tab_alloc, NULL) != VK_SUCCESS) return 0;
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&fq);
    rt_log("vk: ASTC compute decoder ready (pipeline built in %.1f ms)", (double)(t1.QuadPart - t0.QuadPart) * 1e3 / (double)fq.QuadPart);
    return 1;
}

static int native_device(VkCtx *c) { return c->has_astc_ldr; }

static const char *mode_name(int m) {
    return m == VK_ASTC_CPU ? "cpu" : m == VK_ASTC_GPU ? "gpu" : m == VK_ASTC_NATIVE ? "native" : "check";
}

int vk_astc_mode(void) {
    VkCtx *c = vk_ctx();
    if (!c) return VK_ASTC_CPU;
    AcquireSRWLockShared(&g_lock);
    int m = G.dev == c->device ? G.mode : -1;
    ReleaseSRWLockShared(&g_lock);
    if (m >= 0) return m;
    AcquireSRWLockExclusive(&g_lock);
    reset_for(c);
    if (G.mode < 0) {
        const char *e = cfg_get("TOTK_ASTC");
        int want = !e || !*e || !strcmp(e, "auto") ? -1 : !strcmp(e, "cpu") ? VK_ASTC_CPU : !strcmp(e, "gpu") ? VK_ASTC_GPU
                 : !strcmp(e, "native") ? VK_ASTC_NATIVE : !strcmp(e, "check") ? VK_ASTC_CHECK : -2;
        if (want == -2) rt_log("vk: TOTK_ASTC=%s is not cpu, gpu, native, check or auto: auto", e), want = -1;
        if ((want == -1 || want == VK_ASTC_NATIVE) && native_device(c)) m = VK_ASTC_NATIVE;
        else {
            if (want == VK_ASTC_NATIVE) rt_log("vk: TOTK_ASTC=native: the device lacks textureCompressionASTC_LDR; gpu");
            m = want == VK_ASTC_CPU ? VK_ASTC_CPU : want == VK_ASTC_CHECK ? VK_ASTC_CHECK : VK_ASTC_GPU;
        }
        if ((m == VK_ASTC_GPU || m == VK_ASTC_CHECK || m == VK_ASTC_NATIVE) && !G.pipe && !G.failed &&
            !create_pipeline(c)) {
            G.failed = 1;
            rt_log("vk: ASTC compute decoder unavailable on %s: ASTC decodes on the CPU", c->props.deviceName);
        }
        if (G.failed && m != VK_ASTC_NATIVE) m = VK_ASTC_CPU;
        G.mode = m;
        rt_log("vk: ASTC textures: %s (textureCompressionASTC_LDR %d)", mode_name(m), native_device(c));
    }
    m = G.mode;
    ReleaseSRWLockExclusive(&g_lock);
    return m;
}

int vk_astc_native(VkFormat astc_format) {
    VkCtx *c = vk_ctx();
    if (!c || vk_astc_mode() != VK_ASTC_NATIVE) return 0;
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(c->phys, astc_format, &p);
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                                VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    return (p.optimalTilingFeatures & need) == need;
}

int vk_astc_gpu(void) {
    int m = vk_astc_mode();
    if (m == VK_ASTC_GPU || m == VK_ASTC_CHECK) return 1;
    if (m != VK_ASTC_NATIVE) return 0;
    AcquireSRWLockShared(&g_lock);   // native mode, a format the device lacks: the decoder when it exists
    int ok = G.pipe != VK_NULL_HANDLE;
    ReleaseSRWLockShared(&g_lock);
    return ok;
}

static void mem_barrier(VkCommandBuffer cb, VkPipelineStageFlags2 src, VkAccessFlags2 sa, VkPipelineStageFlags2 dst,
                        VkAccessFlags2 da) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = src, .srcAccessMask = sa,
                           .dstStageMask = dst, .dstAccessMask = da};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

int vk_astc_record(VkCommandBuffer cb, VkBuffer src, VkDeviceSize src_size, const VkAstcRegion *rg, int n, uint32_t bw,
                   uint32_t bh, int srgb, VkImage img, VkImageAspectFlags aspect, VkBuffer readback) {
    static volatile long n_fail;
    VkCtx *c = vk_ctx();
    if (!c || !vk_astc_gpu() || bw < 4 || bh < 4 || bw > 12 || bh > 12) return 0;
    AcquireSRWLockShared(&g_lock);
    VkPipeline pipe = G.dev == c->device ? G.pipe : VK_NULL_HANDLE;
    ReleaseSRWLockShared(&g_lock);
    if (!pipe) return 0;
    // scratch: the blocks (a copy of src), then the texels of every region
    VkDeviceSize in_size = (src_size + 15) & ~15ull, out_size = 0;
    for (int i = 0; i < n; i++) out_size += (VkDeviceSize)rg[i].w * rg[i].h * rg[i].slices * 4;
    if (!out_size) return 1;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = in_size + out_size,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkBuffer sb;
    VmaAllocation sa;
    if (vmaCreateBuffer(c->vma, &bi, &aci, &sb, &sa, NULL) != VK_SUCCESS) {
        if (InterlockedIncrement(&n_fail) <= 16)
            rt_log("vk: ASTC scratch buffer of %llu bytes failed: texture left undecoded",
                   (unsigned long long)(in_size + out_size));
        return 0;
    }
    AcquireSRWLockExclusive(&g_lock);
    int need_tab = !G.tab_uploaded;
    G.tab_uploaded = 1;
    VkBuffer tab = G.tab;
    ReleaseSRWLockExclusive(&g_lock);
    if (need_tab) {   // first use on this device: the tables go up in this batch, before the dispatch
        VkBuffer tb;
        uint8_t *p = vk_staging(TAB_SIZE, &tb, NULL);
        fill_tables(p);
        VkBufferCopy cp = {0, 0, TAB_SIZE};
        vkCmdCopyBuffer(cb, tb, tab, 1, &cp);
    }
    VkBufferCopy cp = {0, 0, src_size};
    vkCmdCopyBuffer(cb, src, sb, 1, &cp);
    mem_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    VkDescriptorBufferInfo dbi[3] = {{sb, 0, in_size}, {sb, in_size, out_size}, {tab, 0, TAB_SIZE}};
    VkWriteDescriptorSet w[3];
    for (int i = 0; i < 3; i++)
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = (uint32_t)i,
                                      .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                      .pBufferInfo = &dbi[i]};
    G.push(cb, VK_PIPELINE_BIND_POINT_COMPUTE, G.layout, 0, 3, w);
    VkBufferImageCopy *copies = calloc((size_t)n, sizeof *copies);
    if (!copies) rt_fatal("vk: out of memory (ASTC regions)");
    VkDeviceSize out = 0;
    for (int i = 0; i < n; i++) {
        const VkAstcRegion *r = &rg[i];
        uint32_t bx = (r->w + bw - 1) / bw, by = (r->h + bh - 1) / bh;
        Push pc = {.in_base = (uint32_t)(r->src / 16), .out_base = (uint32_t)(out / 4), .width = r->w, .height = r->h,
                   .bw = bw, .bh = bh, .bx = bx, .bslice = bx * by, .srgb = (uint32_t)(srgb != 0)};
        vkCmdPushConstants(cb, G.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
        vkCmdDispatch(cb, (r->w + 7) / 8, (r->h + 7) / 8, r->slices);
        copies[i] = (VkBufferImageCopy){.bufferOffset = in_size + out, .bufferRowLength = r->w, .bufferImageHeight = r->h,
                                        .imageSubresource = {aspect, r->level, r->layer, 1}, .imageOffset = r->offset,
                                        .imageExtent = {r->w, r->h, r->slices}};
        out += (VkDeviceSize)r->w * r->h * r->slices * 4;
        n_texels += (uint64_t)r->w * r->h * r->slices;
    }
    n_dispatch += (uint64_t)n;
    mem_barrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    vkCmdCopyBufferToImage(cb, sb, img, VK_IMAGE_LAYOUT_GENERAL, (uint32_t)n, copies);
    if (readback) {
        VkBufferCopy rc = {in_size, 0, out_size};
        vkCmdCopyBuffer(cb, sb, readback, 1, &rc);
        mem_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                    VK_ACCESS_2_HOST_READ_BIT);
    }
    free(copies);
    vk_defer_buffer(sb, sa);
    return 1;
}

void vk_astc_release(void) {
    VkCtx *c = vk_ctx();
    AcquireSRWLockExclusive(&g_lock);
    if (c && G.dev == c->device) {
        if (G.pipe) vkDestroyPipeline(c->device, G.pipe, NULL);
        if (G.layout) vkDestroyPipelineLayout(c->device, G.layout, NULL);
        if (G.dsl) vkDestroyDescriptorSetLayout(c->device, G.dsl, NULL);
        if (G.tab) vmaDestroyBuffer(c->vma, G.tab, G.tab_alloc);
    }
    memset(&G.dsl, 0, sizeof G - offsetof(__typeof__(G), dsl));
    G.dev = VK_NULL_HANDLE;
    G.mode = -1;
    G.failed = 0;
    ReleaseSRWLockExclusive(&g_lock);
}

void vk_astc_stats(uint64_t *dispatches, uint64_t *texels) {
    *dispatches = n_dispatch;
    *texels = n_texels;
}
