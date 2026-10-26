// Draws, binds and dispatches of the Vulkan op-stream replay: the strong
// vk_exec_op_draw behind vk_exec.c's weak hook. The bind ops fill the VkExec slots; a draw opens the pass, resolves the
// bound program to translated shaders (the translator's answers come from the bound state), asks vk_pipeline.c for the
// pipeline, pushes (or allocates) set 0 as VkPipeInfo.bindings says and records the dynamic state, vertex and index
// buffers and the draw. A dispatch ends the pass and does the same for the compute shader. UPDATE_UB is a buffer copy
// from transient memory between full barriers.
//
// Per command buffer the replay keeps transient memory (support buffers, copied uniform ranges, generated indices),
// descriptor pools, image views and the storage-buffer ranges the GPU may write; all of it is freed, and the ranges are
// reported with vk_guest_written, from one completion callback once the submission's fence is done (T5b: the write
// watch does not see GPU writes). Nothing here locks: it runs on the submitting thread under vk_sync_submit's lock.
//
// Provenance: the op semantics (bind ops, program and slot conventions, draw and dispatch flow, vertex streams with a
// zeroed buffer for unbound ones) follow MacNativeGames games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m:7409-7460,
// 9365-9488, 9489-9921, 10013-10079 @ b75be8cf (MIT, Copyright (c) 2026 jhaizhou); the uniform-buffer spans, texture and
// sampler descriptor lookups and the dispatch flow follow LAS Recomp runtime/nvn/vulkan/exec.c:1851-1908, 2300-2516
// @ da35b83a (MIT, Copyright (c) 2026 Peiikai). The support buffer layout is Ryujinx's SupportBuffer.cs (MIT).
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_hmap.h"
#include "vk.h"
#include "vk_exec.h"

#define MAX_BINDINGS 128
#define MAX_DESC 1024
#define CHUNK_BYTES (4u << 20)
#define ZERO_BYTES 65536u
#define SUPPORT_BYTES 1408u   // Ryujinx SupportBuffer (std140): alpha_test, is_bgra[8], viewport_inverse, viewport_size, ...
#define NVN_STAGE_VS 0
#define NVN_STAGE_FS 1
#define NVN_STAGE_CS 5
#define ONESHOT_WAIT_MS 30000u   // prepare_draw: how long a one-shot draw waits for its pipeline before it is skipped

enum { W_PROGRAM, W_SHADER, W_PIPELINE, W_NO_VS, W_INDEX, W_UB, W_BIND, W_SSBO_ALIGN, W_SSBO_SLOT, W_RESTART, W_INDIRECT, W_COUNT };

static void warn_once(int key, const char *fmt, ...) {
    static volatile long seen[W_COUNT];
    if (__atomic_exchange_n(&seen[key], 1, __ATOMIC_RELAXED)) return;
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    rt_log("vk: %s (logged once)", msg);
}

static void *grow(void *p, int *cap, size_t elem) {
    *cap = *cap ? *cap * 2 : 8;
    p = realloc(p, elem * (size_t)*cap);
    if (!p) rt_fatal("vk: out of memory");
    return p;
}

// ---- resources of one command buffer, freed by its completion callback

typedef struct { VkBuffer buf; VmaAllocation alloc; uint8_t *map; uint64_t size, used; } Chunk;
typedef struct {
    VkImage image;
    VkFormat format;
    VkImageViewType type;
    uint32_t aspect, level0, levels, layer0, layers, swz;
} ViewKey;
typedef struct { ViewKey k; VkImageView view; } ViewEnt;
_Static_assert(sizeof(ViewKey) == 40, "ViewKey is hashed as bytes: no padding");
typedef struct { uint64_t addr, size; } Range;
typedef struct {
    Chunk *chunks;
    int nchunks, cchunks;
    VkDescriptorPool *pools;
    int npools, cpools;
    ViewEnt *views;
    int nviews, cviews;
    nvn_hmap view_of;                    // ViewKey -> the VkImageView in views
    Range *written;
    int nwritten, cwritten;
} CbRes;

static CbRes *cur;
static uint64_t cur_fence;

// Descriptor pools and full-size transient chunks go back to these lists when their command buffer completes, instead
// of being destroyed and created again for the next one: each creation is a driver memory allocation (on Windows an
// allocation, a map and a paging-fence wait on the CPU), about ten of them per frame inside the replay.
#define SPARE_POOLS 64
#define SPARE_CHUNKS 16
static SRWLOCK spare_lock = SRWLOCK_INIT;
static VkDescriptorPool spare_pool[SPARE_POOLS];
static Chunk spare_chunk[SPARE_CHUNKS];
static int nspare_pool, nspare_chunk;

// State of the command buffer being recorded that the Vulkan command buffer itself holds.
static struct {
    VkPipeline pipe[2];                  // bound per bind point: 0 graphics, 1 compute
    VkPipeState st;                      // the state vk_pipeline_set_dynamic last recorded
    int st_valid;
    VkBuffer vb[17];                     // vertex buffers last bound
    VkDeviceSize vo[17], vsz[17], vstr[17];
    int vb_valid;
    uint8_t support[SUPPORT_BYTES];      // last support buffer and where it lives
    float sup_in[3];                     // the support_fill arguments it was filled from
    VkBuffer sup_buf;
    VkDeviceSize sup_off;
    int sup_valid;
    uint8_t c0[3][VK_C0_BYTES];          // last c0 bank per stage (vertex, fragment, compute) and where it lives
    VkBuffer c0_buf[3];
    VkDeviceSize c0_off[3];
    int c0_valid[3];
    struct { p_bind_buf ssbo[16], ubo[16]; uint32_t dim[3], binst; } c0_in[3];   // what vk_c0_fill read for c0[k]
    VkPipelineLayout ds_layout;          // graphics layout the sets below were last pushed or bound with
    VkDescriptorSet ds_bound[2];         // allocated set bound per set number (0: none or pushed since)
    VkDescriptorSet ds_last[2];          // last allocated and written set per set number, contents in DS
    VkDescriptorSetLayout ds_last_layout[2];
    int ds_pushed[2];                    // set number holds the push of DS.psig under ds_layout
    int res_valid;                       // the graphics sets bound hold what the inputs in RS give (bind_resources)
} B;

// Contents of B.ds_last: the set's packed descriptors (VkDescSlot), which with its layout determine every write. A draw
// whose allocated set would hold the same reuses the set instead of allocating and writing one (Task 2.2's per-draw
// cost). psig: the same for the last push per set number; a push equal to it under the same layout is not recorded
// again (the descriptors stay bound across pipeline binds of that layout).
#define DS_SIG_BYTES (MAX_DESC * sizeof(VkDescSlot))
static struct { uint32_t n[2], pn[2]; uint8_t sig[2][DS_SIG_BYTES], psig[2][DS_SIG_BYTES]; } DS;

// TOTK_BIND_STATS counters (bind_stats).
static struct {
    int on;
    uint64_t draws, bind[2], desc[2], push, push_skip, set_new, set_reuse, ac_miss, same_prog, same_all, bind_skip;
} BS = {-1};

// The inputs of the last graphics bind_resources (bind_sig) and what its build counted for the traffic census, which a
// skipped build counts again. RS_BYTES bounds the bound-slot bytes a pipeline reads (RsList): per binding at most one
// c0 storage table, otherwise one bound range or handle per descriptor.
#define RS_BYTES (MAX_BINDINGS * sizeof(((VkExec *)0)->ssbo[0]) + MAX_DESC * sizeof(p_bind_buf))
typedef struct { int cls, use; uint64_t bytes; } RsCount;
static struct {
    int on;                              // TOTK_BIND_SKIP: 1 skip (default), 2 check, 0 off
    uint32_t hn;                         // words of h the last signature filled; 0: none
    uint64_t h[24];                      // generations, feedback images, support inputs, base instance
    const VkPipeInfo *info;              // the pipeline whose RsList ranges body holds, as of bind op generation gen
    uint64_t gen;
    uint8_t body[RS_BYTES];
    int rec;                             // a graphics build is running: count_use records, scratch copies mark volatile
    int volatile_;                       // the build copied guest bytes into transient memory: a rebuild differs
    RsCount cnt[2 * VK_MCLS_N], pcnt[2 * VK_MCLS_N];
    int ncnt, pncnt, cnt_over;
    int cmds;                            // check mode: pushes, binds and set writes the build recorded
    uint32_t ckn;                        // check mode: the last build's packed descriptors (bytes in ck)
    uint8_t ck[DS_SIG_BYTES];
} RS = {-1};

// The bound-slot bytes of VkExec a pipeline's descriptors read, as ranges merged where they touch, built once per
// pipeline info: c0's storage table, the uniform and storage ranges of the bound slots and the texture and image
// handles. sup and c0: the support inputs and the base instance count too. ok 0: never skipped (too many descriptors).
typedef struct { uint32_t off, len; } RsRange;
typedef struct { uint32_t n, bytes; uint8_t sup, c0, ok; RsRange r[]; } RsList;
static nvn_hmap rs_lists;                // VkPipeInfo pointer -> RsList, until shutdown
static const VkPipeInfo *rs_last;        // the last lookup
static const RsList *rs_last_l;

static uint32_t c0_dim[3], c0_binst;     // the dispatch size and base instance of the draw or dispatch being bound
static uint64_t bind_gen = 1;            // bumped by every bind op that writes ex->prog, ubo, ssbo, tex or img

static void cb_done(void *arg) {   // completion thread
    CbRes *r = arg;
    VkCtx *c = vk_ctx();
    for (int i = 0; i < r->nwritten; i++) vk_guest_written(r->written[i].addr, r->written[i].size);
    for (int i = 0; i < r->nviews; i++) vkDestroyImageView(c->device, r->views[i].view, NULL);
    for (int i = 0; i < r->npools; i++) {
        vkResetDescriptorPool(c->device, r->pools[i], 0);
        AcquireSRWLockExclusive(&spare_lock);
        int kept = nspare_pool < SPARE_POOLS;
        if (kept) spare_pool[nspare_pool++] = r->pools[i];
        ReleaseSRWLockExclusive(&spare_lock);
        if (!kept) vkDestroyDescriptorPool(c->device, r->pools[i], NULL);
    }
    for (int i = 0; i < r->nchunks; i++) {
        AcquireSRWLockExclusive(&spare_lock);
        int kept = r->chunks[i].size == CHUNK_BYTES && nspare_chunk < SPARE_CHUNKS;
        if (kept) spare_chunk[nspare_chunk++] = r->chunks[i];
        ReleaseSRWLockExclusive(&spare_lock);
        if (!kept) vmaDestroyBuffer(c->vma, r->chunks[i].buf, r->chunks[i].alloc);
    }
    free(r->view_of.keys), free(r->view_of.vals);
    free(r->written), free(r->views), free(r->pools), free(r->chunks), free(r);
}

void vk_exec_draw_new_cb(void) { memset(&B, 0, sizeof B); }

static CbRes *cb_res(VkExec *ex) {
    if (cur && cur_fence == ex->fence) return cur;
    cur = calloc(1, sizeof *cur);
    if (!cur) rt_fatal("vk: out of memory");
    nvn_hm_init(&cur->view_of, sizeof(ViewKey));
    cur_fence = ex->fence;
    memset(&B, 0, sizeof B);
    vk_completion_add(ex->fence, cb_done, cur);
    return cur;
}

static uint8_t *scratch(VkExec *ex, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {
    CbRes *r = cb_res(ex);
    size = (size + 255) & ~255ull;
    Chunk *c = r->nchunks ? &r->chunks[r->nchunks - 1] : NULL;
    if (!c || c->used + size > c->size) {
        if (r->nchunks == r->cchunks) r->chunks = grow(r->chunks, &r->cchunks, sizeof *r->chunks);
        c = &r->chunks[r->nchunks];
        uint64_t n = size > CHUNK_BYTES ? size : CHUNK_BYTES;
        int reused = 0;
        if (n == CHUNK_BYTES) {
            AcquireSRWLockExclusive(&spare_lock);
            if (nspare_chunk) *c = spare_chunk[--nspare_chunk], reused = 1;
            ReleaseSRWLockExclusive(&spare_lock);
        }
        if (reused) {
            c->used = 0;
            r->nchunks++;
            *buf = c->buf;
            *off = 0;
            c->used = size;
            return c->map;
        }
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n,
                                 .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        VkResult res = vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &c->buf, &c->alloc, &info);
        if (res != VK_SUCCESS) rt_fatal("vk: transient buffer of %llu bytes failed (%d)", (unsigned long long)n, (int)res);
        c->map = info.pMappedData, c->size = n, c->used = 0;
        r->nchunks++;
    }
    *buf = c->buf;
    *off = c->used;
    uint8_t *p = c->map + c->used;
    c->used += size;
    return p;
}

static void note_written(VkExec *ex, uint64_t addr, uint64_t size) {
    CbRes *r = cb_res(ex);
    for (int i = 0; i < r->nwritten; i++)
        if (r->written[i].addr == addr && r->written[i].size == size) return;
    if (r->nwritten == r->cwritten) r->written = grow(r->written, &r->cwritten, sizeof *r->written);
    r->written[r->nwritten++] = (Range){addr, size};
}

// ---- stand-ins for what a binding names but the game did not bind (zeros, a 1x1 image, the default sampler)

enum { DV_1D, DV_1D_ARRAY, DV_2D, DV_2D_ARRAY, DV_3D, DV_CUBE, DV_CUBE_ARRAY, DV_2D_MS, DV_2D_MS_ARRAY, DV_COUNT };

static struct {
    int ready;
    VkBuffer zbuf;
    VmaAllocation zalloc;
    VkBufferView zview;
    VkSampler samp, samp_shadow;
    struct { VkImage image; VmaAllocation alloc; VkImageView view, sview; } img[DV_COUNT];
} Z;

