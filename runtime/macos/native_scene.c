#include "runtime.h"
#include "native_cache_register.h"
#include "native_scene.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Buckets bound lookup work, not the number of owners. Nodes remain addressable
// until process teardown; destroying and reusing a guest address changes its
// incarnation, without invalidating metadata in in-flight command recordings.
enum { OWNER_BUCKETS = 16384 };
typedef struct SceneInstance {
    uintptr_t address;
    _Atomic uint64_t generation;
    struct SceneInstance *next;
} SceneInstance;
static _Atomic(SceneInstance *) instances[OWNER_BUCKETS];
static _Atomic uint64_t next_generation;

static size_t instance_bucket(uintptr_t address) {
    uint64_t x = address >> 4;
    x ^= x >> 33; x *= UINT64_C(0xff51afd7ed558ccd); x ^= x >> 33;
    return x & (OWNER_BUCKETS - 1);
}
static SceneInstance *find_instance(uintptr_t address) {
    SceneInstance *p = atomic_load_explicit(&instances[instance_bucket(address)], memory_order_acquire);
    for (; p; p = p->next) if (p->address == address) return p;
    return NULL;
}
static SceneInstance *enter_instance(uintptr_t address) {
    size_t bucket = instance_bucket(address);
    SceneInstance *p = find_instance(address);
    if (!p) {
        SceneInstance *fresh = calloc(1, sizeof *fresh);
        if (!fresh) runtime_fail("scene owner allocation");
        fresh->address = address;
        SceneInstance *head = atomic_load_explicit(&instances[bucket], memory_order_acquire);
        for (;;) {
            for (p = head; p; p = p->next) if (p->address == address) break;
            if (p) { free(fresh); break; }
            fresh->next = head;
            if (atomic_compare_exchange_weak_explicit(&instances[bucket], &head, fresh,
                    memory_order_release, memory_order_acquire)) { p = fresh; break; }
        }
    }
    uint64_t generation = atomic_load_explicit(&p->generation, memory_order_acquire);
    if (!generation) {
        uint64_t fresh = atomic_fetch_add_explicit(&next_generation, 1, memory_order_relaxed) + 1;
        if (!fresh) runtime_fail("scene owner generation overflow");
        atomic_compare_exchange_strong_explicit(&p->generation, &generation, fresh,
            memory_order_release, memory_order_acquire);
    }
    return p;
}

#if defined(TOTK_GAME_V121)
static uintptr_t main_base;
static void (*object_destroy)(void *);

static void scene_draw(void *object, void *context, uint32_t role, uint32_t variant) {
    SceneInstance *instance = enter_instance((uintptr_t)object);
    void (*draw)(void *, void *, uint32_t, uint32_t) = *(void **)(*(uint8_t **)object + 0x60);
    // The render dispatcher's DrawContext owns the command buffer. Keep
    // that address within the invocation; it is never retained as scene data.
    uint8_t *device_context = *(uint8_t **)((uint8_t *)context + 0x20);
    void *recorder = *(void **)(device_context + 0xb8);
    NativeSceneAddress address = {(uintptr_t)object,
        atomic_load_explicit(&instance->generation, memory_order_acquire), (uintptr_t)draw - main_base, role, variant};
    NativeSceneInvocation token = nvn_scene_begin(recorder, address);
    draw(object, context, role, variant);
    nvn_scene_end(recorder, token);
}

static uint64_t scene_collect(void *object, uintptr_t view, uintptr_t mode, uintptr_t role,
    void *camera, void *visibility, void *parameters, void *geometry_culling) {
    // Establish the incarnation before this owner's culling/collection action.
    // Its actual virtual implementation and behavior callbacks remain in charge
    // of visibility. A collected object is not assumed static or closed.
    (void)enter_instance((uintptr_t)object);
    uint64_t (*collect)(void *, uintptr_t, uintptr_t, uintptr_t, void *, void *, void *, void *) =
        *(void **)(*(uint8_t **)object + 0x58);
    return collect(object, view, mode, role, camera, visibility, parameters, geometry_culling);
}

static void scene_destroy(void *object) {
    SceneInstance *instance = find_instance((uintptr_t)object);
    if (instance) atomic_store_explicit(&instance->generation, 0, memory_order_release);
    object_destroy(object);
}

void native_scene_install(Module *m) {
    const char *enabled = getenv("TOTK_PERIODIC_SCENE");
    if (!enabled || strcmp(enabled, "1")) return;
    if (!m || !m->base || m->text_end < 4 || m->text_end > m->file_size)
        runtime_fail("scene adapter image bounds");
    const struct { size_t site; uint32_t word; void *target; const char *name; int call; } hooks[] = {
        {0x12d7560, 0xd63f0100, scene_draw, "scene-object-draw", 1},
        {0x9829f8, 0xd63f0100, scene_collect, "scene-object-collect", 1},
        {0x10ca65c, 0xd63f0100, scene_collect, "scene-object-collect", 1},
        {0xdc7480, 0xa9bc7bfd, scene_destroy, "scene-object-destroy", 0},
    };
    size_t first = ((m->text_end + 15) & ~(size_t)15) + 256;
    size_t end = (m->text_end + 0x3fff) & ~(size_t)0x3fff;
    if (end > m->ro_off) end = m->ro_off;
    if (end > m->file_size) end = m->file_size;
    if (first > end || end - first < sizeof hooks / sizeof *hooks * 32)
        runtime_fail("scene adapters have no executable padding");
    enum { COUNT = sizeof hooks / sizeof *hooks };
    uint32_t branches[COUNT], returns[COUNT];
    for (size_t i = 0; i < COUNT; ++i) {
        uint32_t word;
        if (hooks[i].site > m->text_end - 4) runtime_fail("scene adapter outside text");
        memcpy(&word, m->base + hooks[i].site, 4);
        size_t stub = first + i * 32;
        if (word != hooks[i].word || !native_ctr_branch(hooks[i].site, stub, &branches[i]) ||
            !native_ctr_branch(stub + 20, hooks[i].site + 4, &returns[i]))
            runtime_fail("scene adapter signature or branch range");
        for (size_t j = 0; j < 24; ++j) if (m->base[stub + j])
            runtime_fail("scene adapter padding occupied");
    }
    main_base = (uintptr_t)m->base;
    for (size_t i = 0; i < COUNT; ++i) {
        uintptr_t host = (uintptr_t)runtime_hle_wrap_named(hooks[i].target, hooks[i].name);
        size_t stub = first + i * 32;
        const uint32_t relay[] = {0x58000050, 0xd61f0200};
        memcpy(m->base + stub, relay, 8); memcpy(m->base + stub + 8, &host, 8);
        if (hooks[i].call) branches[i] |= UINT32_C(0x80000000); // BL preserves the original call's return site.
        else {
            memcpy(m->base + stub + 16, &hooks[i].word, 4);
            memcpy(m->base + stub + 20, &returns[i], 4);
            object_destroy = (void *)(m->base + stub + 16);
        }
        memcpy(m->base + hooks[i].site, &branches[i], 4);
    }
    fprintf(stderr, "[periodic-scene] render-object dispatch, collection and base lifetime connected\n");
}
#else
void native_scene_install(Module *m) { (void)m; }
#endif
