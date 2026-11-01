// The recorder and lifetime implementation below are extracted unchanged from
// the patched production source. Only diagnostics and the GPU are stubbed.
#include <assert.h>
#include <malloc/malloc.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "nvn_obj.h"
#include "nvn_command_owner.h"
#define NVN(name, result, ...) static result h_##name(__VA_ARGS__)
#define NVN_NOPROTO NVN
#define LOG(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#define die(...) do { LOG(__VA_ARGS__); abort(); } while (0)
#define B(v) ((v) & 0xffu)
static _Atomic uint64_t g_frame_pub;
static int totk_life_match(const tex_t *t) { (void)t; return 0; }
static int cmdlog_env(void) { return 0; }
static int async_replay(void) { return 0; }
static int perf_stats_on(void) { return 0; }
static int totk_life_on(void) { return 0; }
static int totk_life_cb_take(const void *p) { (void)p; return 0; }
static int totk_life_handle_has(uint64_t h) { (void)h; return 0; }
static void totk_life_handle_add(uint64_t h) { (void)h; }
static void totk_life_cb_mark(const void *p) { (void)p; }
static void totk_life_log(const char *s, ...) { (void)s; }
static void guest_chain(const char *s) { (void)s; }
static int call_stats_on(void);
static void call_stats_end(const void *, const uint8_t *);
static const tex_t *probe_rt;
void mtl_pin_refs(void (*mark)(const void *)) { mark(probe_rt); }
void mtl_probe_pin_rt(const tex_t *t) { probe_rt = t; }
void mtl_dead_forget(const uint64_t *serials, size_t n) { (void)serials; (void)n; }
uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
uint64_t dm_nvn_frame_count(void) { return 0; }
__thread sigjmp_buf *runtime_fault_guard;
#include "recorder.inc"