static const struct { VkImageType t; VkImageViewType vt; uint32_t layers; VkSampleCountFlagBits s; VkImageCreateFlags f; } dinfo[DV_COUNT] = {
    {VK_IMAGE_TYPE_1D, VK_IMAGE_VIEW_TYPE_1D, 1, VK_SAMPLE_COUNT_1_BIT, 0},
    {VK_IMAGE_TYPE_1D, VK_IMAGE_VIEW_TYPE_1D_ARRAY, 1, VK_SAMPLE_COUNT_1_BIT, 0},
    {VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, 1, VK_SAMPLE_COUNT_1_BIT, 0},
    {VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 1, VK_SAMPLE_COUNT_1_BIT, 0},
    {VK_IMAGE_TYPE_3D, VK_IMAGE_VIEW_TYPE_3D, 1, VK_SAMPLE_COUNT_1_BIT, 0},
    {VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE, 6, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT},
    {VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, 6, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT},
    {VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, 1, VK_SAMPLE_COUNT_4_BIT, 0},
    {VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 1, VK_SAMPLE_COUNT_4_BIT, 0}};

// A 1x1 stand-in image of view type dinfo[i] in `fmt`, moved to GENERAL by the upload batch (which reaches the queue before
// the submission that uses it). *sview is the view a storage descriptor takes (a cube becomes a 2D array).
static void make_dummy(int i, VkFormat fmt, VkImage *image, VmaAllocation *alloc, VkImageView *view, VkImageView *sview) {
    VkCtx *c = vk_ctx();
    int ms = dinfo[i].s != VK_SAMPLE_COUNT_1_BIT;
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = dinfo[i].f, .imageType = dinfo[i].t,
                            .format = fmt, .extent = {1, 1, 1}, .mipLevels = 1, .arrayLayers = dinfo[i].layers,
                            .samples = dinfo[i].s, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | (ms ? 0 : VK_IMAGE_USAGE_STORAGE_BIT),
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ia = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ii, &ia, image, alloc, NULL) != VK_SUCCESS) rt_fatal("vk: stand-in image %d failed", i);
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = *image,
                                .viewType = dinfo[i].vt, .format = fmt,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, dinfo[i].layers}};
    if (vkCreateImageView(c->device, &vi, NULL, view) != VK_SUCCESS) rt_fatal("vk: stand-in view failed");
    *sview = *view;
    if (!ms && (dinfo[i].vt == VK_IMAGE_VIEW_TYPE_CUBE || dinfo[i].vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY)) {
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        if (vkCreateImageView(c->device, &vi, NULL, sview) != VK_SUCCESS) rt_fatal("vk: stand-in view failed");
    }
    VkImageMemoryBarrier2 bar = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                 .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                 .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                 .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                 .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                 .image = *image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, dinfo[i].layers}};
    VkCommandBuffer cb = vk_upload_begin();
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &bar};
    vkCmdPipelineBarrier2(cb, &di);
    vk_upload_end();
}

// Storage image stand-ins in the format a shader declares (GfxShaderRes.format: Ryujinx TextureFormat order); Vulkan wants
// the view format to match the declaration. Created on first use.
static const VkFormat tex_format[40] = {
    VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SNORM, VK_FORMAT_R8_UINT, VK_FORMAT_R8_SINT,
    VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16_UNORM, VK_FORMAT_R16_SNORM, VK_FORMAT_R16_UINT, VK_FORMAT_R16_SINT,
    VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SNORM,
    VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8_SINT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM,
    VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_SINT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_SINT,
    VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT,
    VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_UINT,
    VK_FORMAT_R16G16B16A16_SINT, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_SINT,
    VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2B10G10R10_UINT_PACK32, VK_FORMAT_B10G11R11_UFLOAT_PACK32};
static struct { int used, di; VkFormat fmt; VkImage image; VmaAllocation alloc; VkImageView view, sview; } SD[48];

static VkImageView storage_dummy(int di, int code) {
    VkFormat fmt = code > 0 && code < 40 ? tex_format[code] : VK_FORMAT_R8G8B8A8_UNORM;
    if (fmt == VK_FORMAT_R8G8B8A8_UNORM || dinfo[di].s != VK_SAMPLE_COUNT_1_BIT) return Z.img[di].sview;
    for (int i = 0; i < 48; i++) {
        if (!SD[i].used) {
            SD[i].used = 1, SD[i].di = di, SD[i].fmt = fmt;
            make_dummy(di, fmt, &SD[i].image, &SD[i].alloc, &SD[i].view, &SD[i].sview);
            return SD[i].sview;
        }
        if (SD[i].di == di && SD[i].fmt == fmt) return SD[i].sview;
    }
    return Z.img[di].sview;
}

static void dummies_init(void) {
    if (Z.ready) return;
    VkCtx *c = vk_ctx();
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = ZERO_BYTES,
                             .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                      VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    if (vmaCreateBuffer(c->vma, &bi, &ai, &Z.zbuf, &Z.zalloc, &info) != VK_SUCCESS) rt_fatal("vk: zero buffer failed");
    memset(info.pMappedData, 0, ZERO_BYTES);
    VkBufferViewCreateInfo bv = {.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = Z.zbuf,
                                 .format = VK_FORMAT_R32G32B32A32_SFLOAT, .range = ZERO_BYTES};
    if (vkCreateBufferView(c->device, &bv, NULL, &Z.zview) != VK_SUCCESS) rt_fatal("vk: zero buffer view failed");
    samp_par sp;
    memset(&sp, 0, sizeof sp);
    sp.lod_max = 1000.0f;
    Z.samp = vk_sampler_for(&sp);
    sp.cmp_mode = 1, sp.cmp_func = NVN_DEPTH_FUNC_ALWAYS;
    Z.samp_shadow = vk_sampler_for(&sp);

    for (int i = 0; i < DV_COUNT; i++)
        make_dummy(i, VK_FORMAT_R8G8B8A8_UNORM, &Z.img[i].image, &Z.img[i].alloc, &Z.img[i].view, &Z.img[i].sview);
    Z.ready = 1;
}

// Releases the stand-ins. The device must be idle; the game path leaves them to process exit like the pipelines.
void vk_exec_draw_shutdown(void) {
    if (!Z.ready) return;
    VkCtx *c = vk_ctx();
    for (int i = 0; i < DV_COUNT; i++) {
        if (Z.img[i].sview != Z.img[i].view) vkDestroyImageView(c->device, Z.img[i].sview, NULL);
        vkDestroyImageView(c->device, Z.img[i].view, NULL);
        vmaDestroyImage(c->vma, Z.img[i].image, Z.img[i].alloc);
    }
    for (int i = 0; i < 48 && SD[i].used; i++) {
        if (SD[i].sview != SD[i].view) vkDestroyImageView(c->device, SD[i].sview, NULL);
        vkDestroyImageView(c->device, SD[i].view, NULL);
        vmaDestroyImage(c->vma, SD[i].image, SD[i].alloc);
    }
    memset(SD, 0, sizeof SD);
    nvn_hm_clear(&rs_lists, free);
    rs_last = NULL, rs_last_l = NULL;
    for (int i = 0; i < nspare_pool; i++) vkDestroyDescriptorPool(c->device, spare_pool[i], NULL);
    for (int i = 0; i < nspare_chunk; i++) vmaDestroyBuffer(c->vma, spare_chunk[i].buf, spare_chunk[i].alloc);
    nspare_pool = nspare_chunk = 0;
    vkDestroyBufferView(c->device, Z.zview, NULL);
    vmaDestroyBuffer(c->vma, Z.zbuf, Z.zalloc);
    memset(&Z, 0, sizeof Z);
}

// ---- resource types

static VkImageViewType res_view_type(uint16_t t) {
    int arr = t & 0x100;
    switch (t & 0xff) {
    case 1: return arr ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
    case 4: return VK_IMAGE_VIEW_TYPE_3D;
    case 5: return arr ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
    default: return arr ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    }
}

static int dummy_index(VkImageViewType vt, int ms) {
    switch (vt) {
    case VK_IMAGE_VIEW_TYPE_1D: return DV_1D;
    case VK_IMAGE_VIEW_TYPE_1D_ARRAY: return DV_1D_ARRAY;
    case VK_IMAGE_VIEW_TYPE_3D: return DV_3D;
    case VK_IMAGE_VIEW_TYPE_CUBE: return DV_CUBE;
    case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY: return DV_CUBE_ARRAY;
    case VK_IMAGE_VIEW_TYPE_2D_ARRAY: return ms ? DV_2D_MS_ARRAY : DV_2D_ARRAY;
    default: return ms ? DV_2D_MS : DV_2D;
    }
}

// GFX_TEX_* of a bound texture slot (the translator's tex_type / img_type answers).
static int gfx_type_of(const VkTexImage *img, const view_t *v, int hv) {
    if (hv && v->set_target) return vk_pipeline_tex_type(v->target);
    int ms = img->samples > 1;
    switch (img->view_type) {
    case VK_IMAGE_VIEW_TYPE_1D: return GFX_TEX_1D;
    case VK_IMAGE_VIEW_TYPE_1D_ARRAY: return GFX_TEX_1D_ARRAY;
    case VK_IMAGE_VIEW_TYPE_2D_ARRAY: return ms ? GFX_TEX_2D_MS_ARRAY : GFX_TEX_2D_ARRAY;
    case VK_IMAGE_VIEW_TYPE_3D: return GFX_TEX_3D;
    case VK_IMAGE_VIEW_TYPE_CUBE: return GFX_TEX_CUBE;
    case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY: return GFX_TEX_CUBE_ARRAY;
    default: return ms ? GFX_TEX_2D_MS : GFX_TEX_2D;
    }
}

static VkComponentSwizzle swz_of(uint8_t s) {
    switch (s) {
    case NVN_TEXTURE_SWIZZLE_ZERO: return VK_COMPONENT_SWIZZLE_ZERO;
    case NVN_TEXTURE_SWIZZLE_ONE: return VK_COMPONENT_SWIZZLE_ONE;
    case NVN_TEXTURE_SWIZZLE_R: return VK_COMPONENT_SWIZZLE_R;
    case NVN_TEXTURE_SWIZZLE_G: return VK_COMPONENT_SWIZZLE_G;
    case NVN_TEXTURE_SWIZZLE_B: return VK_COMPONENT_SWIZZLE_B;
    default: return VK_COMPONENT_SWIZZLE_A;
    }
}

// A view in STENCIL mode samples the stencil plane (TextureViewSetDepthStencilMode); otherwise depth wins. The
// translator declares every sampled image float and the shader takes the raw bits, as for integer formats (nxnative
// nvn_mtl.m:3174).
VkImageAspectFlags vk_exec_view_aspect(const VkTexImage *img, const view_t *v, int hv) {
    if (hv && v->set_dsmode && v->dsmode == NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL && (img->aspect & VK_IMAGE_ASPECT_STENCIL_BIT))
        return VK_IMAGE_ASPECT_STENCIL_BIT;
    return img->aspect & VK_IMAGE_ASPECT_DEPTH_BIT     ? VK_IMAGE_ASPECT_DEPTH_BIT
           : img->aspect & VK_IMAGE_ASPECT_STENCIL_BIT ? VK_IMAGE_ASPECT_STENCIL_BIT
                                                       : VK_IMAGE_ASPECT_COLOR_BIT;
}

// An image view of a registered texture for a shader resource of type `rtype` (GfxShaderRes.type); VK_NULL_HANDLE when
// the image cannot serve it (the caller binds a stand-in).
static VkImageView get_view(VkExec *ex, const VkTexImage *img, const view_t *v, int hv, uint16_t rtype, int storage) {
    VkImageViewType vt = res_view_type(rtype);
    int ms = (rtype & 0x200) != 0;
    if ((img->samples > 1) != ms || (storage && ms)) return VK_NULL_HANDLE;
    if (storage && !(img->usage & VK_IMAGE_USAGE_STORAGE_BIT)) return VK_NULL_HANDLE;
    int cube = vt == VK_IMAGE_VIEW_TYPE_CUBE || vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    int ok = 0;
    switch (vt) {
    case VK_IMAGE_VIEW_TYPE_1D:
    case VK_IMAGE_VIEW_TYPE_1D_ARRAY: ok = img->type == VK_IMAGE_TYPE_1D; break;
    case VK_IMAGE_VIEW_TYPE_3D: ok = img->type == VK_IMAGE_TYPE_3D; break;
    case VK_IMAGE_VIEW_TYPE_CUBE:
    case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
        ok = img->type == VK_IMAGE_TYPE_2D && (img->view_type == VK_IMAGE_VIEW_TYPE_CUBE || img->view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY);
        break;
    default: ok = img->type == VK_IMAGE_TYPE_2D; break;
    }
    if (!ok) return VK_NULL_HANDLE;
    uint32_t l0 = hv && v->set_levels ? v->base_level : 0;
    if (l0 >= img->levels) return VK_NULL_HANDLE;
    uint32_t nl = hv && v->set_levels && v->nlevels ? v->nlevels : img->levels - l0;
    if (l0 + nl > img->levels) nl = img->levels - l0;
    if (storage) nl = 1;
    uint32_t y0 = 0, ny = 1;
    if (vt != VK_IMAGE_VIEW_TYPE_3D) {
        y0 = hv && v->set_layers ? v->min_layer : 0;
        if (y0 >= img->layers) return VK_NULL_HANDLE;
        uint32_t rest = hv && v->set_layers && v->nlayers ? v->nlayers : img->layers - y0;
        if (y0 + rest > img->layers) rest = img->layers - y0;
        if (vt == VK_IMAGE_VIEW_TYPE_1D || vt == VK_IMAGE_VIEW_TYPE_2D) ny = 1;
        else if (vt == VK_IMAGE_VIEW_TYPE_CUBE) ny = 6;
        else if (vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) ny = rest / 6 * 6;
        else ny = rest;
        if (!ny || y0 + ny > img->layers) return VK_NULL_HANDLE;
    }
    if (storage && cube) vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    uint32_t aspect = vk_exec_view_aspect(img, v, hv);
    uint8_t sw[4] = {NVN_TEXTURE_SWIZZLE_R, NVN_TEXTURE_SWIZZLE_G, NVN_TEXTURE_SWIZZLE_B, NVN_TEXTURE_SWIZZLE_A};
    if (!storage) {   // MNG nvn_mtl.m tex_swizzle_sel: view selectors replace the texture's, then the format remap
        memcpy(sw, hv && v->set_swizzle ? v->swz : img->swz, 4);
        if (img->alpha_one)
            for (int i = 0; i < 4; i++)
                if (sw[i] == NVN_TEXTURE_SWIZZLE_A) sw[i] = NVN_TEXTURE_SWIZZLE_ONE;
    }
    uint32_t swz = (uint32_t)sw[0] | (uint32_t)sw[1] << 8 | (uint32_t)sw[2] << 16 | (uint32_t)sw[3] << 24;
    CbRes *r = cb_res(ex);
    ViewKey k = {img->image, img->format, vt, aspect, l0, nl, y0, ny, swz};
    VkImageView found = (VkImageView)nvn_hm_get(&r->view_of, &k);
    if (found) return found;
    VkImageViewCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img->image, .viewType = vt,
                                .format = img->format,
                                .components = {swz_of(sw[0]), swz_of(sw[1]), swz_of(sw[2]), swz_of(sw[3])},
                                .subresourceRange = {aspect, l0, nl, y0, ny}};
    VkImageViewUsageCreateInfo use = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
                                      .usage = VK_IMAGE_USAGE_SAMPLED_BIT};
    if (img->block_views) ci.pNext = &use;   // the image's COLOR_ATTACHMENT usage is its block views' (vk_texture.c)
    VkImageView view;
    if (vkCreateImageView(vk_ctx()->device, &ci, NULL, &view) != VK_SUCCESS) return VK_NULL_HANDLE;
    if (r->nviews == r->cviews) r->views = grow(r->views, &r->cviews, sizeof *r->views);
    r->views[r->nviews++] = (ViewEnt){k, view};
    nvn_hm_put(&r->view_of, &k, (void *)view);
    return view;
}

