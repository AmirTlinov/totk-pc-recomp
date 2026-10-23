// Submission, fences and the completion thread of the Vulkan back end.
// One timeline semaphore (VkCtx.timeline) orders everything: each submission records its command handles into a
// primary command buffer (vk_exec_record, task T6c) and signals the next timeline value, which is the NVN fence value
// the front end hands to syncs. The completion thread waits on the timeline and, in submit order, runs the CPU side
// effects queued for each value (event writes, counter reports, sync state), then frees the T5 objects whose users
// completed. A value counts as done only after its side effects ran, so a guest woken by fence_wait sees them.
//
// The submission shape (one timeline value per submission, signalled at ALL_COMMANDS, vkWaitSemaphores for waits, a
// failed wait treated as a lost device) follows LAS Recomp runtime/nvn/vulkan/exec.c:39-110, 355-397
// (https://github.com/peiikai/LASRecomp @ da35b83a9611d046a9a681aae668d7085954d37f, MIT, Copyright (c) 2026 Peiikai).
// Side effects written after GPU completion, as MacNativeGames does: games/totk/third_party/d2r_gfx/gfx/nvn/hle_nvn.c:7-9
// (https://github.com/ly05010419/MacNativeGames @ b75be8cfa704df3701e86130c8ced268e79d6b11, MIT, Copyright (c) 2026
// jhaizhou; origin d2r-mobile).
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "gfx.h"
#include "host.h"
#include "nvn_api.h"
#include "vk.h"

#define WEAK __attribute__((weak))

typedef struct { uint64_t fence; void (*fn)(void *); void *arg; } Pending;
typedef struct { VkCommandBuffer cb; uint64_t fence; } CmdSlot;   // reusable once the timeline reached fence

static SRWLOCK start_lock = SRWLOCK_INIT;
static int started;
static HANDLE completion_thread;

// Submissions: one at a time, so timeline values are signalled in the order they are handed out.
static SRWLOCK sub_lock = SRWLOCK_INIT;
static VkCommandPool cmd_pool;
static CmdSlot *slots;
static int nslots;
static int cur_slot;          // the slot vk_sync_submit records into now (an index: take_slot may move slots)
static uint64_t cur_fence;    // the value of the submission being recorded

// Shared with the completion thread (st_lock).
static SRWLOCK st_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE cv_work = CONDITION_VARIABLE_INIT;   // a submission, a callback or stop
static CONDITION_VARIABLE cv_done = CONDITION_VARIABLE_INIT;   // done advanced
static uint64_t issued, done;     // newest submitted value; newest value whose side effects ran
static int stop;
static Pending *queue;            // side effects in the order they were added
static int qhead, qn, qcap;

// Without the replay (T6c) only empty recordings ({OP_BEGIN, OP_END}) can be submitted.
WEAK int vk_exec_record(VkCommandBuffer cb, const uint64_t *handles, int n, uint64_t fence) {
    (void)cb, (void)fence;
    for (int i = 0; i < n; i++) {
        const op_hdr *h = handles[i] ? nvn_op_first(handles[i]) : NULL;
        if (!h || h->op != OP_BEGIN || h->size < sizeof *h || nvn_op_next(h)->op != OP_END)
            rt_fatal("vk: op replay is not implemented (T6c)");
    }
    return 0;
}

static DWORD WINAPI completion_main(void *arg) {
    VkCtx *c = arg;
    AcquireSRWLockExclusive(&st_lock);
    for (;;) {
        while (!stop && done == issued && !(qn && queue[qhead].fence <= done))
            SleepConditionVariableSRW(&cv_work, &st_lock, INFINITE, 0);
        if (stop) break;
        uint64_t reached = done;
        if (issued > done) {
            uint64_t want = done + 1;
            ReleaseSRWLockExclusive(&st_lock);
            VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
                                      .pSemaphores = &c->timeline, .pValues = &want};
            VkResult r = vkWaitSemaphores(c->device, &wi, 100000000ull);   // 100 ms: stop is checked between waits
            if (r != VK_SUCCESS && r != VK_TIMEOUT) rt_fatal("vk: vkWaitSemaphores failed (%d; device lost?)", (int)r);
            vkGetSemaphoreCounterValue(c->device, c->timeline, &reached);
            AcquireSRWLockExclusive(&st_lock);
            if (reached > issued) reached = issued;
        }
        while (qn && queue[qhead].fence <= reached) {
            Pending p = queue[qhead++];
            qn--;
            ReleaseSRWLockExclusive(&st_lock);
            p.fn(p.arg);
            AcquireSRWLockExclusive(&st_lock);
        }
        if (reached > done) {
            ReleaseSRWLockExclusive(&st_lock);
            vk_mem_collect();
            AcquireSRWLockExclusive(&st_lock);
            __atomic_store_n(&done, reached, __ATOMIC_RELEASE);
            WakeAllConditionVariable(&cv_done);
        }
    }
    ReleaseSRWLockExclusive(&st_lock);
    return 0;
}