static unsigned checks;
static size_t avoided_body_bytes, compact_recording_bytes, copied_recording_bytes;
#define CHECK(x) do { if (!(x)) die("owner probe line %d: %s", __LINE__, #x); checks++; } while (0)
typedef struct { NVNcommandBuffer cb; uint8_t control[256], commands[1024]; pool_t pool; } Recorder;
static void init(Recorder *r) {
    memset(r, 0, sizeof *r); r->pool.mem = r->commands;
    h_nvnCommandBufferInitialize(&r->cb, NULL);
    h_nvnCommandBufferAddControlMemory(&r->cb, r->control, sizeof r->control);
    h_nvnCommandBufferAddCommandMemory(&r->cb, (NVNmemoryPool *)&r->pool, 0, sizeof r->commands);
}
static void retire(Recorder *r) {
    h_nvnCommandBufferAddControlMemory(&r->cb, r->control, sizeof r->control);
}
static void destroy(Recorder *r) { retire(r); h_nvnCommandBufferFinalize(&r->cb); }
static void flat(NVNcommandHandle h) {
    const uint8_t *p = (const void *)(uintptr_t)h;
    for (;;) {
        op_hdr op; memcpy(&op, p, sizeof op);
        CHECK(op.op != OP_CALL && op.op != OP_JUMP && op.size >= 8);
        if (op.op == OP_END) break;
        p += op.size;
    }
}
static void collect(void) {
    for (int i = 0; i < 4; i++) pin_sweep(g_submits + 10000 + i, 1);
}
static p_bind_handles *binding(NVNcommandBuffer *cb, uint64_t *value, int deferred) {
    p_bind_handles *b = RECN(cb, OP_BIND_TEX, p_bind_handles, h, 1);
    b->stage = 1; b->first = 2; b->n = 1;
    b->src = deferred ? (uint64_t)(uintptr_t)value : 0; b->h[0] = *value;
    return b;
}
static p_bind_handles *first_binding(NVNcommandHandle h) {
    return (void *)((uint8_t *)(uintptr_t)h + 16);
}
static void shared_and_nested(void) {
    Recorder source, parent, reference; init(&source); init(&parent); init(&reference);
    h_nvnCommandBufferBeginRecording(&source.cb);
    uint64_t value = 19; binding(&source.cb, &value, 0);
    for (int i = 0; i < 4096; i++) h_nvnCommandBufferSetScissor(&source.cb, i, i+1, 16, 16);
    NVNcommandHandle child = h_nvnCommandBufferEndRecording(&source.cb);
    const NvnCommandOwner *owner = nvn_command_owner_acquire(child);
    const NvnCommandOwnerInfo *info = nvn_command_owner_info(owner);
    CHECK(info->control_origin == (uintptr_t)source.control);
    CHECK(info->recorder == (uintptr_t)&source.cb && !info->has_deferred);
    h_nvnCommandBufferBeginRecording(&parent.cb);
    h_nvnCommandBufferBeginRecording(&reference.cb);
    for (int i = 0; i < 2; i++) {
        h_nvnCommandBufferCallCommands(&parent.cb, 1, &child);
        h_nvnCommandBufferCopyCommands(&reference.cb, 1, &child);
    }
    CHECK(CB(&parent.cb)->cur - CB(&parent.cb)->seg == 8 + 2*16);
    CHECK(CB(&reference.cb)->cur - CB(&reference.cb)->seg == 8 + 2*(info->bytes-16));
    avoided_body_bytes = 2*(info->bytes-16);
    compact_recording_bytes = CB(&parent.cb)->cur - CB(&parent.cb)->seg;
    copied_recording_bytes = CB(&reference.cb)->cur - CB(&reference.cb)->seg;
    CHECK(CB(&parent.cb)->cmd_used == CB(&reference.cb)->cmd_used);
    CHECK(CB(&parent.cb)->hw_used == CB(&reference.cb)->hw_used);
    CHECK(CB(&parent.cb)->last_rec == CB(&reference.cb)->last_rec);
    retire(&source); // The retained child is already absent from the handle registry.
    NVNcommandHandle actual = h_nvnCommandBufferEndRecording(&parent.cb);
    NVNcommandHandle expected = h_nvnCommandBufferEndRecording(&reference.cb);
    size_t bytes = copy_of(actual)->owner->info.bytes;
    CHECK(bytes == copy_of(expected)->owner->info.bytes);
    CHECK(!memcmp((void *)(uintptr_t)actual, (void *)(uintptr_t)expected, bytes));
    const NvnCommandOwner *parent_owner = nvn_command_owner_acquire(actual);
    const NvnCommandOwnerInfo *pi = nvn_command_owner_info(parent_owner);
    CHECK(pi->scope_count == 2 && pi->scopes[0].owner == owner && pi->scopes[1].owner == owner);
    CHECK(pi->scopes[0].offset == 8 && pi->scopes[1].offset == 8 + info->bytes - 16);
    CHECK(pi->scopes[0].bytes == info->bytes - 16);
    flat(actual);
    uint64_t old = info->incarnation;
    h_nvnCommandBufferBeginRecording(&source.cb);
    NVNcommandHandle replacement = h_nvnCommandBufferEndRecording(&source.cb);
    CHECK(nvn_command_owner_info(nvn_command_owner_borrow(replacement))->incarnation != old);
    destroy(&source); destroy(&parent); destroy(&reference);
    CHECK(nvn_command_owner_info(pi->scopes[0].owner)->incarnation == old);
    nvn_command_owner_release(owner); nvn_command_owner_release(parent_owner);
}
static void deferred_is_local(void) {
    Recorder source, a, b; init(&source); init(&a); init(&b);
    uint64_t value = 11;
    h_nvnCommandBufferBeginRecording(&source.cb); binding(&source.cb, &value, 1);
    NVNcommandHandle child = h_nvnCommandBufferEndRecording(&source.cb);
    CHECK(copy_of(child)->has_def);
    h_nvnCommandBufferBeginRecording(&a.cb); h_nvnCommandBufferCallCommands(&a.cb, 1, &child);
    value = 22; resolve_deferred((void *)(uintptr_t)child, 0);
    h_nvnCommandBufferBeginRecording(&b.cb); h_nvnCommandBufferCallCommands(&b.cb, 1, &child);
    retire(&source);
    NVNcommandHandle ha = h_nvnCommandBufferEndRecording(&a.cb), hb = h_nvnCommandBufferEndRecording(&b.cb);
    CHECK(first_binding(ha)->h[0] == 11 && first_binding(hb)->h[0] == 22);
    value = 33; resolve_deferred((void *)(uintptr_t)ha, 0);
    CHECK(first_binding(ha)->h[0] == 33 && first_binding(hb)->h[0] == 22);
    CHECK(copy_of(ha)->owner->info.has_deferred && copy_of(ha)->owner->info.scope_count == 1);
    flat(ha); flat(hb); destroy(&source); destroy(&a); destroy(&b);
}
static void empty_calls(void) {
    Recorder source, parent; init(&source); init(&parent);
    h_nvnCommandBufferBeginRecording(&source.cb);
    NVNcommandHandle child = h_nvnCommandBufferEndRecording(&source.cb);
    h_nvnCommandBufferBeginRecording(&parent.cb);
    h_nvnCommandBufferSetScissor(&parent.cb, 1, 2, 3, 4);
    cmd_t before = *CB(&parent.cb);
    h_nvnCommandBufferCallCommands(&parent.cb, 1, &child);
    CHECK(CB(&parent.cb)->cmd_used == before.cmd_used && CB(&parent.cb)->hw_used == before.hw_used);
    CHECK(CB(&parent.cb)->last_rec == before.last_rec);
    retire(&source);
    NVNcommandHandle h = h_nvnCommandBufferEndRecording(&parent.cb);
    CHECK(copy_of(h)->body_bytes == sizeof(op_hdr) + sizeof(p_scissor));
    CHECK(copy_of(h)->owner->info.scope_count == 1 && copy_of(h)->owner->info.scopes[0].bytes == 0);
    flat(h); destroy(&source); destroy(&parent);
}
static void pending_texture_lifetime(void) {
    size_t before = dm_nvn_pin_count();
    Recorder source, parent; init(&source); init(&parent);
    tex_t texture = {.serial=0xfeed12345}; const NVNtexture *color = (const void *)&texture;
    h_nvnCommandBufferBeginRecording(&source.cb);
    h_nvnCommandBufferSetRenderTargets(&source.cb, 1, &color, NULL, NULL, NULL);
    NVNcommandHandle child = h_nvnCommandBufferEndRecording(&source.cb);
    CHECK(copy_of(child)->pin_count == 1);
    h_nvnCommandBufferBeginRecording(&parent.cb); h_nvnCommandBufferCallCommands(&parent.cb, 1, &child);
    retire(&source); collect(); CHECK(dm_nvn_pin_count() == before + 1);
    NVNcommandHandle hp = h_nvnCommandBufferEndRecording(&parent.cb);
    const NvnCommandOwner *metadata = nvn_command_owner_acquire(hp);
    collect(); CHECK(dm_nvn_pin_count() == before + 1);
    destroy(&source); destroy(&parent); collect(); CHECK(dm_nvn_pin_count() == before);
    // Keeping metadata alone cannot extend GPU resource lifetime.
    CHECK(nvn_command_owner_info(metadata)->scope_count == 1);
    nvn_command_owner_release(metadata);
}
static void copied_texture_lifetime(void) {
    for (int deferred = 0; deferred < 2; deferred++) {
        Recorder source, parent; init(&source); init(&parent);
        tex_t texture = {.serial=0xfeed44440 + deferred}; const NVNtexture *color = (const void *)&texture;
        uint64_t handle_value = 17;
        h_nvnCommandBufferBeginRecording(&source.cb);
        h_nvnCommandBufferSetRenderTargets(&source.cb, 1, &color, NULL, NULL, NULL);
        binding(&source.cb, &handle_value, deferred);
        NVNcommandHandle child = h_nvnCommandBufferEndRecording(&source.cb);
        h_nvnCommandBufferBeginRecording(&parent.cb);
        if (deferred) h_nvnCommandBufferCallCommands(&parent.cb, 1, &child);
        else h_nvnCommandBufferCopyCommands(&parent.cb, 1, &child);
        CHECK(CB(&parent.cb)->cur - CB(&parent.cb)->seg > 24); // Both paths copy now.
        retire(&source); collect(); CHECK(dm_nvn_pin_has_serial(texture.serial));
        NVNcommandHandle parent_handle = h_nvnCommandBufferEndRecording(&parent.cb);
        const p_set_rt *rt = (const void *)((const uint8_t *)(uintptr_t)parent_handle + 16);
        CHECK(rt->color[0]->serial == texture.serial);
        if (deferred) { handle_value = 39; resolve_deferred((const void *)(uintptr_t)parent_handle, 0); }
        flat(parent_handle); destroy(&source); destroy(&parent); collect();
        CHECK(!dm_nvn_pin_has_serial(texture.serial));
    }
}
static void abort_and_deep_owners(void) {
    Recorder a, b; init(&a); init(&b);
    h_nvnCommandBufferBeginRecording(&a.cb); h_nvnCommandBufferSetScissor(&a.cb, 1, 2, 3, 4);
    NVNcommandHandle h = h_nvnCommandBufferEndRecording(&a.cb);
    for (int i = 0; i < 4096; i++) {
        Recorder *src = i % 2 ? &b : &a, *dst = i % 2 ? &a : &b;
        h_nvnCommandBufferBeginRecording(&dst->cb); h_nvnCommandBufferCallCommands(&dst->cb, 1, &h);
        retire(src); h = h_nvnCommandBufferEndRecording(&dst->cb);
        CHECK(copy_of(h)->body_bytes == sizeof(op_hdr) + sizeof(p_scissor));
    }
    const NvnCommandOwner *owner = nvn_command_owner_acquire(h);
    const NvnCommandOwnerInfo *info = nvn_command_owner_info(owner);
    for (int i = 0; i < 4096; i++) { CHECK(info->scope_count == 1); info = nvn_command_owner_info(info->scopes[0].owner); }
    CHECK(info->scope_count == 0);
    // Abandon one child call by restarting and a second by finalization.
    h_nvnCommandBufferBeginRecording(&b.cb); h_nvnCommandBufferCallCommands(&b.cb, 1, &h);
    h_nvnCommandBufferBeginRecording(&b.cb); h_nvnCommandBufferCallCommands(&b.cb, 1, &h);
    destroy(&a); destroy(&b); nvn_command_owner_release(owner);
}
typedef struct {
    NVNcommandHandle handle;
    _Atomic int proceed;
    uint64_t incarnation;
} Queued;
static void *queued_replay(void *argument) {
    Queued *q = argument;
    while (!atomic_load_explicit(&q->proceed, memory_order_acquire)) sched_yield();
    const NvnCommandOwnerInfo *owner = nvn_command_owner_info(nvn_command_owner_borrow(q->handle));
    CHECK(owner->incarnation == q->incarnation && owner->scope_count == 1);
    flat(q->handle);
    const p_set_rt *rt = (const void *)((const uint8_t *)(uintptr_t)q->handle + 16);
    CHECK(rt->color[0] && rt->color[0]->serial == 0xfeed76543);
    copy_unref(copy_of(q->handle)); // Same final release as x_thread.
    return NULL;
}
static void asynchronous_retirement(void) {
    Recorder source, parent; init(&source); init(&parent);
    tex_t texture = {.serial=0xfeed76543}; const NVNtexture *color = (const void *)&texture;
    h_nvnCommandBufferBeginRecording(&source.cb);
    h_nvnCommandBufferSetRenderTargets(&source.cb, 1, &color, NULL, NULL, NULL);
    NVNcommandHandle child = h_nvnCommandBufferEndRecording(&source.cb);
    h_nvnCommandBufferBeginRecording(&parent.cb); h_nvnCommandBufferCallCommands(&parent.cb, 1, &child);
    NVNcommandHandle handle = h_nvnCommandBufferEndRecording(&parent.cb);
    Queued q = {.handle=handle, .incarnation=copy_of(handle)->owner->info.incarnation};
    // The real submit path retains finished copies before publishing its job.
    atomic_fetch_add_explicit(&copy_of(handle)->refs, 1, memory_order_relaxed);
    pthread_t worker; CHECK(!pthread_create(&worker, NULL, queued_replay, &q));
    destroy(&source); destroy(&parent); collect();
    CHECK(dm_nvn_pin_has_serial(texture.serial));
    atomic_store_explicit(&q.proceed, 1, memory_order_release);
    CHECK(!pthread_join(worker, NULL)); collect();
    CHECK(!dm_nvn_pin_has_serial(texture.serial));
}
static void addressed_scene_calls(void) {
    Recorder child, parent; init(&child); init(&parent);
    NativeSceneAddress a = {0x1000, 7, 0x4000, 1, 2};
    NativeSceneAddress b = {0x2000, 8, 0x4000, 1, 2};
    h_nvnCommandBufferBeginRecording(&child.cb);
    NativeSceneInvocation outer = nvn_scene_begin(&child.cb, a);
    size_t first = recording_meta(CB(&child.cb))->logical_bytes;
    h_nvnCommandBufferSetScissor(&child.cb, 0, 0, 16, 16);
    NativeSceneInvocation nested = nvn_scene_begin(&child.cb, b);
    size_t second = recording_meta(CB(&child.cb))->logical_bytes;
    h_nvnCommandBufferSetScissor(&child.cb, 1, 1, 16, 16);
    nvn_scene_end(&child.cb, nested);
    size_t third = recording_meta(CB(&child.cb))->logical_bytes;
    h_nvnCommandBufferSetScissor(&child.cb, 2, 2, 16, 16);
    nvn_scene_end(&child.cb, outer);
    size_t outside = recording_meta(CB(&child.cb))->logical_bytes;
    h_nvnCommandBufferSetScissor(&child.cb, 3, 3, 16, 16);
    NVNcommandHandle hc = h_nvnCommandBufferEndRecording(&child.cb);
    const NvnCommandOwner *co = nvn_command_owner_acquire(hc);
    CHECK(nvn_command_scene_at(co, first).owner == a.owner);
    CHECK(nvn_command_scene_at(co, second).owner == b.owner);
    CHECK(nvn_command_scene_at(co, third).owner == a.owner);
    CHECK(!nvn_command_scene_at(co, outside).owner);
    CHECK(nvn_command_owner_info(co)->has_scene);
    h_nvnCommandBufferBeginRecording(&parent.cb);
    h_nvnCommandBufferCallCommands(&parent.cb, 1, &hc);
    h_nvnCommandBufferCallCommands(&parent.cb, 1, &hc);
    NVNcommandHandle hp = h_nvnCommandBufferEndRecording(&parent.cb);
    const NvnCommandOwner *po = nvn_command_owner_acquire(hp);
    const NvnCommandOwnerInfo *pi = nvn_command_owner_info(po);
    CHECK(pi->has_scene && pi->scope_count == 2);
    for (size_t i = 0; i < 2; ++i) {
        size_t origin = pi->scopes[i].offset - 8;
        CHECK(nvn_command_scene_at(po, origin + first).owner == a.owner);
        CHECK(nvn_command_scene_at(po, origin + second).owner == b.owner);
        CHECK(nvn_command_scene_at(po, origin + third).generation == a.generation);
        CHECK(!nvn_command_scene_at(po, origin + outside).owner);
    }
    // Reusing the recorder cannot alter a queued invocation's incarnation.
    h_nvnCommandBufferBeginRecording(&child.cb);
    a.generation++;
    outer = nvn_scene_begin(&child.cb, a);
    h_nvnCommandBufferSetScissor(&child.cb, 0, 0, 16, 16);
    nvn_scene_end(&child.cb, outer);
    NVNcommandHandle fresh = h_nvnCommandBufferEndRecording(&child.cb);
    CHECK(nvn_command_scene_at(nvn_command_owner_borrow(fresh), first).generation == a.generation);
    CHECK(nvn_command_scene_at(po, first).generation == a.generation - 1);
    h_nvnCommandBufferBeginRecording(&child.cb);
    NativeSceneInvocation abandoned = nvn_scene_begin(&child.cb, a);
    h_nvnCommandBufferBeginRecording(&child.cb);
    NativeSceneInvocation replacement = nvn_scene_begin(&child.cb, b);
    CHECK(abandoned.index == replacement.index && abandoned.recording != replacement.recording);
    CHECK(!nvn_scene_end(&child.cb, abandoned));
    CHECK(recording_meta(CB(&child.cb))->scene_top == replacement.index + 1);
    CHECK(nvn_scene_end(&child.cb, replacement));
    (void)h_nvnCommandBufferEndRecording(&child.cb);
    destroy(&child); destroy(&parent);
    CHECK(nvn_command_scene_at(po, second).owner == b.owner);
    nvn_command_owner_release(co); nvn_command_owner_release(po);
}
int main(void) {
    uint64_t copies = g_copy_bytes, owners = g_owner_bytes; size_t count = g_ncopies;
    shared_and_nested(); deferred_is_local(); empty_calls(); pending_texture_lifetime(); copied_texture_lifetime(); abort_and_deep_owners(); asynchronous_retirement(); addressed_scene_calls();
    dm_nvn_probe_commands(); dm_nvn_probe_inline_bulk(); dm_nvn_probe_texture_pins();
    CHECK(g_copy_bytes == copies && g_owner_bytes == owners && g_ncopies == count);
    printf("{\"status\":\"pass\",\"probe\":\"command-owner\",\"checks\":%u,\"deep_owner_levels\":4096,\"copied_child_bytes_avoided\":%zu,\"compact_recording_bytes\":%zu,\"copied_recording_bytes\":%zu}\n",
        checks, avoided_body_bytes, compact_recording_bytes, copied_recording_bytes);
}