// ---- program to shaders

// (program, stage, answers) -> shader, 4-way set-associative, most recently used first. A miss costs a SHA-256 of the
// whole shader code (gfx_shader.c req_init), slow on CPUs without SHA instructions: a field frame looks up ~3000 and
// the 2048-slot direct-mapped table this replaced missed 3% of them on conflicts.
#define SHADER_SETS 2048
#define SHADER_WAYS 4
typedef struct { uint64_t serial; int stage; int used; GfxShaderAnswers a; const GfxShader *s; } ShaderEnt;
static ShaderEnt shader_tab[SHADER_SETS][SHADER_WAYS];

static int find_stage(const prog_t *p, int nvn_stage, const uint8_t **ctl, const uint8_t **code, uint32_t *len) {
    for (uint32_t i = 0; i < p->nstage && i < 6; i++) {
        const uint8_t *c = p->sd[i].control;
        if (!c || !p->sd[i].data) continue;
        uint32_t st;
        memcpy(&st, c + 0x714, 4);
        if ((int)st != nvn_stage) continue;
        memcpy(len, c + 0x704, 4);
        *ctl = c, *code = (const uint8_t *)(uintptr_t)p->sd[i].data;
        return 1;
    }
    return 0;
}

// Texture and image types of a stage's first 32 slots, kept while every slot's resolution (vk_texture_slots_gen)
// stays the same: only slots whose handle changed are looked up again, each lookup being a hash probe.
static struct { int valid; uint32_t gen; uint64_t tex[32], img[32]; uint8_t tt[32], it[32]; } AC[6];
static int shader_memo_on(void);

static void answers_for(VkExec *ex, int st, const uint8_t *ctl, GfxShaderAnswers *a) {
    memset(a, 0, sizeof *a);
    if (st == NVN_STAGE_VS) vk_pipeline_answers(&ex->pipe, a, NULL);
    else if (st == NVN_STAGE_FS) vk_pipeline_answers(&ex->pipe, NULL, a);
    else
        for (int k = 0; k < 3; k++) {
            uint32_t l;
            memcpy(&l, ctl + 0x748 + 4 * k, 4);
            a->local[k] = (uint16_t)(l ? l : 1);
        }
    typeof(AC[0]) *c = &AC[st];
    vk_texture_lock();
    uint32_t gen = vk_texture_slots_gen();   // before the lookups: one that creates an entry bumps it
    int all = !c->valid || c->gen != gen;
    if (all || memcmp(c->tex, ex->tex[st], sizeof c->tex) || memcmp(c->img, ex->img[st], sizeof c->img)) {
        for (int slot = 0; slot < 32; slot++) {
            view_t v;
            int hv;
            uint64_t h = ex->tex[st][slot];
            if (all || h != c->tex[slot]) {
                const VkTexImage *img = h ? vk_texture_by_id_locked((int)(h & 0xfffff), &v, &hv) : NULL;
                c->tt[slot] = img ? (uint8_t)gfx_type_of(img, &v, hv) : 0;
            }
            h = ex->img[st][slot];
            if (all || h != c->img[slot]) {
                const VkTexImage *img = h ? vk_texture_by_id_locked((int)(h & 0xfffff), &v, &hv) : NULL;
                c->it[slot] = img ? (uint8_t)gfx_type_of(img, &v, hv) : 0;
            }
        }
        memcpy(c->tex, ex->tex[st], sizeof c->tex);
        memcpy(c->img, ex->img[st], sizeof c->img);
        c->gen = gen, c->valid = 1;
        BS.ac_miss++;
    }
    if (shader_memo_on() == 2)   // check mode: every slot looked up again
        for (int slot = 0; slot < 32; slot++) {
            view_t v;
            int hv;
            uint64_t h = ex->tex[st][slot], hi = ex->img[st][slot];
            const VkTexImage *img = h ? vk_texture_by_id_locked((int)(h & 0xfffff), &v, &hv) : NULL;
            int tt = img ? gfx_type_of(img, &v, hv) : 0;
            img = hi ? vk_texture_by_id_locked((int)(hi & 0xfffff), &v, &hv) : NULL;
            int it = img ? gfx_type_of(img, &v, hv) : 0;
            if (tt != c->tt[slot] || it != c->it[slot])
                rt_fatal("vk_exec: kept texture types of stage %d slot %d differ from a lookup", st, slot);
        }
    vk_texture_unlock();
    memcpy(a->tex_type, c->tt, sizeof c->tt);
    memcpy(a->img_type, c->it, sizeof c->it);
}

// *present: the program has this stage. Returns NULL when it has not or the translation failed. pending (graphics
// stages, vk_pipeline_async_on): a shader not translated yet is queued and NULL returned with *pending = 1. await: a
// shader queued earlier is waited for (gfx_shader_await) instead.
static const GfxShader *shader_for(VkExec *ex, int st, int *present, int *pending, int await) {
    const uint8_t *ctl, *code;
    uint32_t len;
    *present = 0;
    if (pending) *pending = 0;
    if (!ex->prog || !find_stage(ex->prog, st, &ctl, &code, &len)) return NULL;
    *present = 1;
    GfxShaderAnswers a;
    answers_for(ex, st, ctl, &a);
    uint64_t serial = ex->prog->serial;
    ShaderEnt *set = NULL;
    if (serial) {
        uint64_t h = (serial * 0x9E3779B97F4A7C15ull ^ (uint64_t)st) ^ nvn_hm_hash(&a, sizeof a);   // a is zero-padded
        set = shader_tab[(h ^ h >> 29) & (SHADER_SETS - 1)];
        for (int w = 0; w < SHADER_WAYS && set[w].used; w++) {
            ShaderEnt *e = &set[w];
            if (e->serial != serial || e->stage != st || memcmp(&e->a, &a, sizeof a)) continue;
            if (w) {   // to the front
                ShaderEnt hit = *e;
                memmove(&set[1], &set[0], w * sizeof *set);
                set[0] = hit;
            }
            return set[0].s;
        }
    }
    int gs = st == NVN_STAGE_VS ? GFX_STAGE_VS : st == NVN_STAGE_FS ? GFX_STAGE_FS : GFX_STAGE_CS;
    const GfxShader *s = await ? gfx_shader_await(gs, ctl, code, len, &a)
                       : pending && vk_pipeline_async_on() ? gfx_shader_get_async(gs, ctl, code, len, &a, pending)
                                                           : gfx_shader_get(gs, ctl, code, len, NULL, &a);
    if (pending && *pending) return NULL;
    if (!s) warn_once(W_SHADER, "a stage %d shader of program %llu failed to translate: draws with it are skipped", st,
                      (unsigned long long)serial);
    else if (set) {   // in front, the least recently used way drops out
        memmove(&set[1], &set[0], (SHADER_WAYS - 1) * sizeof *set);
        set[0] = (ShaderEnt){.serial = serial, .stage = st, .used = 1, .a = a, .s = s};
    }
    return s;
}

// ---- descriptors

static int nvn_stage_of(int gfx_stage) {
    return gfx_stage == GFX_STAGE_VS ? NVN_STAGE_VS : gfx_stage == GFX_STAGE_FS ? NVN_STAGE_FS : NVN_STAGE_CS;
}

static VkDescriptorBufferInfo zero_info(uint64_t max) {
    return (VkDescriptorBufferInfo){Z.zbuf, 0, max < ZERO_BYTES ? max : ZERO_BYTES};
}

// vk_mem_count, also kept per (class, use) while a graphics build runs (RS).
static void count_use(int cls, int use, uint64_t n) {
    vk_mem_count(cls, use, n);
    if (!RS.rec) return;
    for (int i = 0; i < RS.ncnt; i++)
        if (RS.cnt[i].cls == cls && RS.cnt[i].use == use) {
            RS.cnt[i].bytes += n;
            return;
        }
    if (RS.ncnt < (int)(sizeof RS.cnt / sizeof RS.cnt[0])) RS.cnt[RS.ncnt++] = (RsCount){cls, use, n};
    else RS.cnt_over = 1;
}

// A buffer descriptor for [addr, addr+size) of guest memory: its backing (a pool's VRAM mirror when it has one) when
// aligned, a copy in transient memory when the backing is host memory with an unaligned offset. 0 when neither works.
// written: the shader stores into it (a mirror's range goes back to guest memory at the end of the submission).
static int bind_range(VkExec *ex, uint64_t addr, uint64_t size, uint64_t align, uint64_t max, int use, int written,
                      VkDescriptorBufferInfo *out) {
    VkMemSpan sp;
    if (!addr || !vk_mem_resolve(addr, 1, &sp)) return 0;
    uint64_t n = size && size < sp.avail ? size : sp.avail;
    if (n > max) n = max;
    if (!n) return 0;
    if (sp.offset % align == 0) {
        VK_MEM_USE(sp);
        *out = (VkDescriptorBufferInfo){sp.buffer, sp.offset, n};
        if (written) vk_mem_gpu_wrote(&sp, n);
        count_use(sp.cls, use, n);
        VK_MEM_BOUND(sp);
        return 1;
    }
    if (sp.alloc) return 0;
    VkBuffer b;
    VkDeviceSize o;
    RS.volatile_ = 1;
    memcpy(scratch(ex, n, &b, &o), (const void *)(uintptr_t)addr, n);
    vk_mem_count(sp.cls, VK_MUSE_CPU_READ, n);
    vk_mem_count(VK_MCLS_SCRATCH, use, n);
    *out = (VkDescriptorBufferInfo){b, o, n};
    return 1;
}

static void c0_table(uint8_t *at, const p_bind_buf b[16]) {
    for (int r = 0; r < 16; r++) {
        uint64_t a = b[r].addr;
        uint32_t n = (uint32_t)b[r].size;
        memcpy(at + 16 * r, &a, 8);
        memcpy(at + 16 * r + 8, &n, 4);
    }
}

// The driver constant bank c0 as GLSLC-compiled shaders read it. Storage-buffer tables {u64 address, u32 size} every
// 16 bytes at 0x110 (vertex), 0x510 (fragment), 0x310 (compute): Ryujinx's GlobalToStorage fallback loads each
// candidate base from here and compares the global pointer against it. Compute: dispatch size at 0x200, the uniform
// buffer table at 0x210 + 16 r. Graphics: the largest point size (Maxwell's limit, GLSLC clamps gl_PointSize to it) at
// 0x610, the base instance at 0x644. Other stages: zeros. Unbound slots read zero, since the replay keeps them zero.
// Layout: nxnative dm/gfx/nvn/nvn_mtl.m:4395-4420 (MIT); TotK readers in build/lessons/census.md G2.
void vk_c0_fill(uint8_t bank[VK_C0_BYTES], int stage, const p_bind_buf ssbo[16], const p_bind_buf ubo[16],
                const uint32_t dispatch[3], uint32_t base_instance) {
    memset(bank, 0, VK_C0_BYTES);
    int base = stage == NVN_STAGE_VS ? 0x110 : stage == NVN_STAGE_FS ? 0x510 : stage == NVN_STAGE_CS ? 0x310 : -1;
    if (base < 0) return;
    c0_table(bank + base, ssbo);
    if (stage == NVN_STAGE_CS) {
        memcpy(bank + 0x200, dispatch, 12);
        c0_table(bank + 0x210, ubo);
        return;
    }
    const float max_point = 2047.0f;
    memcpy(bank + 0x610, &max_point, 4);
    memcpy(bank + 0x644, &base_instance, 4);
}

static int c0_on(void) {   // TOTK_FIX_C0=0: c0 reads zeros, as in M4
    static int on = -1;
    if (on < 0) {
        const char *e = cfg_get("TOTK_FIX_C0");
        on = !(e && !strcmp(e, "0"));
    }
    return on;
}