void vk_sync_start(void) {
    if (__atomic_load_n(&started, __ATOMIC_ACQUIRE)) return;
    AcquireSRWLockExclusive(&start_lock);
    if (!started) {
        if (!vk_init()) rt_fatal("vk: no usable Vulkan device (see the vk: lines above; TOTK_GPU=null runs without a GPU)");
        VkCtx *c = vk_ctx();
        VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                      .queueFamilyIndex = c->gfx_family};
        if (vkCreateCommandPool(c->device, &pi, NULL, &cmd_pool) != VK_SUCCESS) rt_fatal("vk: submission command pool");
        uint64_t now = 0;
        vkGetSemaphoreCounterValue(c->device, c->timeline, &now);
        if (issued > now) {   // a new device after vk_shutdown: values keep increasing for the front end
            VkSemaphoreSignalInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = c->timeline,
                                        .value = issued};
            if (vkSignalSemaphore(c->device, &si) != VK_SUCCESS) rt_fatal("vk: timeline restart at %llu", (unsigned long long)issued);
            now = issued;
        }
        AcquireSRWLockExclusive(&st_lock);
        issued = done = now;
        stop = 0;
        ReleaseSRWLockExclusive(&st_lock);
        vk_mem_note_submit(now);
        completion_thread = CreateThread(NULL, 0, completion_main, c, 0, NULL);
        if (!completion_thread) rt_fatal("vk: cannot start the completion thread (error %lu)", GetLastError());
        __atomic_store_n(&started, 1, __ATOMIC_RELEASE);
    }
    ReleaseSRWLockExclusive(&start_lock);
}

// A command buffer the GPU no longer uses, claimed for value `fence`. Called with sub_lock held.
static CmdSlot *take_slot(VkCtx *c, uint64_t fence) {
    uint64_t gpu = 0;
    vkGetSemaphoreCounterValue(c->device, c->timeline, &gpu);
    for (int i = 0; i < nslots; i++)
        if (slots[i].fence <= gpu) {
            slots[i].fence = fence;
            cur_slot = i;
            return &slots[i];
        }
    CmdSlot *n = realloc(slots, sizeof *n * (size_t)(nslots + 1));
    if (!n) rt_fatal("vk: out of memory (command buffers)");
    slots = n;
    CmdSlot *s = &slots[nslots];
    cur_slot = nslots;
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cmd_pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    if (vkAllocateCommandBuffers(c->device, &ai, &s->cb) != VK_SUCCESS) rt_fatal("vk: command buffer allocation");
    s->fence = fence;
    nslots++;
    return s;
}

VkCommandBuffer vk_sync_split(void) {
    VkCtx *c = vk_ctx();
    VkCommandBuffer cb = slots[cur_slot].cb;
    vkEndCommandBuffer(cb);
    vk_upload_flush(0);   // the uploads this part asked for precede it in queue order
    VkCommandBufferSubmitInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &cbi};
    vk_queue_lock();
    VkResult r = vkQueueSubmit2(c->gfx_queue, 1, &si, VK_NULL_HANDLE);
    vk_queue_unlock();
    if (r != VK_SUCCESS) rt_fatal("vk: vkQueueSubmit2 of a part of value %llu failed (%d)", (unsigned long long)cur_fence, (int)r);
    vk_mem_note_submit(cur_fence);   // objects destroyed from here on may be in use by the part just submitted
    CmdSlot *s = take_slot(c, cur_fence);   // the submitted part keeps its slot until the value is reached
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(s->cb, &bi);
    return s->cb;
}