// c0 from replay scratch; a bank equal to the stage's previous one reuses its range, so it costs no copy.
static VkDescriptorBufferInfo c0_info(VkExec *ex, int st) {
    int k = st == NVN_STAGE_VS ? 0 : st == NVN_STAGE_FS ? 1 : 2;
    // vk_c0_fill reads the storage slots, and the uniform slots and dispatch size for compute or the base instance for
    // graphics: equal inputs give the same bank, so neither the fill nor the 2 KiB compare is needed.
    typeof(B.c0_in[0]) in;
    memset(&in, 0, sizeof in);
    memcpy(in.ssbo, ex->ssbo[st], sizeof in.ssbo);
    if (st == NVN_STAGE_CS) memcpy(in.ubo, ex->ubo[st], sizeof in.ubo), memcpy(in.dim, c0_dim, sizeof in.dim);
    else in.binst = c0_binst;
    if (B.c0_valid[k] && !memcmp(&in, &B.c0_in[k], sizeof in))
        return (VkDescriptorBufferInfo){B.c0_buf[k], B.c0_off[k], VK_C0_BYTES};
    B.c0_in[k] = in;
    uint8_t tmp[VK_C0_BYTES];
    vk_c0_fill(tmp, st, ex->ssbo[st], ex->ubo[st], c0_dim, c0_binst);
    if (!B.c0_valid[k] || memcmp(tmp, B.c0[k], VK_C0_BYTES)) {
        memcpy(scratch(ex, VK_C0_BYTES, &B.c0_buf[k], &B.c0_off[k]), tmp, VK_C0_BYTES);
        memcpy(B.c0[k], tmp, VK_C0_BYTES);
        B.c0_valid[k] = 1;
    }
    return (VkDescriptorBufferInfo){B.c0_buf[k], B.c0_off[k], VK_C0_BYTES};
}

static VkDescriptorBufferInfo ubo_info(VkExec *ex, int st, int slot) {
    uint64_t max = vk_ctx()->props.limits.maxUniformBufferRange, align = vk_ctx()->props.limits.minUniformBufferOffsetAlignment;
    VkDescriptorBufferInfo bi;
    if (slot == 0 && c0_on()) return c0_info(ex, st);
    if (slot == 1 && ex->prog) {   // compiler constants stored after the shader code
        const uint8_t *ctl, *code;
        uint32_t len, c1;
        if (find_stage(ex->prog, st, &ctl, &code, &len)) {
            memcpy(&c1, ctl + 0x700, 4);
            if (c1 < len && bind_range(ex, (uint64_t)(uintptr_t)code + c1, len - c1, align, max, VK_MUSE_UBO, 0, &bi))
                return bi;
        }
    } else if (slot >= 3 && slot - 3 < 18) {
        const p_bind_buf *b = &ex->ubo[st][slot - 3];
        if (bind_range(ex, b->addr, b->size, align, max, VK_MUSE_UBO, 0, &bi)) return bi;
    }
    return zero_info(max);
}

// The NVN slot of a storage binding. Ryujinx numbers a shader's storage buffers in the order it meets them
// (GfxShaderRes.slot); the NVN slot is the entry of the driver constant buffer c0 that holds the buffer's address
// (cbuf_slot 0, handle = word offset): 16-byte entries at byte 0x110 (vertex), 0x510 (fragment) and 0x310 (compute).
// A compute address at c0 0x210..0x30f is uniform block (offset - 0x210) / 16 read as storage: *ubo is set. -1: the
// address is not in c0 (the shader loads it from a user uniform block, not replayed) or the stage has no known table.
// Mapping: MacNativeGames games/totk/tools/totk_xlate/Service.cs:206-222 @ b75be8cf (MIT, Copyright (c) 2026 jhaizhou).
static int ssbo_slot(const GfxShaderRes *r, int st, int *ubo) {
    *ubo = 0;
    if (r->cbuf_slot != 0) return -1;
    int off = r->handle * 4, base = st == NVN_STAGE_VS ? 0x110 : st == NVN_STAGE_FS ? 0x510 : st == NVN_STAGE_CS ? 0x310 : -1;
    if (st == NVN_STAGE_CS && off >= 0x210 && off < 0x310 && (off - 0x210) % 16 == 0) {
        *ubo = 1;
        return (off - 0x210) / 16;
    }
    return base >= 0 && off >= base && off < base + 0x100 && (off - base) % 16 == 0 ? (off - base) / 16 : -1;
}

static VkDescriptorBufferInfo ssbo_info(VkExec *ex, int st, const GfxShaderRes *r, int written) {
    uint64_t max = vk_ctx()->props.limits.maxStorageBufferRange, align = vk_ctx()->props.limits.minStorageBufferOffsetAlignment;
    VkDescriptorBufferInfo bi;
    int ubo, slot = ssbo_slot(r, st, &ubo);
    if (slot >= 0 && slot < 16) {
        const p_bind_buf *b = ubo ? &ex->ubo[st][slot] : &ex->ssbo[st][slot];
        // vk_caps.c: shaders translated for sbuf_align address the buffer from its address rounded down to it.
        uint64_t lead = vk_caps.sbuf_align ? b->addr & (vk_caps.sbuf_align - 1) : 0;
        if (bind_range(ex, b->addr - lead, b->size ? b->size + lead : 0, align, max, ubo ? VK_MUSE_UBO : VK_MUSE_SSBO,
                       written, &bi)) {
            if (written && bi.buffer != VK_NULL_HANDLE) note_written(ex, b->addr - lead, bi.range);
            return bi;
        }
        if (b->addr) warn_once(W_SSBO_ALIGN, "storage buffer %#llx is not backed or not aligned: a zero buffer is bound",
                               (unsigned long long)b->addr);
    } else {
        warn_once(W_SSBO_SLOT, "a storage buffer whose address is at c%d+%#x (stage %d) is not replayed: a zero buffer is bound",
                  r->cbuf_slot, r->handle * 4, st);
    }
    return zero_info(max);
}

// ---- feedback loops (maxwell.md 1.9, vk.h VkCtx.feedback)
// TotK's G-buffer and terrain shaders sample targets of the pass that renders them, at their own pixel, after earlier
// draws of the pass wrote them (albedo, normals, R32F linear depth). Maxwell reads see those writes; in Vulkan the read
// is undefined without a barrier, and on the RX 9070 it showed as dark screen-aligned blocks on the island ground
// (build/v121-sq/SQUARES.md) and hid Tulin's sage spirit. Before a draw whose fragment shader samples an attachment of
// the open pass that a draw or clear of the pass wrote since the pass began or the last barrier:
// - VK_FEEDBACK_LAYOUT: the image is learned (VkTexImage.fb_learned; it has the feedback usage since its creation), the
//   pass restarts once with it in ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT, and from then on its passes attach it in that
//   layout, its descriptors carry that layout, the draws that sample it use a pipeline with the feedback flag
//   (VkPipeState.feedback) and a by-region barrier precedes such a draw.
// - VK_FEEDBACK_RESTART: the pass ends and restarts behind the full barrier (vk_exec_pass_begin).
static int in_fbl(const VkExec *ex, VkImage img) {
    for (int i = 0; i < ex->nfbl; i++)
        if (ex->fbl_img[i] == img) return 1;
    return 0;
}

// Attachment slots of the open pass (bit 8 depth) whose subresource the view of img covers.
static uint32_t attached_as(const VkExec *ex, const VkTexImage *img, const view_t *v, int hv) {
    uint32_t l0 = 0, l1 = img->levels, a0 = 0, a1 = img->layers, m = 0;
    if (hv && v->set_levels) l0 = v->base_level, l1 = v->base_level + (v->nlevels ? v->nlevels : img->levels);
    if (hv && v->set_layers) a0 = v->min_layer, a1 = v->min_layer + (v->nlayers ? v->nlayers : img->layers);
    for (uint32_t i = 0; i < 9; i++) {
        const VkExecTarget *t = i < 8 ? &ex->color[i] : &ex->depth;
        if ((i < 8 && i >= ex->pass_ncolor) || t->image != img->image) continue;
        if (t->level < l0 || t->level >= l1) continue;
        if (!t->is3d && (t->base_layer + t->layers <= a0 || t->base_layer >= a1)) continue;
        m |= 1u << i;
    }
    return m;
}

// Attachments of the open pass the fragment shader fs samples; kept while the shader, the texture bindings and the
// pass stay the same.
static uint32_t feedback_scan(VkExec *ex, const GfxShader *fs) {
    if (ex->fb_fs == fs && ex->fb_serial == ex->tex_serial && ex->fb_pass == ex->pass_serial) return ex->fb_sampled;
    const GfxShaderRes *res;
    int n = gfx_shader_resources(fs, GFX_RES_TEX, &res);
    uint32_t m = 0;
    vk_texture_lock();
    for (int r = 0; r < n; r++)
        for (int k = 0; k < (res[r].count ? res[r].count : 1); k++) {
            int slot = res[r].slot + k;
            uint64_t h = slot >= 0 && slot < 128 ? ex->tex[NVN_STAGE_FS][slot] : 0;
            view_t v;
            int hv;
            const VkTexImage *img = h ? vk_texture_by_id_locked((int)(h & 0xfffff), &v, &hv) : NULL;
            if (img) m |= attached_as(ex, img, &v, hv);
        }
    vk_texture_unlock();
    ex->fb_fs = fs, ex->fb_serial = ex->tex_serial, ex->fb_pass = ex->pass_serial, ex->fb_sampled = m;
    return m;
}

// Before the pipeline lookup of a draw with fragment shader fs: learns and restarts as the mode says. Returns the
// sampled attachments in the feedback layout (the pipeline's feedback flag and feedback_barrier).
static uint32_t feedback_prepare(VkExec *ex, const GfxShader *fs) {
    int mode = vk_ctx()->feedback;
    if (mode == VK_FEEDBACK_OFF || !fs || !ex->in_pass) return 0;
    uint32_t m = feedback_scan(ex, fs);
    uint32_t d = m & ex->fb_dirty;
    if (d) vk_exec_frame.fb_draws++;
    if (d & ~(mode == VK_FEEDBACK_LAYOUT ? ex->fbl_mask : 0)) {
        if (mode == VK_FEEDBACK_LAYOUT)
            for (uint32_t i = 0; i < 9; i++) {
                const VkExecTarget *t = i < 8 ? &ex->color[i] : &ex->depth;
                if ((d >> i & 1) && t->img && t->img->fb_usage) ((VkTexImage *)t->img)->fb_learned = 1;
            }
        vk_exec_pass_end(ex);
        vk_exec_pass_begin(ex);   // learned targets now in the feedback layout; the full barrier cleared fb_dirty
        vk_exec_frame.fb_restarts++;
    }
    return mode == VK_FEEDBACK_LAYOUT ? m & ex->fbl_mask : 0;
}

// By-region barrier inside the pass: earlier attachment writes become visible to this draw's fragment shader reads.
static void feedback_barrier(VkExec *ex) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                                           VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                           .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                           .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
                           .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(ex->cb, &di);
    ex->recorded++;
    ex->fb_dirty = 0;
    vk_exec_frame.fb_barriers++;
}

// Marks the attachments the draw with state s writes: colour targets with a nonzero effective write mask, depth with
// depth writes or the stencil test on.
static void feedback_written(VkExec *ex, const VkPipeState *s) {
    if (vk_ctx()->feedback == VK_FEEDBACK_OFF) return;
    if (ex->depth.image && !(ex->fb_dirty & 0x100) && ((s->ds.dt && s->ds.dw) || s->ds.st)) ex->fb_dirty |= 0x100;
    for (uint32_t i = 0; i < ex->pass_ncolor; i++)
        if (ex->color[i].image && !(ex->fb_dirty >> i & 1) && vk_pipeline_target_wmask(s, (int)i)) ex->fb_dirty |= 1u << i;
}

static void tex_info(VkExec *ex, const GfxShaderRes *r, uint64_t h, int storage, int separate, VkDescriptorImageInfo *out) {
    int ms = (r->type & 0x200) != 0;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler samp = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
    if (h) {
        view_t v;
        int hv;
        const VkTexImage *img = vk_texture_by_id_locked((int)(h & 0xfffff), &v, &hv);
        if (img) view = get_view(ex, img, &v, hv, r->type, storage);
        if (img && storage) vk_texture_storage_bound_locked(img);
        if (img && !storage && ex->nfbl && in_fbl(ex, img->image)) layout = VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT;
        if (!storage && !separate) samp = vk_sampler_by_id_locked((int)(h >> 20 & 0xfff));
    }
    if (!view) {
        int di = dummy_index(res_view_type(r->type), ms);
        view = storage ? storage_dummy(di, r->format) : Z.img[di].view;
    }
    if (!samp) samp = r->type & 0x400 ? Z.samp_shadow : Z.samp;
    *out = (VkDescriptorImageInfo){separate || storage ? VK_NULL_HANDLE : samp, view, layout};
}

static VkDescriptorSet alloc_set(VkExec *ex, VkDescriptorSetLayout layout) {
    CbRes *r = cb_res(ex);
    VkCtx *c = vk_ctx();
    for (int attempt = 0; attempt < 2; attempt++) {
        if (r->npools) {
            VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                              .descriptorPool = r->pools[r->npools - 1], .descriptorSetCount = 1,
                                              .pSetLayouts = &layout};
            VkDescriptorSet set;
            VkResult res = vkAllocateDescriptorSets(c->device, &ai, &set);
            if (res == VK_SUCCESS) return set;
            if (res != VK_ERROR_OUT_OF_POOL_MEMORY && res != VK_ERROR_FRAGMENTED_POOL) rt_fatal("vk: descriptor set allocation failed (%d)", (int)res);
        }
        static const VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4096}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2048},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1024}, {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 256},
            {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 256}};
        VkDescriptorPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 256,
                                         .poolSizeCount = sizeof sizes / sizeof sizes[0], .pPoolSizes = sizes};
        if (r->npools == r->cpools) r->pools = grow(r->pools, &r->cpools, sizeof *r->pools);
        VkDescriptorPool pool = VK_NULL_HANDLE;
        AcquireSRWLockExclusive(&spare_lock);
        if (nspare_pool) pool = spare_pool[--nspare_pool];
        ReleaseSRWLockExclusive(&spare_lock);
        if (!pool && vkCreateDescriptorPool(c->device, &pi, NULL, &pool) != VK_SUCCESS) rt_fatal("vk: descriptor pool failed");
        r->pools[r->npools++] = pool;
    }
    rt_fatal("vk: descriptor set allocation failed twice");
    return VK_NULL_HANDLE;
}

static void support_in(const VkExec *ex, float in[3]) {   // what support_fill reads: viewport size, point size
    in[0] = ex->in_pass && ex->has_vp ? ex->vp[0][2] : (float)ex->pass_w;
    in[1] = ex->in_pass && ex->has_vp ? ex->vp[0][3] : (float)ex->pass_h;
    in[2] = ex->point_size > 0 ? ex->point_size : 1.0f;
}

static void support_fill(uint8_t *b, float w, float h, float point_size) {
    memset(b, 0, SUPPORT_BYTES);
    float inv[4] = {w > 0 ? 2.0f / w : 0, h > 0 ? 2.0f / h : 0, 0, 0}, size[4] = {w, h, 0, 0};
    memcpy(b + 144, inv, 16);
    memcpy(b + 160, size, 16);
    const float one = 1.0f;
    memcpy(b + 1392, &point_size, 4);   // point_size (translator patch 0008): vertex shaders that write none output it
    for (int i = 0; i < 73; i++) memcpy(b + 192 + 16 * i, &one, 4);   // render scale 1: the host renders at guest size
}

// TOTK_BIND_STATS=1: bindings and descriptors per draw of each stage (Task 2.2's choice of the pushed stage), and how
// often a draw repeats the previous one, logged every 20000 draws: pushes recorded and skipped, allocated sets written
// and reused, stage texture-type recomputations (answers_for), draws with the previous draw's program, bound state
// and textures (shaders and pipeline would be the same) and those that also have its buffers and viewport (all
// descriptors would be the same). BS is declared beside DS.
static int bind_stats_on(void) {
    if (BS.on < 0) {
        const char *e = cfg_get("TOTK_BIND_STATS");
        BS.on = e && *e && strcmp(e, "0");
    }
    return BS.on;
}

static void bind_stats(const VkPipeInfo *info) {
    if (!bind_stats_on() || info->bind_point != VK_PIPELINE_BIND_POINT_GRAPHICS) return;
    for (uint32_t i = 0; i < info->n_bindings; i++) {
        const VkPipeBinding *b = &info->bindings[i];
        int fs = info->n_sets > 1 ? b->set : !(b->stages & VK_SHADER_STAGE_VERTEX_BIT);
        BS.bind[fs]++, BS.desc[fs] += b->count ? b->count : 1;
    }
    if (++BS.draws % 20000 == 0) {
        double d = (double)BS.draws;
        rt_log("vk_exec: per draw over %llu draws: vertex %.2f bindings %.2f descriptors, fragment %.2f bindings %.2f descriptors",
               (unsigned long long)BS.draws, BS.bind[0] / d, BS.desc[0] / d, BS.bind[1] / d, BS.desc[1] / d);
        rt_log("vk_exec: per draw: pushes %.3f skipped %.3f, sets written %.3f reused %.3f, texture types %.3f; "
               "same program and state as the draw before %.3f, also same buffers %.3f; descriptor builds skipped %.3f",
               BS.push / d, BS.push_skip / d, BS.set_new / d, BS.set_reuse / d, BS.ac_miss / d, BS.same_prog / d,
               BS.same_all / d, BS.bind_skip / d);
    }
}

// The bound state a draw reads, compared with the previous draw's (TOTK_BIND_STATS only).
static void repeat_stats(const VkExec *ex, const VkPipeInfo *info) {
    static struct {
        const prog_t *prog;
        VkPipeState pipe;
        uint64_t tex[2][128], img[2][32];
        const VkPipeInfo *info;
        p_bind_buf ubo[2][18], ssbo[2][16];
        float vp[6], point_size;
    } last, cur;
    if (!bind_stats_on()) return;
    memset(&cur, 0, sizeof cur);
    cur.prog = ex->prog, cur.pipe = ex->pipe, cur.info = info;
    const int st[2] = {NVN_STAGE_VS, NVN_STAGE_FS};
    for (int k = 0; k < 2; k++) {
        memcpy(cur.tex[k], ex->tex[st[k]], sizeof cur.tex[k]), memcpy(cur.img[k], ex->img[st[k]], sizeof cur.img[k]);
        memcpy(cur.ubo[k], ex->ubo[st[k]], sizeof cur.ubo[k]), memcpy(cur.ssbo[k], ex->ssbo[st[k]], sizeof cur.ssbo[k]);
    }
    memcpy(cur.vp, ex->vp[0], sizeof cur.vp), cur.point_size = ex->point_size;
    size_t prog_part = offsetof(typeof(cur), info);
    if (!memcmp(&cur, &last, prog_part)) {
        BS.same_prog++;
        if (!memcmp((const char *)&cur + prog_part, (const char *)&last + prog_part, sizeof cur - prog_part)) BS.same_all++;
    }
    last = cur;
}

static int push_skip_on(void) {   // TOTK_PUSH_SKIP=0: every graphics draw pushes its set again
    static int on = -1;
    if (on < 0) {
        const char *e = cfg_get("TOTK_PUSH_SKIP");
        on = !(e && !strcmp(e, "0"));
    }
    return on;
}

// A graphics draw whose descriptor inputs equal the previous graphics draw's in this command buffer skips building its
// sets: every write would be the same, the push would be skipped and the allocated set reused without a bind (about a
// quarter of field and monsters2 draws, TOTK_BIND_STATS). The inputs, compared and stored in place, are what
// bind_resources and its helpers read beyond the pipeline info's fixed bindings and Z's stand-ins:
// - the info (bindings, layouts), the program and its serial (the compiler-constant range of UBO slot 1);
// - vk_texture_slots_gen, vk_texture_epoch (a slot's image, view and once-per-submit storage check), vk_sampler_gen;
// - vk_mem_gen (buffer and offset of a span, mirror creation and per-submission sync);
// - the images in the feedback layout (the sampled layout);
// - per binding: the support inputs, c0's storage table and base instance, the bound uniform and storage ranges, the
//   texture and image handles (RsList; compared only when a bind op ran since the last draw, bind_gen).
// The side effects of a build repeat as no-ops in the same command buffer and submission: c0 and support buffers kept
// in B, note_written's ranges, vk_mem_gpu_wrote's write-backs, the per-submission mirror sync and dirty census note,
// the views cached per submission. The census counts are added again. A build that copied guest bytes into transient
// memory (an unaligned host range) is never skipped after: the copy may differ. B.res_valid says the sets bound hold
// the last build; vk_exec_draw_new_cb's memset clears it, and every build clears it until it ends.
// TOTK_BIND_SKIP=0 builds every draw; =check builds every draw and stops when a draw with equal inputs gets other
// writes, census counts or descriptor commands.
static int bind_skip_on(void) {
    if (RS.on < 0) {
        const char *e = cfg_get("TOTK_BIND_SKIP");
        RS.on = e && !strcmp(e, "check") ? 2 : !(e && !strcmp(e, "0"));
        if (RS.on != 1) rt_log("vk_exec: descriptor build skip %s", RS.on ? "check" : "off");
    }
    return RS.on;
}

static void rs_add(RsRange *r, uint32_t *n, const VkExec *ex, const void *at, size_t len) {
    uint32_t off = (uint32_t)((const uint8_t *)at - (const uint8_t *)ex);
    if (*n && r[*n - 1].off + r[*n - 1].len == off) r[*n - 1].len += (uint32_t)len;
    else r[(*n)++] = (RsRange){off, (uint32_t)len};
}

static const RsList *rs_list(const VkExec *ex, const VkPipeInfo *info) {
    if (info == rs_last) return rs_last_l;
    if (!rs_lists.klen) nvn_hm_init(&rs_lists, sizeof info);
    RsList *l = nvn_hm_get(&rs_lists, &info);
    if (!l) {
        static RsRange tmp[MAX_DESC];
        uint32_t n = 0, nd = 0;
        int sup = 0, c0 = 0, ok = 1;
        for (uint32_t i = 0; i < info->n_bindings && ok; i++) {
            const VkPipeBinding *b = &info->bindings[i];
            const GfxShaderRes *r = b->res;
            uint32_t cnt = b->count ? b->count : 1;
            if ((nd += cnt) > MAX_DESC) { ok = 0; break; }   // the build stops on it
            int st = nvn_stage_of(b->stage);
            if (b->kind == VK_PIPE_SUPPORT) {
                sup = 1;
            } else if (b->kind == GFX_RES_UBO) {
                for (uint32_t k = 0; k < cnt; k++) {
                    int slot = r->slot + (int)k;
                    if (slot == 0 && c0_on()) c0 = 1, rs_add(tmp, &n, ex, ex->ssbo[st], sizeof ex->ssbo[st]);
                    else if (slot >= 3 && slot - 3 < 18) rs_add(tmp, &n, ex, &ex->ubo[st][slot - 3], sizeof(p_bind_buf));
                }   // slot 1: the program (bind_sig); others: the zero buffer
            } else if (b->kind == GFX_RES_SSBO) {   // every descriptor of the binding is the same range
                int ubo, slot = ssbo_slot(r, st, &ubo);
                if (slot >= 0 && slot < 16) rs_add(tmp, &n, ex, ubo ? &ex->ubo[st][slot] : &ex->ssbo[st][slot], sizeof(p_bind_buf));
            } else if ((b->kind == GFX_RES_TEX || b->kind == GFX_RES_IMG) && b->type != VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER &&
                       b->type != VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
                int img = b->kind == GFX_RES_IMG, cap = img ? 32 : 128;
                for (uint32_t k = 0; k < cnt; k++) {
                    int slot = r->slot + (int)k;
                    if (slot >= 0 && slot < cap) rs_add(tmp, &n, ex, img ? &ex->img[st][slot] : &ex->tex[st][slot], 8);
                }
            }
        }
        uint64_t bytes = 0;
        for (uint32_t i = 0; i < n; i++) bytes += tmp[i].len;
        if (bytes > RS_BYTES) ok = 0;
        l = malloc(sizeof *l + sizeof *tmp * n);
        if (!l) rt_fatal("vk: out of memory");
        l->n = n, l->bytes = (uint32_t)bytes, l->sup = (uint8_t)sup, l->c0 = (uint8_t)c0, l->ok = (uint8_t)ok;
        memcpy(l->r, tmp, sizeof *tmp * n);
        nvn_hm_put(&rs_lists, &info, l);
    }
    rs_last = info, rs_last_l = l;
    return l;
}

// The signature of a graphics draw's descriptor inputs into RS; 1 when it equals the stored one. The bound-slot bytes
// are compared only when a bind op ran since they were stored or the pipeline changed: otherwise they are the same.
// Check mode compares them anyway and stops when the bind op generation said they could not differ but they do.
static int bind_sig(const VkExec *ex, const VkPipeInfo *info) {
    const RsList *l = rs_list(ex, info);
    if (!l->ok) return RS.hn = 0, RS.info = NULL, 0;
    uint64_t *h = RS.h, diff = 0;
    uint32_t n = 0;
#define PUT(v) do { uint64_t v_ = (uint64_t)(v); diff |= h[n] ^ v_; h[n++] = v_; } while (0)
    PUT((uintptr_t)info);
    PUT((uintptr_t)ex->prog);
    PUT(ex->prog->serial);
    PUT((uint64_t)vk_texture_slots_gen() << 32 | vk_texture_epoch());
    PUT((uint64_t)vk_sampler_gen() << 32 | (uint32_t)ex->nfbl);
    PUT(vk_mem_gen());
    for (int i = 0; i < ex->nfbl && i < 9; i++) PUT((uintptr_t)ex->fbl_img[i]);
    if (l->sup) {
        float in[3];
        support_in(ex, in);
        uint64_t a;
        uint32_t ps;
        memcpy(&a, in, 8), memcpy(&ps, &in[2], 4);
        PUT(a);
        PUT(ps);
    }
    if (l->c0) PUT(c0_binst);
#undef PUT
    diff |= n != RS.hn;
    RS.hn = n;
    int fast = RS.info == info && RS.gen == bind_gen;
    if (fast && RS.on != 2) return !diff;
    const uint8_t *e = (const uint8_t *)ex;
    uint8_t *d = RS.body;
    int bd = 0;
    for (uint32_t i = 0; i < l->n; i++) {
        const RsRange *r = &l->r[i];
        if (!bd && memcmp(d, e + r->off, r->len)) bd = 1;
        if (bd) memcpy(d, e + r->off, r->len);
        d += r->len;
    }
    if (fast && bd) rt_fatal("vk_exec: descriptor build skip check: bound slots changed without a bind op");
    RS.info = info, RS.gen = bind_gen;
    return !diff && !bd;
}

// Writes allocated set s from its packed descriptors d: through the layout's update template, or with the write
// structs w[i0..i1) when it has none.
static void write_set(const VkPipeInfo *info, uint32_t s, VkDescriptorSet set, const VkDescSlot *d, VkWriteDescriptorSet *w,
                      uint32_t i0, uint32_t i1) {
    if (i1 == i0) return;
    if (info->tpl[s]) {
        vk_pipeline_update_tpl_fn()(vk_ctx()->device, set, info->tpl[s], d);
        return;
    }
    for (uint32_t k = i0; k < i1; k++) w[k].dstSet = set;
    vkUpdateDescriptorSets(vk_ctx()->device, i1 - i0, w + i0, 0, NULL);
}