uint64_t vk_sync_submit(const uint64_t *handles, int n) {
    vk_sync_start();
    VkCtx *c = vk_ctx();
    AcquireSRWLockExclusive(&sub_lock);
    uint64_t fence = __atomic_load_n(&issued, __ATOMIC_ACQUIRE) + 1;
    cur_fence = fence;
    CmdSlot *s = take_slot(c, fence);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    uint64_t t0 = perf_begin();
    vkBeginCommandBuffer(s->cb, &bi);
    int recorded = vk_exec_record(s->cb, handles, n, fence);
    s = &slots[cur_slot];   // the last part (vk_sync_split)
    vkEndCommandBuffer(s->cb);
    perf_replay(t0);
    // After the recording: the uploads it asked for (and every earlier one) precede it in queue order.
    vk_upload_flush(0);
    VkCommandBufferSubmitInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->cb};
    VkSemaphoreSubmitInfo sig = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = c->timeline,
                                 .value = fence, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = recorded ? 1 : 0,
                        .pCommandBufferInfos = &cbi, .signalSemaphoreInfoCount = 1, .pSignalSemaphoreInfos = &sig};
    vk_queue_lock();
    VkResult r = vkQueueSubmit2(c->gfx_queue, 1, &si, VK_NULL_HANDLE);
    vk_queue_unlock();
    if (r != VK_SUCCESS) rt_fatal("vk: vkQueueSubmit2 of value %llu failed (%d)", (unsigned long long)fence, (int)r);
    s->fence = recorded ? fence : 0;
    vk_mem_note_submit(fence);
    AcquireSRWLockExclusive(&st_lock);
    __atomic_store_n(&issued, fence, __ATOMIC_RELEASE);
    WakeAllConditionVariable(&cv_work);
    ReleaseSRWLockExclusive(&st_lock);
    ReleaseSRWLockExclusive(&sub_lock);
    return fence;
}

void vk_completion_add(uint64_t fence, void (*fn)(void *), void *arg) {
    AcquireSRWLockExclusive(&st_lock);
    if (qhead + qn == qcap) {
        if (qhead) {
            memmove(queue, queue + qhead, sizeof *queue * (size_t)qn);
            qhead = 0;
        } else {
            int cap = qcap ? qcap * 2 : 256;
            Pending *q = realloc(queue, sizeof *q * (size_t)cap);
            if (!q) rt_fatal("vk: out of memory (completion queue)");
            queue = q, qcap = cap;
        }
    }
    queue[qhead + qn++] = (Pending){fence, fn, arg};
    WakeAllConditionVariable(&cv_work);
    ReleaseSRWLockExclusive(&st_lock);
}

static int fence_wait_impl(uint64_t value, uint64_t timeout_ns);

int vk_sync_fence_wait(uint64_t value, uint64_t timeout_ns) {
    uint64_t p0 = perf_begin();
    int ok = fence_wait_impl(value, timeout_ns);
    perf_end(PERF_FENCE, p0);
    return ok;
}

static int fence_wait_impl(uint64_t value, uint64_t timeout_ns) {
    vk_sync_start();
    AcquireSRWLockExclusive(&st_lock);
    int ok;
    if (value > issued) {
        ok = 0;   // never submitted: never reached (as in the null back end)
    } else {
        uint64_t t0 = kern_now_ns();
        while (done < value) {
            DWORD ms = INFINITE;
            if (timeout_ns < (1ull << 62)) {
                uint64_t el = kern_now_ns() - t0;
                if (el >= timeout_ns) break;
                uint64_t left = (timeout_ns - el + 999999) / 1000000;
                ms = left >= INFINITE ? INFINITE - 1 : (DWORD)left;
            }
            SleepConditionVariableSRW(&cv_done, &st_lock, ms, 0);
        }
        ok = done >= value;
    }
    ReleaseSRWLockExclusive(&st_lock);
    return ok;
}

uint64_t vk_sync_fence_done(void) { return __atomic_load_n(&done, __ATOMIC_ACQUIRE); }

void vk_sync_finish(void) {
    vk_sync_start();
    vk_sync_fence_wait(__atomic_load_n(&issued, __ATOMIC_ACQUIRE), UINT64_MAX);
    vk_upload_flush(1);
}

void vk_sync_shutdown(void) {
    AcquireSRWLockExclusive(&start_lock);
    if (started) {
        vk_sync_finish();
        AcquireSRWLockExclusive(&st_lock);
        stop = 1;
        WakeAllConditionVariable(&cv_work);
        ReleaseSRWLockExclusive(&st_lock);
        WaitForSingleObject(completion_thread, INFINITE);
        CloseHandle(completion_thread);
        completion_thread = NULL;
        AcquireSRWLockExclusive(&st_lock);
        qhead = qn = 0;   // callbacks of values never submitted
        ReleaseSRWLockExclusive(&st_lock);
        AcquireSRWLockExclusive(&sub_lock);
        VkCtx *c = vk_ctx();
        if (c && cmd_pool) vkDestroyCommandPool(c->device, cmd_pool, NULL);
        cmd_pool = VK_NULL_HANDLE;
        free(slots);
        slots = NULL;
        nslots = 0;
        ReleaseSRWLockExclusive(&sub_lock);
        __atomic_store_n(&started, 0, __ATOMIC_RELEASE);
    }
    ReleaseSRWLockExclusive(&start_lock);
}