// Writes the pipeline's sets from the bound state: set 0, and set 1 with independent layouts. Each is pushed into the
// command buffer, or allocated, written and bound; a graphics set whose contents equal the last one allocated for that
// set number in this command buffer is reused, and bound again only when another layout or a push came between. A
// graphics push equal to the last one at its set number, with no other layout or bind between, is not recorded.
// The descriptors go into one packed array d, a VkDescSlot per descriptor in binding order, which a set's update
// template reads directly (VkPipeInfo.tpl); write structs are filled only when a set has no template.
static void bind_resources(VkExec *ex, const VkPipeInfo *info) {
    static VkWriteDescriptorSet w[MAX_BINDINGS];
    static VkDescSlot d[MAX_DESC];
    static VkBufferView bv[MAX_DESC];
    if (info->n_bindings > MAX_BINDINGS) rt_fatal("vk: %u descriptor bindings exceed the replay's %d", info->n_bindings, MAX_BINDINGS);
    bind_stats(info);
    int gfx = info->bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS;
    int skip = gfx ? bind_skip_on() : 0, same = 0, was_valid = B.res_valid;
    if (skip) {
        same = bind_sig(ex, info);
        if (same && was_valid && skip == 1) {
            for (int i = 0; i < RS.ncnt; i++) vk_mem_count(RS.cnt[i].cls, RS.cnt[i].use, RS.cnt[i].bytes);
            BS.bind_skip++;
            ex->recorded++;
            return;
        }
        B.res_valid = 0;   // until the build below ends
        if (skip == 2) memcpy(RS.pcnt, RS.cnt, sizeof RS.cnt), RS.pncnt = RS.ncnt;
        RS.rec = 1, RS.ncnt = 0, RS.cnt_over = 0, RS.volatile_ = 0, RS.cmds = 0;
    }
    uint32_t n_sets = info->n_sets ? info->n_sets : 1;
    uint32_t lo[2] = {0, 0}, hi[2] = {0, 0}, d0[2] = {0, 0};   // bindings [lo, hi) and first descriptor per set
    int need_w = 0;                                             // a set without a template: fill the write structs
    for (uint32_t s = 0, i0 = 0; s < n_sets; s++) {
        uint32_t i1 = i0;
        while (i1 < info->n_bindings && (n_sets == 1 || info->bindings[i1].set == s)) i1++;
        lo[s] = i0, hi[s] = i1, i0 = i1;
        if (i1 > lo[s] && !info->tpl[s]) need_w = 1;
    }
    uint32_t nd = 0;
    for (uint32_t i = 0; i < info->n_bindings; i++) {
        const VkPipeBinding *b = &info->bindings[i];
        const GfxShaderRes *r = b->res;
        uint32_t cnt = b->count ? b->count : 1;
        if (nd + cnt > MAX_DESC) rt_fatal("vk: %u descriptors exceed the replay's %d", nd + cnt, MAX_DESC);
        if (n_sets == 2 && i == lo[1]) d0[1] = nd;
        VkDescSlot *o = &d[nd];
        int st = nvn_stage_of(b->stage), kind = 0;   // kind: 0 buffer, 1 image, 2 texel buffer view
        if (b->kind == VK_PIPE_SUPPORT) {
            float in[3];
            support_in(ex, in);
            if (!B.sup_valid || memcmp(in, B.sup_in, sizeof in)) {   // support_fill reads only these
                uint8_t tmp[SUPPORT_BYTES];
                support_fill(tmp, in[0], in[1], in[2]);
                memcpy(B.sup_in, in, sizeof in);
                if (!B.sup_valid || memcmp(tmp, B.support, SUPPORT_BYTES)) {   // a new buffer only when the contents changed
                    VkBuffer sb;
                    VkDeviceSize so;
                    memcpy(scratch(ex, SUPPORT_BYTES, &sb, &so), tmp, SUPPORT_BYTES);
                    memcpy(B.support, tmp, SUPPORT_BYTES);
                    B.sup_buf = sb, B.sup_off = so, B.sup_valid = 1;
                }
            }
            o->buffer = (VkDescriptorBufferInfo){B.sup_buf, B.sup_off, SUPPORT_BYTES};
        } else if (b->kind == GFX_RES_UBO) {
            for (uint32_t k = 0; k < cnt; k++) o[k].buffer = ubo_info(ex, st, r->slot + (int)k);
        } else if (b->kind == GFX_RES_SSBO) {
            for (uint32_t k = 0; k < cnt; k++) o[k].buffer = ssbo_info(ex, st, r, r->flags & 1);
        } else if (b->kind == GFX_RES_TEX || b->kind == GFX_RES_IMG) {
            int img = b->kind == GFX_RES_IMG;
            if (b->type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || b->type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
                kind = 2;
                for (uint32_t k = 0; k < cnt; k++) {   // buffer textures are not replayed yet
                    o[k].w[0] = 0, o[k].w[1] = 0, o[k].w[2] = 0;
                    o[k].view = Z.zview;
                    if (need_w) bv[nd + k] = Z.zview;
                }
            } else {
                kind = 1;
                vk_texture_lock();   // tex_info looks up with the texture lock held
                for (uint32_t k = 0; k < cnt; k++) {
                    int slot = r->slot + (int)k;
                    uint64_t h = 0;
                    if (img) h = slot >= 0 && slot < 32 ? ex->img[st][slot] : 0;
                    else h = slot >= 0 && slot < 128 ? ex->tex[st][slot] : 0;
                    tex_info(ex, r, h, img, r->separate, &o[k].image);
                    o[k].w[2] = (uint32_t)o[k].image.imageLayout;   // zero the tail padding
                }
                vk_texture_unlock();
            }
        } else {
            rt_fatal("vk: binding %u has an unknown kind %d", (unsigned)b->binding, (int)b->kind);
        }
        if (need_w) {
            w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                          .dstBinding = b->binding, .descriptorCount = cnt, .descriptorType = b->type};
            if (kind == 0) w[i].pBufferInfo = &o->buffer;   // a VkDescSlot is as large as either info struct
            else if (kind == 1) w[i].pImageInfo = &o->image;
            else w[i].pTexelBufferView = &bv[nd];
        }
        nd += cnt;
    }
    // Under the validation layer, a new graphics layout binds set 1 before set 0 is pushed or bound. The layer (SDK
    // 1.4.363, PipelineLayoutCompatDef::operator==) takes any two INDEPENDENT_SETS layouts as compatible, so
    // vkCmdPushDescriptorSetKHR keeps the push set it made for an earlier layout and drops writes to bindings that
    // layout lacks (false VUID-vkCmdDrawIndexed-None-08114, then an access violation inside the layer). A bind at set 1
    // disturbs set 0 by its canonical id, which drops the layer's push set: the push after it gets a new one.
    int set1_first = 0;
    if (gfx && info->layout != B.ds_layout) {   // another layout: sets bound before may be disturbed
        B.ds_layout = info->layout;
        B.ds_bound[0] = B.ds_bound[1] = VK_NULL_HANDLE;
        B.ds_pushed[0] = B.ds_pushed[1] = 0;
        set1_first = n_sets == 2 && vk_ctx()->validation;
    }
    for (uint32_t k = 0; k < n_sets; k++) {
        uint32_t s = set1_first ? 1 - k : k, i0 = lo[s], i1 = hi[s];
        const VkDescSlot *ds = &d[d0[s]];
        uint32_t dbytes = (uint32_t)(((s + 1 < n_sets ? d0[s + 1] : nd) - d0[s]) * sizeof *d);
        if (s && i1 == i0) {               // no fragment stage: set 1 is empty and unused
            if (set1_first) {              // bound only to drop the layer's push set (comment above)
                VkDescriptorSet set = alloc_set(ex, info->set_layout1);
                vkCmdBindDescriptorSets(ex->cb, info->bind_point, info->layout, 1, 1, &set, 0, NULL);
                B.ds_bound[1] = set, B.ds_pushed[1] = 0;
                RS.cmds++;
            }
            continue;
        }
        if (s ? info->push1 : info->push) {
            uint32_t n = gfx && push_skip_on() ? dbytes : 0;
            if (n && B.ds_pushed[s] && DS.pn[s] == n && !memcmp(DS.psig[s], ds, n)) {
                BS.push_skip++;
                continue;
            }
            if (info->tpl[s]) vk_pipeline_push_tpl_fn()(ex->cb, info->tpl[s], info->layout, s, ds);
            else vk_pipeline_push_fn()(ex->cb, info->bind_point, info->layout, s, i1 - i0, w + i0);
            BS.push++;
            RS.cmds++;
            if (gfx) {
                B.ds_bound[s] = VK_NULL_HANDLE, B.ds_pushed[s] = n != 0;
                if (n) DS.pn[s] = n, memcpy(DS.psig[s], ds, n);
            }
        } else if (!gfx || s > 1) {
            VkDescriptorSet set = alloc_set(ex, s ? info->set_layout1 : info->set_layout);
            write_set(info, s, set, ds, w, i0, i1);
            vkCmdBindDescriptorSets(ex->cb, info->bind_point, info->layout, s, 1, &set, 0, NULL);
        } else {
            VkDescriptorSetLayout sl = s ? info->set_layout1 : info->set_layout;
            VkDescriptorSet set = B.ds_last[s];
            if (!set || B.ds_last_layout[s] != sl || DS.n[s] != dbytes || memcmp(DS.sig[s], ds, dbytes)) {
                set = alloc_set(ex, sl);
                write_set(info, s, set, ds, w, i0, i1);
                B.ds_last[s] = set, B.ds_last_layout[s] = sl, DS.n[s] = dbytes;
                memcpy(DS.sig[s], ds, dbytes);
                BS.set_new++;
                RS.cmds++;
            } else {
                BS.set_reuse++;
            }
            if (B.ds_bound[s] != set) {
                vkCmdBindDescriptorSets(ex->cb, info->bind_point, info->layout, s, 1, &set, 0, NULL);
                B.ds_bound[s] = set, B.ds_pushed[s] = 0;
                RS.cmds++;
            }
        }
    }
    if (skip) {
        RS.rec = 0;
        // An equal next draw records nothing when every push set holds its push (TOTK_PUSH_SKIP on) and nothing made
        // the build unrepeatable; allocated sets are always bound to the one written last.
        int valid = !RS.volatile_ && !RS.cnt_over && RS.hn;
        for (uint32_t s = 0; s < n_sets; s++)
            if (hi[s] > lo[s] && (s ? info->push1 : info->push) && !B.ds_pushed[s]) valid = 0;
        if (skip == 2) {   // with the pipeline info equal (bind_sig), the packed descriptors determine every write
            uint32_t n = (uint32_t)(nd * sizeof *d);
            int wr = n != RS.ckn || memcmp(RS.ck, d, n);
            int cn = RS.ncnt != RS.pncnt || memcmp(RS.cnt, RS.pcnt, sizeof RS.cnt[0] * (size_t)RS.ncnt);
            if (same && was_valid && (wr || cn || RS.cmds))
                rt_fatal("vk_exec: descriptor build skip check: a draw of program %llu with the inputs of the previous one "
                         "gets %s", (unsigned long long)ex->prog->serial,
                         RS.cmds ? "descriptor commands" : wr ? "other writes" : "other census counts");
            if (same && was_valid) BS.bind_skip++;   // the draws a skip would have spared, compared instead
            RS.ckn = n;
            memcpy(RS.ck, d, n);
        }
        B.res_valid = valid;
    }
    ex->recorded++;
}

// ---- draws

static void bind_pipeline(VkExec *ex, VkPipelineBindPoint bp, VkPipeline p) {
    int k = bp == VK_PIPELINE_BIND_POINT_COMPUTE;
    if (B.pipe[k] == p) return;
    vkCmdBindPipeline(ex->cb, bp, p);
    ex->recorded++;
    B.pipe[k] = p;
    if (!k) B.st_valid = 0;
}

static void record_fixed(VkExec *ex) {
    VkCommandBuffer cb = ex->cb;
    if (ex->dirty & VK_EXEC_DIRTY_VIEWPORT) {
        float x = 0, y = 0, w = (float)ex->pass_w, h = (float)ex->pass_h;
        if (ex->has_vp) x = ex->vp[0][0], y = ex->vp[0][1], w = ex->vp[0][2], h = ex->vp[0][3];
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        VkViewport v = {x, y + h, w, -h, ex->depth_range[0], ex->depth_range[1]};   // the y flip: negative height
        vkCmdSetViewportWithCount(cb, 1, &v);
        ex->recorded++;
    }
    if (ex->dirty & VK_EXEC_DIRTY_SCISSOR) {
        VkRect2D r = vk_exec_scissor(ex);
        vkCmdSetScissorWithCount(cb, 1, &r);
        ex->recorded++;
    }
    if (ex->dirty & VK_EXEC_DIRTY_DEPTH_BIAS) {
        vkCmdSetDepthBias(cb, ex->poly_offset.units, ex->poly_offset.clamp, ex->poly_offset.factor);
        ex->recorded++;
    }
    if (ex->dirty & VK_EXEC_DIRTY_BLEND_CONST) {
        vkCmdSetBlendConstants(cb, ex->blend_color);
        ex->recorded++;
    }
    if (ex->dirty & VK_EXEC_DIRTY_STENCIL) {
        for (int f = 0; f < 2; f++) {
            VkStencilFaceFlags face = f ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
            vkCmdSetStencilCompareMask(cb, face, ex->st_vmask[f]);
            vkCmdSetStencilWriteMask(cb, face, ex->st_wmask[f]);
            vkCmdSetStencilReference(cb, face, ex->st_ref[f]);
            ex->recorded += 3;
        }
    }
    ex->dirty = 0;
}

static void bind_vertex_buffers(VkExec *ex) {
    VkBuffer vb[17];
    VkDeviceSize vo[17], vsz[17], vstr[17];
    int vcls[17] = {0};
    void *vpool[17] = {0};
    int dyn = vk_pipeline_vertex_input_dynamic();
    for (int b = 0; b < 17; b++) {
        vb[b] = Z.zbuf, vo[b] = 0, vsz[b] = ZERO_BYTES, vstr[b] = b < 16 ? ex->pipe.vstream[b].stride : 0;
        if (b == VK_PIPE_ZERO_BINDING) continue;
        const NVNbufferRange *r = &ex->vbuf[b];
        VkMemSpan sp;
        if (!r->address || !vk_mem_resolve(r->address, 1, &sp)) continue;
        VK_MEM_USE(sp);
        uint64_t n = r->size && r->size < sp.avail ? r->size : sp.avail;
        vb[b] = sp.buffer, vo[b] = sp.offset, vsz[b] = n, vcls[b] = sp.cls, vpool[b] = sp.pool;
    }
    if (B.vb_valid && !memcmp(B.vb, vb, sizeof vb) && !memcmp(B.vo, vo, sizeof vo) && !memcmp(B.vsz, vsz, sizeof vsz) &&
        (dyn || !memcmp(B.vstr, vstr, sizeof vstr)))
        return;
    vkCmdBindVertexBuffers2(ex->cb, 0, 17, vb, vo, vsz, dyn ? NULL : vstr);
    ex->recorded++;
    for (int b = 0; b < 17; b++)
        if (vcls[b]) vk_mem_count(vcls[b], VK_MUSE_VB, vsz[b]);
    if (rt_trace & RT_TRACE_PERF)
        for (int b = 0; b < 17; b++)
            if (vpool[b]) vk_mem_bound_note(vpool[b]);
    memcpy(B.vb, vb, sizeof vb), memcpy(B.vo, vo, sizeof vo), memcpy(B.vsz, vsz, sizeof vsz), memcpy(B.vstr, vstr, sizeof vstr);
    B.vb_valid = 1;
}

// TOTK_PIPE_SKIP: which draws are skipped, rather than waited for, while their shaders or pipeline build.
// strict (0): only draws whose pass targets were all rendered in the previous presented frame; a pass with a target
//   that was not counts as one-shot and waits.
// any (1, default): also draws in a pass with at least one target rendered in the previous frame (a per-frame depth
//   buffer or g-buffer with a colour target the frame before did not use: half-resolution effect buffers in fights).
//   Passes with none (volumes, LUTs, shadow layers seen for the first time) still wait.
// fullscreen (2): as any, and full-screen draws into such passes are skipped instead of waiting from their budget.
static int pipe_skip_mode(void) {
    static int mode = -1;
    if (mode < 0) {
        const char *e = cfg_get("TOTK_PIPE_SKIP");
        mode = !e || !*e ? 1 : !strcmp(e, "strict") || !strcmp(e, "0") ? 0 : !strcmp(e, "fullscreen") || !strcmp(e, "2") ? 2 : 1;
        static const char *const what[3] = {"only draws into per-frame passes (TOTK_PIPE_SKIP=strict)",
                                            "draws into passes with a target the previous frame rendered",
                                            "those and full-screen draws (TOTK_PIPE_SKIP=fullscreen)"};
        rt_log("vk_exec: draws skipped while their pipeline builds: %s", what[mode]);
    }
    return mode;
}

// The previous draw's shaders, kept while its program, bound pipeline state, the vertex and fragment stages' first 32
// texture and image handles and every slot's resolution (vk_texture_slots_gen, read unlocked: a registration racing
// with it is ordered after this draw, as answers_for's own check would order it) stay the same: answers_for reads
// nothing else, so shader_for would return the same shaders. About 65% of field draws repeat them
// (TOTK_BIND_STATS). TOTK_SHADER_MEMO=0 turns it off; =check looks the shaders up on every hit too, and every slot's
// texture type in answers_for, and stops on a difference.
typedef struct {
    const prog_t *prog;
    uint64_t serial;
    uint32_t gen;
    uint64_t tex[2][32], img[2][32];
    VkPipeState pipe;
} MemoKey;
static struct { int on, valid, has_fs; MemoKey k; const GfxShader *vs, *fs; } SM = {-1};

static void memo_key(const VkExec *ex, uint32_t gen, MemoKey *k) {
    k->prog = ex->prog, k->serial = ex->prog->serial, k->gen = gen;
    memcpy(k->tex[0], ex->tex[NVN_STAGE_VS], sizeof k->tex[0]), memcpy(k->tex[1], ex->tex[NVN_STAGE_FS], sizeof k->tex[1]);
    memcpy(k->img[0], ex->img[NVN_STAGE_VS], sizeof k->img[0]), memcpy(k->img[1], ex->img[NVN_STAGE_FS], sizeof k->img[1]);
    memcpy(&k->pipe, &ex->pipe, sizeof k->pipe);
}

static int memo_match(const VkExec *ex, uint32_t gen, const MemoKey *k) {
    return k->prog == ex->prog && k->serial == ex->prog->serial && k->gen == gen &&
           !memcmp(k->tex[0], ex->tex[NVN_STAGE_VS], sizeof k->tex[0]) && !memcmp(k->tex[1], ex->tex[NVN_STAGE_FS], sizeof k->tex[1]) &&
           !memcmp(k->img[0], ex->img[NVN_STAGE_VS], sizeof k->img[0]) && !memcmp(k->img[1], ex->img[NVN_STAGE_FS], sizeof k->img[1]) &&
           !memcmp(&k->pipe, &ex->pipe, sizeof k->pipe);
}

static int shader_memo_on(void) {
    if (SM.on < 0) {
        const char *e = cfg_get("TOTK_SHADER_MEMO");
        SM.on = e && !strcmp(e, "check") ? 2 : !(e && !strcmp(e, "0"));
    }
    return SM.on;
}

// Pass, shaders, pipeline, fixed state and resources for a draw. 0: the draw is skipped.
static int prepare_draw(VkExec *ex, int prim, int32_t count, int restart) {
    if (ex->render_disabled) return 0;
    cb_res(ex);
    dummies_init();
    if (!ex->in_pass) ex->dirty |= VK_EXEC_DIRTY_VIEWPORT | VK_EXEC_DIRTY_SCISSOR;
    vk_exec_pass_begin(ex);
    VkRect2D sc = vk_exec_scissor(ex);
    if (!sc.extent.width || !sc.extent.height) return 0;
    if (!ex->prog) return warn_once(W_PROGRAM, "a draw without a bound program is skipped"), 0;
    int has_vs, has_fs, vs_wait = 0, fs_wait = 0, memo = shader_memo_on();
    const GfxShader *vs, *fs;
    uint32_t gen = memo ? vk_texture_slots_gen() : 0;   // before shader_for, whose lookups may bump it
    if (memo && SM.valid && memo_match(ex, gen, &SM.k)) {
        vs = SM.vs, fs = SM.fs, has_vs = 1, has_fs = SM.has_fs;
        if (memo == 2) {
            int hv, hf, wv, wf;
            const GfxShader *v2 = shader_for(ex, NVN_STAGE_VS, &hv, &wv, 0), *f2 = shader_for(ex, NVN_STAGE_FS, &hf, &wf, 0);
            if (v2 != vs || f2 != fs || hf != has_fs || wv || wf)
                rt_fatal("vk_exec: shader memo of program %llu differs from the lookup", (unsigned long long)ex->prog->serial);
        }
    } else {
        SM.valid = 0;
        vs = shader_for(ex, NVN_STAGE_VS, &has_vs, &vs_wait, 0);
        fs = shader_for(ex, NVN_STAGE_FS, &has_fs, &fs_wait, 0);
        if (memo && vs && !vs_wait && !fs_wait && (!has_fs || fs))
            memo_key(ex, gen, &SM.k), SM.vs = vs, SM.fs = fs, SM.has_fs = has_fs, SM.valid = 1;
    }
    if (!has_vs) return warn_once(W_NO_VS, "a draw with a program that has no vertex stage is skipped"), 0;
    // A draw into a target the previous frame did not render (or into none) waits for a pending shader or pipeline:
    // one-shot draws (volumes and LUTs generated once, later only sampled) would leave their target empty for the
    // session. A draw into per-frame targets is skipped until they are ready and comes back next frame.
    int oneshot = -1, skip = pipe_skip_mode();   // -1: not looked up
    if (vs_wait || fs_wait) {
        if (!(oneshot = !vk_exec_targets_recent(ex, skip >= 1))) return perf_count(PERF_CNT_SHADER_SKIP), 0;   // skipped until ready
        perf_count(PERF_CNT_ONESHOT_WAIT);
        if (vs_wait) vs = shader_for(ex, NVN_STAGE_VS, &has_vs, NULL, 1);
        if (fs_wait) fs = shader_for(ex, NVN_STAGE_FS, &has_fs, NULL, 1);
    }
    if (!vs || (has_fs && !fs)) return 0;
    uint32_t fbl = feedback_prepare(ex, fs);
    VkPipeState s = ex->pipe;
    s.vs = vs, s.fs = fs;
    s.feedback = (uint8_t)((fbl & 0xff ? 1 : 0) | (fbl & 0x100 ? 2 : 0));
    s.prim = (uint8_t)prim;
    s.depth_minus_w = (uint8_t)(nvn_depth_minus_w() != 0);
    s.y_flip = 1;
    s.prim_restart = (uint8_t)(restart != 0);
    const VkPipeInfo *info = NULL;
    int pending;
    // A draw of at most 6 vertices over half the render area or more (a full-screen triangle or quad, a tile) waits for
    // its pipeline: skipping it can leave a black tile. The waits share 50 ms per presented frame
    // (TOTK_PIPE_SKIP=fullscreen skips them instead). A one-shot draw waits without that budget. Everything else is
    // skipped while its pipeline is built.
    static uint64_t wait_frame;
    static uint64_t wait_left_ns;
    uint32_t wait_ms = 0;
    uint64_t t0 = 0;
    if (oneshot > 0) {
        wait_ms = ONESHOT_WAIT_MS;
    } else if (skip < 2 && count <= 6 && (uint64_t)sc.extent.width * sc.extent.height * 2 >= (uint64_t)ex->pass_w * ex->pass_h) {
        uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
        if (f != wait_frame) wait_frame = f, wait_left_ns = 50000000;
        wait_ms = (uint32_t)(wait_left_ns / 1000000);
        if (wait_ms) t0 = kern_now_ns();
    }
    uint64_t w0 = kern_now_ns();
    VkPipeline p = vk_pipeline_graphics_async(&s, &info, &pending, wait_ms);
    if (t0) {
        uint64_t spent = kern_now_ns() - t0;
        wait_left_ns = spent < wait_left_ns ? wait_left_ns - spent : 0;
    }
    const char *why = oneshot > 0 ? "one-shot, shader" : wait_ms ? "full-screen" : NULL;
    if (pending && oneshot < 0 && !vk_exec_targets_recent(ex, skip >= 1)) {
        perf_count(PERF_CNT_ONESHOT_WAIT);
        why = "one-shot";
        p = vk_pipeline_graphics_async(&s, &info, &pending, ONESHOT_WAIT_MS);
    }
    uint64_t waited = kern_now_ns() - w0;
    static int wait_logs;
    if (why && waited >= 2000000 && wait_logs < 200) {
        char tg[512];
        wait_logs++;
        vk_exec_targets_describe(ex, tg, sizeof tg);
        rt_log("vk_exec: frame %llu: a draw waited %.1f ms for its pipeline (%s%s): %u vertices, pass %ux%u, %s",
               (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1, waited / 1e6, why,
               pending ? ", skipped after all" : "", count, ex->pass_w, ex->pass_h, tg);
    }
    if (pending) return 0;   // a background worker is building it: the draw is skipped until it is ready
    if (!p || !info) return warn_once(W_PIPELINE, "a graphics pipeline could not be created: draws with it are skipped"), 0;
    bind_pipeline(ex, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    if (!B.st_valid || memcmp(&B.st, &s, sizeof s)) {
        vk_pipeline_set_dynamic(ex->cb, &s);
        ex->recorded++;
        B.st = s, B.st_valid = 1;
    }
    record_fixed(ex);
    repeat_stats(ex, info);
    bind_resources(ex, info);
    if (fbl & ex->fb_dirty) feedback_barrier(ex);
    feedback_written(ex, &s);
    bind_vertex_buffers(ex);
    if (vk_dump_armed) vk_dump_draw(ex, &s, p, info);
    vk_exec_oq_draw(ex);
    return 1;
}

static void bind_index(VkExec *ex, const void *data, uint64_t bytes, VkIndexType type) {
    VkBuffer b;
    VkDeviceSize o;
    memcpy(scratch(ex, bytes, &b, &o), data, bytes);
    vkCmdBindIndexBuffer(ex->cb, b, o, type);
    ex->recorded++;
}

static void op_draw(VkExec *ex, const p_draw *d) {
    c0_binst = (uint32_t)d->base_instance;
    if (d->count <= 0 || !prepare_draw(ex, d->prim, d->count, 0)) return;
    vk_exec_frame.draws++;
    uint32_t inst = d->instances > 0 ? (uint32_t)d->instances : 1, first = (uint32_t)d->first, n = (uint32_t)d->count;
    if (d->prim == NVN_DRAW_PRIMITIVE_QUADS && n >= 4) {   // quads become two triangles each
        uint32_t q = n / 4;
        uint32_t *ix = malloc(sizeof *ix * 6 * q);
        if (!ix) rt_fatal("vk: out of memory");
        for (uint32_t i = 0; i < q; i++) {
            uint32_t v = first + 4 * i, k[6] = {0, 1, 2, 0, 2, 3};
            for (int j = 0; j < 6; j++) ix[6 * i + j] = v + k[j];
        }
        bind_index(ex, ix, sizeof *ix * 6 * q, VK_INDEX_TYPE_UINT32);
        free(ix);
        vkCmdDrawIndexed(ex->cb, 6 * q, inst, 0, 0, (uint32_t)d->base_instance);
    } else if (d->prim == NVN_DRAW_PRIMITIVE_LINE_LOOP && n >= 2) {   // a strip that returns to its first vertex
        uint32_t *ix = malloc(sizeof *ix * (n + 1));
        if (!ix) rt_fatal("vk: out of memory");
        for (uint32_t i = 0; i < n; i++) ix[i] = first + i;
        ix[n] = first;
        bind_index(ex, ix, sizeof *ix * (n + 1), VK_INDEX_TYPE_UINT32);
        free(ix);
        vkCmdDrawIndexed(ex->cb, n + 1, inst, 0, 0, (uint32_t)d->base_instance);
    } else {
        vkCmdDraw(ex->cb, n, inst, first, (uint32_t)d->base_instance);
    }
    ex->recorded++;
}

// Primitive restart of an indexed draw. NVN restarts strips wherever an index equals the SetPrimitiveRestart value;
// Vulkan restarts only at the all-ones value of the bound index type, and only strips and fans without
// primitiveTopologyListRestart. Byte indices are widened to 16 bits, so their restart value becomes 0xffff
// (op_draw_idx). Without restart the 0xffff entries of TOTK's terrain and cliff strips fetch vertex 65535 + base and
// the triangles around them drop out.
static int restart_for(const VkExec *ex, int prim, int itype) {
    if (!ex->prim_restart.enable) return 0;
    switch (vk_pipeline_topology(prim)) {
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP: case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY: case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY: break;
    default: return 0;   // lists: a restart index only drops the unfinished primitive
    }
    static const uint32_t all_ones[3] = {0xffu, 0xffffu, 0xffffffffu};
    if (ex->prim_restart.index == all_ones[itype]) return 1;
    warn_once(W_RESTART, "primitive restart index %#x with %d-byte indices is not replayed: strips drawn without restart",
              ex->prim_restart.index, 1 << itype);
    return 0;
}

static void op_draw_idx(VkExec *ex, const p_draw_idx *d) {
    c0_binst = (uint32_t)d->base_instance;
    static const uint32_t isize[3] = {1, 2, 4};
    if (d->itype < 0 || d->itype > 2) return warn_once(W_INDEX, "index type %d is unknown: draw skipped", d->itype);
    int restart = restart_for(ex, d->prim, d->itype);
    if (d->count <= 0 || !prepare_draw(ex, d->prim, d->count, restart)) return;
    vk_exec_frame.draws++;
    uint64_t bytes = (uint64_t)d->count * isize[d->itype];
    VkMemSpan sp;
    if (!vk_mem_resolve(d->ibuf, bytes, &sp)) return warn_once(W_INDEX, "index buffer %#llx is not backed: draws with it are skipped", (unsigned long long)d->ibuf);
    if (d->itype == NVN_INDEX_TYPE_UNSIGNED_BYTE) {   // no uint8 index type here: widen (host-readable backing only)
        if (sp.alloc) return warn_once(W_INDEX, "byte indices in device-local memory: draw skipped");
        vk_mem_count(sp.cls, VK_MUSE_CPU_READ, bytes);
        vk_mem_count(VK_MCLS_SCRATCH, VK_MUSE_IB, 2 * bytes);
        uint16_t *ix = malloc(sizeof *ix * (size_t)d->count);
        if (!ix) rt_fatal("vk: out of memory");
        for (int i = 0; i < d->count; i++) {
            uint8_t v = ((const uint8_t *)(uintptr_t)d->ibuf)[i];
            ix[i] = restart && v == 0xff ? 0xffff : v;
        }
        bind_index(ex, ix, sizeof *ix * (size_t)d->count, VK_INDEX_TYPE_UINT16);
        free(ix);
    } else if (sp.offset % isize[d->itype]) {
        return warn_once(W_INDEX, "index buffer %#llx is not aligned to its index size: draw skipped", (unsigned long long)d->ibuf);
    } else {
        VK_MEM_USE(sp);
        vkCmdBindIndexBuffer(ex->cb, sp.buffer, sp.offset, d->itype == NVN_INDEX_TYPE_UNSIGNED_SHORT ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        ex->recorded++;
        vk_mem_count(sp.cls, VK_MUSE_IB, bytes);
        VK_MEM_BOUND(sp);
    }
    vkCmdDrawIndexed(ex->cb, (uint32_t)d->count, d->instances > 0 ? (uint32_t)d->instances : 1, 0, d->base_vertex,
                     (uint32_t)d->base_instance);
    ex->recorded++;
}

// DrawElementsIndirect: the arguments are read by the GPU from the pool buffer (they are often written by an earlier
// dispatch), as nxnative nvn_mtl.m does. The base instance stays out of c0 (0x644): c0 cannot be patched by a transfer
// inside a pass; gl_BaseInstance of the Vulkan draw is the argument's. Byte indices need the count to be widened: skipped.
static void op_draw_idx_ind(VkExec *ex, const p_draw_idx_ind *d) {
    c0_binst = 0;
    if (d->itype != NVN_INDEX_TYPE_UNSIGNED_SHORT && d->itype != NVN_INDEX_TYPE_UNSIGNED_INT)
        return warn_once(W_INDEX, "indirect draw with index type %d: skipped", d->itype);
    VkMemSpan ib, ip;
    if (!vk_mem_resolve(d->ind, sizeof(VkDrawIndexedIndirectCommand), &ip) || ip.offset % 4)
        return warn_once(W_INDIRECT, "indirect draw arguments at %#llx are not backed or not aligned: draws skipped", (unsigned long long)d->ind);
    if (!vk_mem_resolve(d->ibuf, 1, &ib) || ib.offset % (d->itype == NVN_INDEX_TYPE_UNSIGNED_SHORT ? 2 : 4))
        return warn_once(W_INDEX, "index buffer %#llx is not backed or not aligned: indirect draws skipped", (unsigned long long)d->ibuf);
    if (!prepare_draw(ex, d->prim, 0x7fffffff, restart_for(ex, d->prim, d->itype))) return;
    vk_exec_frame.draws++;
    VK_MEM_USE(ip);
    VK_MEM_USE(ib);
    vkCmdBindIndexBuffer(ex->cb, ib.buffer, ib.offset, d->itype == NVN_INDEX_TYPE_UNSIGNED_SHORT ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexedIndirect(ex->cb, ip.buffer, ip.offset, 1, sizeof(VkDrawIndexedIndirectCommand));
    ex->recorded += 2;
    vk_mem_count(ip.cls, VK_MUSE_IND, sizeof(VkDrawIndexedIndirectCommand));
    VK_MEM_BOUND(ip);
    VK_MEM_BOUND(ib);
}

// ---- dispatch and uniform buffer updates

static void op_dispatch(VkExec *ex, const p_dispatch *d) {
    if (d->x <= 0 || d->y <= 0 || d->z <= 0) return;
    if (!ex->prog) return warn_once(W_PROGRAM, "a dispatch without a bound program is skipped");
    cb_res(ex);
    dummies_init();
    int present;
    const GfxShader *cs = shader_for(ex, NVN_STAGE_CS, &present, NULL, 0);   // synchronous: a skipped dispatch changes results
    if (!cs) return;
    const VkPipeInfo *info = NULL;
    VkPipeline p = vk_pipeline_compute(cs, &info);
    if (!p || !info) return warn_once(W_PIPELINE, "a compute pipeline could not be created: dispatches with it are skipped");
    vk_exec_barrier(ex);   // ends the pass; earlier writes are visible to the shader
    bind_pipeline(ex, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    c0_dim[0] = (uint32_t)d->x, c0_dim[1] = (uint32_t)d->y, c0_dim[2] = (uint32_t)d->z;
    bind_resources(ex, info);
    vkCmdDispatch(ex->cb, (uint32_t)d->x, (uint32_t)d->y, (uint32_t)d->z);
    ex->recorded++;
    if (vk_dump_armed) vk_dump_dispatch(ex, cs, p, info, d);
}

// DispatchComputeIndirect: vkCmdDispatchIndirect from the pool buffer. The shader reads the group counts from c0 0x200,
// so they are copied there on the GPU, into a c0 range no other dispatch uses, before the dispatch.
static void op_dispatch_ind(VkExec *ex, const p_dispatch_ind *d) {
    if (!ex->prog) return warn_once(W_PROGRAM, "a dispatch without a bound program is skipped");
    VkMemSpan ip;
    if (!vk_mem_resolve(d->ind, sizeof(VkDispatchIndirectCommand), &ip) || ip.offset % 4)
        return warn_once(W_INDIRECT, "indirect dispatch arguments at %#llx are not backed or not aligned: skipped", (unsigned long long)d->ind);
    cb_res(ex);
    dummies_init();
    int present;
    const GfxShader *cs = shader_for(ex, NVN_STAGE_CS, &present, NULL, 0);
    if (!cs) return;
    const VkPipeInfo *info = NULL;
    VkPipeline p = vk_pipeline_compute(cs, &info);
    if (!p || !info) return warn_once(W_PIPELINE, "a compute pipeline could not be created: dispatches with it are skipped");
    vk_exec_barrier(ex);
    VK_MEM_USE(ip);
    bind_pipeline(ex, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    c0_dim[0] = c0_dim[1] = c0_dim[2] = 0;
    B.c0_valid[2] = 0;   // a fresh c0 range: the copy below must not change one an earlier dispatch reads
    bind_resources(ex, info);
    if (B.c0_valid[2]) {   // the shader binds c0
        VkBufferCopy cp = {ip.offset, B.c0_off[2] + 0x200, sizeof(VkDispatchIndirectCommand)};
        vkCmdCopyBuffer(ex->cb, ip.buffer, B.c0_buf[2], 1, &cp);
        ex->recorded++;
        vk_exec_barrier(ex);
        B.c0_valid[2] = 0;   // the range no longer holds B.c0[2]
    }
    vkCmdDispatchIndirect(ex->cb, ip.buffer, ip.offset);
    ex->recorded++;
    vk_mem_count(ip.cls, VK_MUSE_IND, sizeof(VkDispatchIndirectCommand));
    VK_MEM_BOUND(ip);
    if (vk_dump_armed) vk_dump_dispatch(ex, cs, p, info, &(p_dispatch){0, 0, 0, d->caller});
}

static void op_update_ub(VkExec *ex, const p_update_ub *u) {
    if (!u->size) return;
    VkMemSpan sp;
    if (!vk_mem_resolve(u->dst, u->size, &sp)) return warn_once(W_UB, "UpdateUniformBuffer to %#llx is not backed: dropped", (unsigned long long)u->dst);
    cb_res(ex);
    vk_exec_barrier(ex);   // earlier draws finished with the old contents
    VkBuffer b;
    VkDeviceSize o;
    memcpy(scratch(ex, u->size, &b, &o), u->data, u->size);
    VK_MEM_PICK(sp);
    VkBufferCopy cp = {o, sp.offset, u->size};
    vkCmdCopyBuffer(ex->cb, b, sp.buffer, 1, &cp);
    vk_mem_gpu_wrote(&sp, u->size);
    ex->recorded++;
    vk_mem_count(sp.cls, VK_MUSE_GPU_WRITE, u->size);
    note_written(ex, u->dst, u->size);
}

// ---- bind ops

static void bind_handles(uint64_t *dst, int cap, const p_bind_handles *b) {
    if (b->stage < 0 || b->stage >= 6 || b->first < 0 || b->n < 0 || b->n > 32 || b->first + b->n > cap) {
        warn_once(W_BIND, "a bind of %d handles at %d of stage %d is out of range: ignored", b->n, b->first, b->stage);
        return;
    }
    memcpy(&dst[b->first], b->h, sizeof(uint64_t) * (size_t)b->n);
}

void vk_exec_op_draw(VkExec *ex, uint32_t op, const void *payload) {
    switch (op) {
    case OP_BIND_PROGRAM: {
        const p_bind_program *b = payload;
        ex->prog = b->prog, ex->prog_stages = b->stages;
        bind_gen++;
        break;
    }
    case OP_BIND_STATE: {
        const p_bind_state *b = payload;
        blend_st bs;
        switch (b->kind) {
        case 0:
            memcpy(&bs, b->bytes, sizeof bs);
            if (bs.target < 8) ex->pipe.blend[bs.target] = bs;
            break;
        case 1: memcpy(&ex->pipe.color, b->bytes, sizeof ex->pipe.color); break;
        case 2: memcpy(&ex->pipe.chmask, b->bytes, sizeof ex->pipe.chmask); break;
        case 3: memcpy(&ex->pipe.ms, b->bytes, sizeof ex->pipe.ms); break;
        case 4: memcpy(&ex->pipe.poly, b->bytes, sizeof ex->pipe.poly); break;
        case 5: memcpy(&ex->pipe.ds, b->bytes, sizeof ex->pipe.ds); break;
        case 6: memcpy(&ex->point_size, b->bytes, sizeof ex->point_size); break;
        }
        break;
    }
    case OP_BIND_VATTR: {
        const p_bind_vattr *b = payload;
        if (b->first < 0 || b->n < 0 || b->first + b->n > 16) break;
        memset(ex->pipe.vattr, 0, sizeof ex->pipe.vattr);   // MNG: the new list replaces the old one
        memcpy(&ex->pipe.vattr[b->first], b->st, sizeof(uint32_t) * (size_t)b->n);
        break;
    }
    case OP_BIND_VSTREAM: {
        const p_bind_vstream *b = payload;
        if (b->first < 0 || b->n < 0 || b->first + b->n > 16) break;
        memset(ex->pipe.vstream, 0, sizeof ex->pipe.vstream);
        memcpy(&ex->pipe.vstream[b->first], b->st, sizeof(vs_st) * (size_t)b->n);
        break;
    }
    case OP_BIND_VBUF: {
        const p_bind_vbuf *b = payload;
        if (b->first < 0 || b->n < 0 || b->first + b->n > 16) break;
        memcpy(&ex->vbuf[b->first], b->r, sizeof(NVNbufferRange) * (size_t)b->n);
        break;
    }
    case OP_BIND_UBO: {
        const p_bind_buf *b = payload;
        if (b->stage >= 0 && b->stage < 6 && b->index >= 0 && b->index < 18) ex->ubo[b->stage][b->index] = *b;
        bind_gen++;
        break;
    }
    case OP_BIND_SSBO: {
        const p_bind_buf *b = payload;
        if (b->stage >= 0 && b->stage < 6 && b->index >= 0 && b->index < 16) ex->ssbo[b->stage][b->index] = *b;
        bind_gen++;
        break;
    }
    case OP_BIND_TEX: {
        const p_bind_handles *b = payload;
        bind_handles(b->stage >= 0 && b->stage < 6 ? ex->tex[b->stage] : NULL, 128, b);
        ex->tex_serial++;
        bind_gen++;
        break;
    }
    case OP_BIND_SAMP: {
        const p_bind_handles *b = payload;
        bind_handles(b->stage >= 0 && b->stage < 6 ? ex->samp[b->stage] : NULL, 128, b);
        break;
    }
    case OP_BIND_IMG: {
        const p_bind_handles *b = payload;
        bind_handles(b->stage >= 0 && b->stage < 6 ? ex->img[b->stage] : NULL, 32, b);
        bind_gen++;
        break;
    }
    case OP_UPDATE_UB: op_update_ub(ex, payload); break;
    case OP_DRAW: op_draw(ex, payload); break;
    case OP_DRAW_IDX: op_draw_idx(ex, payload); break;
    case OP_DISPATCH: op_dispatch(ex, payload); break;
    case OP_DRAW_IDX_IND: op_draw_idx_ind(ex, payload); break;
    case OP_DISPATCH_IND: op_dispatch_ind(ex, payload); break;
    default: rt_fatal("vk: op %s is not a draw op", nvn_op_name(op));
    }
}
