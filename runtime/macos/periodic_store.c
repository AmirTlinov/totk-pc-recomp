#include "periodic_store.h"
#include "periodic_form.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    STORE_RING_CAPACITY = 1024,
    STORE_PASS_LIMIT = 512,
    STORE_USE_LIMIT = 128,
    STORE_USE_BUCKETS = 256,
    STORE_RESOURCE_LIMIT = 4096,
    STORE_RESOURCE_BUCKETS = 8192,
    STORE_PROMOTION_CAPACITY = 32768,
    STORE_PROMOTION_PROBES = 16,
    STORE_PROMOTION_RETURNS = 3,
    STORE_PROMOTION_MAX_BYTES = 64 * 1024 * 1024,
    STORE_GRAPH_WORDS = (STORE_PASS_LIMIT + 63) / 64,
    STORE_SNAPSHOTS = 3,
};
enum { STORE_RECORD_PASS, STORE_RECORD_SUBMIT };
enum { PROMOTION_EMPTY, PROMOTION_WATCHING, PROMOTION_STABLE, PROMOTION_REJECTED };

typedef struct {
    PeriodicResourceKey key;
    uint64_t generation;
    uint32_t role;
    uint8_t access;
} StoreUse;

typedef struct {
    uint32_t kind;
    uint16_t use_count;
    uint8_t unknown;
    uint64_t id;
    uint64_t loss_epoch;
    StoreUse uses[STORE_USE_LIMIT];
} StoreRecord;

typedef struct {
    PeriodicResourceKey key;
    uint64_t generation;
    uint64_t readers[STORE_GRAPH_WORDS];
    uint64_t writers[STORE_GRAPH_WORDS];
    int32_t last_writer;
    uint32_t next, owner_next;
} GraphResource;

typedef struct {
    _Atomic uint32_t readers;
    uint64_t generation;
    uint64_t submission_id;
    uint64_t shape_hash;
    uint64_t invalidated[STORE_GRAPH_WORDS];
    uint64_t edges[STORE_PASS_LIMIT][STORE_GRAPH_WORDS];
    uint64_t pass_ids[STORE_PASS_LIMIT];
    uint64_t unknown[STORE_GRAPH_WORDS];
    GraphResource resources[STORE_RESOURCE_LIMIT];
    uint32_t buckets[STORE_RESOURCE_BUCKETS];
    uint32_t owner_buckets[STORE_RESOURCE_BUCKETS];
    uint32_t pass_count;
    uint32_t resource_count;
    uint32_t valid;
} GraphSnapshot;

typedef struct {
    _Atomic uint32_t state;
    PeriodicResourceKey key;
    uint64_t stamp;
    _Atomic uint64_t checked_epoch;
    uint64_t last_submission;
    uint32_t returns;
} GuestPromotion;

static struct {
    StoreRecord *records;
    StoreRecord *compile_passes;
    GraphSnapshot *snapshots;
    GuestPromotion *promotions;
    _Alignas(64) _Atomic uint64_t read;
    _Alignas(64) _Atomic uint64_t write;
    _Atomic uint64_t processed;
    uint64_t producer_write, producer_read_cache;
    StoreRecord *open;
    int16_t use_buckets[STORE_USE_BUCKETS];
    _Atomic uint64_t observed_passes, observed_submissions, dropped_passes, unknown_passes;
    _Atomic uint64_t loss_epoch;
    _Atomic uint64_t same_shape_submissions, changed_resources, invalidated_passes;
    _Atomic uint64_t graph_generation;
    _Atomic uint64_t promoted_guest_ranges, rejected_guest_ranges;
    _Atomic(PeriodicGuestProtectFn) protect_guest;
    _Atomic(PeriodicGuestChangedFn) guest_changed;
    _Atomic(PeriodicGuestEpochFn) guest_epoch;
    _Atomic int published;
    _Atomic int active, stopping;
    pthread_t compiler;
} store;

static pthread_once_t store_once = PTHREAD_ONCE_INIT;

static uint64_t hash_bytes(uint64_t hash, const void *data, size_t size) {
    const unsigned char *p = data;
    for (size_t i = 0; i < size; ++i) {
        hash ^= p[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t mix64(uint64_t value) {
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

static uint64_t key_hash(const PeriodicResourceKey *key) {
    uint64_t shape = (uint64_t)key->kind << 32 | key->plane;
    return mix64(key->owner) ^ mix64(key->offset + UINT64_C(0x9e3779b97f4a7c15)) ^
           mix64(key->length + UINT64_C(0x243f6a8885a308d3)) ^ mix64(shape);
}

static int key_equal(const PeriodicResourceKey *a, const PeriodicResourceKey *b) {
    return !memcmp(a, b, sizeof *a);
}

static uint64_t owner_hash(const PeriodicResourceKey *key) {
    uint64_t shape = (uint64_t)key->kind << 32 | key->plane;
    return mix64(key->owner) ^ mix64(shape);
}

static int key_overlaps(const PeriodicResourceKey *a, const PeriodicResourceKey *b) {
    if (a->owner != b->owner || a->kind != b->kind || a->plane != b->plane) return 0;
    uint64_t al = a->length ? a->length : 1, bl = b->length ? b->length : 1;
    return a->offset <= b->offset ? al > b->offset - a->offset : bl > a->offset - b->offset;
}

static GuestPromotion *promotion_find(const PeriodicResourceKey *key, int create) {
    unsigned bucket = (unsigned)(key_hash(key) & (STORE_PROMOTION_CAPACITY - 1));
    for (unsigned probe = 0; probe < STORE_PROMOTION_PROBES; ++probe) {
        GuestPromotion *promotion = &store.promotions[bucket];
        uint32_t state = atomic_load_explicit(&promotion->state, memory_order_acquire);
        if (state == PROMOTION_EMPTY) {
            if (!create) return NULL;
            promotion->key = *key;
            promotion->stamp = 0;
            promotion->last_submission = 0;
            promotion->returns = 0;
            atomic_store_explicit(&promotion->checked_epoch, 0, memory_order_relaxed);
            atomic_store_explicit(&promotion->state, PROMOTION_WATCHING, memory_order_release);
            return promotion;
        }
        if (key_equal(&promotion->key, key)) return promotion;
        bucket = (bucket + 1) & (STORE_PROMOTION_CAPACITY - 1);
    }
    return NULL;
}

static void bit_set(uint64_t bits[STORE_GRAPH_WORDS], unsigned index) {
    bits[index >> 6] |= UINT64_C(1) << (index & 63);
}

static unsigned bit_count(const uint64_t bits[STORE_GRAPH_WORDS]) {
    unsigned count = 0;
    for (unsigned i = 0; i < STORE_GRAPH_WORDS; ++i) count += (unsigned)__builtin_popcountll(bits[i]);
    return count;
}

static StoreRecord *reserve_record(uint64_t *position) {
    uint64_t write = store.producer_write;
    uint64_t read = store.producer_read_cache;
    if (write - read == STORE_RING_CAPACITY) {
        read = atomic_load_explicit(&store.read, memory_order_acquire);
        store.producer_read_cache = read;
    }
    if (write - read == STORE_RING_CAPACITY) return NULL;
    *position = write;
    return &store.records[write % STORE_RING_CAPACITY];
}

static void publish_record(uint64_t position) {
    store.producer_write = position + 1;
    atomic_store_explicit(&store.write, store.producer_write, memory_order_release);
}

static GraphResource *graph_resource(GraphSnapshot *graph, const PeriodicResourceKey *key, int create) {
    unsigned bucket = (unsigned)(key_hash(key) % STORE_RESOURCE_BUCKETS);
    for (uint32_t entry = graph->buckets[bucket]; entry; entry = graph->resources[entry - 1].next) {
        GraphResource *resource = &graph->resources[entry - 1];
        if (key_equal(&resource->key, key)) return resource;
    }
    if (!create || graph->resource_count == STORE_RESOURCE_LIMIT) return NULL;
    GraphResource *resource = &graph->resources[graph->resource_count++];
    memset(resource, 0, sizeof *resource);
    resource->key = *key;
    resource->last_writer = -1;
    resource->next = graph->buckets[bucket];
    graph->buckets[bucket] = graph->resource_count;
    unsigned owner_bucket = (unsigned)(owner_hash(key) % STORE_RESOURCE_BUCKETS);
    resource->owner_next = graph->owner_buckets[owner_bucket];
    graph->owner_buckets[owner_bucket] = graph->resource_count;
    return resource;
}

static const GraphResource *find_resource(const GraphSnapshot *graph, const PeriodicResourceKey *key) {
    unsigned bucket = (unsigned)(key_hash(key) % STORE_RESOURCE_BUCKETS);
    for (uint32_t entry = graph->buckets[bucket]; entry; entry = graph->resources[entry - 1].next) {
        const GraphResource *resource = &graph->resources[entry - 1];
        if (key_equal(&resource->key, key)) return resource;
    }
    return NULL;
}

static void add_edge(GraphSnapshot *graph, unsigned from, unsigned to) {
    if (from < graph->pass_count && to < graph->pass_count && from != to) bit_set(graph->edges[from], to);
}

static int graph_use(GraphSnapshot *graph, unsigned pass, const StoreUse *use) {
    unsigned owner_bucket = (unsigned)(owner_hash(&use->key) % STORE_RESOURCE_BUCKETS);
    for (uint32_t entry = graph->owner_buckets[owner_bucket]; entry;
         entry = graph->resources[entry - 1].owner_next) {
        GraphResource *resource = &graph->resources[entry - 1];
        if (!key_overlaps(&resource->key, &use->key)) continue;
        if (resource->last_writer >= 0) add_edge(graph, (unsigned)resource->last_writer, pass);
        if (use->access & PERIODIC_RESOURCE_READ) bit_set(resource->readers, pass);
        if (use->access & PERIODIC_RESOURCE_WRITE) {
            bit_set(resource->writers, pass);
            resource->last_writer = (int32_t)pass;
        }
    }
    GraphResource *resource = graph_resource(graph, &use->key, 1);
    if (!resource) return 0;
    if (use->generation > resource->generation) resource->generation = use->generation;
    if (use->access & PERIODIC_RESOURCE_READ) bit_set(resource->readers, pass);
    if (use->access & PERIODIC_RESOURCE_WRITE) {
        bit_set(resource->writers, pass);
        resource->last_writer = (int32_t)pass;
    }
    return 1;
}

static void expand_cone(const GraphSnapshot *graph, uint64_t bits[STORE_GRAPH_WORDS]) {
    unsigned queue[STORE_PASS_LIMIT], head = 0, tail = 0;
    for (unsigned i = 0; i < graph->pass_count; ++i)
        if (bits[i >> 6] & (UINT64_C(1) << (i & 63))) queue[tail++] = i;
    while (head < tail) {
        unsigned pass = queue[head++];
        for (unsigned word = 0; word < STORE_GRAPH_WORDS; ++word) {
            uint64_t fresh = graph->edges[pass][word] & ~bits[word];
            bits[word] |= fresh;
            while (fresh) {
                unsigned bit = (unsigned)__builtin_ctzll(fresh);
                unsigned next = word * 64 + bit;
                if (next < graph->pass_count) queue[tail++] = next;
                fresh &= fresh - 1;
            }
        }
    }
}

static int resource_has_overlapping_writer(const GraphSnapshot *graph, const GraphResource *candidate) {
    unsigned bucket = (unsigned)(owner_hash(&candidate->key) % STORE_RESOURCE_BUCKETS);
    for (uint32_t entry = graph->owner_buckets[bucket]; entry;
         entry = graph->resources[entry - 1].owner_next) {
        const GraphResource *resource = &graph->resources[entry - 1];
        if (key_overlaps(&candidate->key, &resource->key) && bit_count(resource->writers)) return 1;
    }
    return 0;
}

static void update_guest_promotions(GraphSnapshot *graph) {
    PeriodicGuestProtectFn protect = atomic_load_explicit(&store.protect_guest, memory_order_acquire);
    if (!protect) return;
    for (unsigned i = 0; i < graph->resource_count; ++i) {
        GraphResource *resource = &graph->resources[i];
        if (resource->key.kind != 2 || !bit_count(resource->readers) ||
            resource->key.length > STORE_PROMOTION_MAX_BYTES) continue;
        GuestPromotion *promotion = promotion_find(&resource->key, 1);
        if (!promotion) continue;
        uint32_t state = atomic_load_explicit(&promotion->state, memory_order_acquire);
        if (state == PROMOTION_REJECTED) continue;
        if (resource_has_overlapping_writer(graph, resource)) {
            if (state == PROMOTION_STABLE || state == PROMOTION_WATCHING) {
                atomic_store_explicit(&promotion->state, PROMOTION_REJECTED, memory_order_release);
                atomic_fetch_add_explicit(&store.rejected_guest_ranges, 1, memory_order_relaxed);
            }
            continue;
        }
        if (state == PROMOTION_STABLE) continue;
        if (promotion->last_submission + 1 == graph->submission_id) promotion->returns++;
        else promotion->returns = 1;
        promotion->last_submission = graph->submission_id;
        if (promotion->returns < STORE_PROMOTION_RETURNS) continue;
        uint64_t stamp = protect(resource->key.offset, resource->key.length);
        if (!stamp) {
            atomic_store_explicit(&promotion->state, PROMOTION_REJECTED, memory_order_release);
            atomic_fetch_add_explicit(&store.rejected_guest_ranges, 1, memory_order_relaxed);
            continue;
        }
        promotion->stamp = stamp;
        atomic_store_explicit(&promotion->checked_epoch, stamp, memory_order_relaxed);
        atomic_store_explicit(&promotion->state, PROMOTION_STABLE, memory_order_release);
        atomic_fetch_add_explicit(&store.promoted_guest_ranges, 1, memory_order_relaxed);
    }
}

static int writable_snapshot(void) {
    int published = atomic_load_explicit(&store.published, memory_order_acquire);
    for (int step = 1; step <= STORE_SNAPSHOTS; ++step) {
        int index = (published + step) % STORE_SNAPSHOTS;
        if (index != published && !atomic_load_explicit(&store.snapshots[index].readers, memory_order_acquire)) return index;
    }
    return -1;
}

static void compile_submission(size_t count, uint64_t submission_id, int invalid) {
    int index;
    while ((index = writable_snapshot()) < 0) {
        struct timespec pause = {.tv_nsec = 100000};
        nanosleep(&pause, NULL);
    }
    GraphSnapshot *graph = &store.snapshots[index];
    uint32_t held_readers = atomic_load_explicit(&graph->readers, memory_order_relaxed);
    memset((char *)graph + sizeof graph->readers, 0, sizeof *graph - sizeof graph->readers);
    atomic_store_explicit(&graph->readers, held_readers, memory_order_relaxed);
    graph->submission_id = submission_id;
    graph->pass_count = (uint32_t)(count > STORE_PASS_LIMIT ? STORE_PASS_LIMIT : count);
    graph->valid = !invalid;
    graph->shape_hash = UINT64_C(1469598103934665603);
    int last_unknown = -1;
    for (unsigned pass = 0; pass < graph->pass_count; ++pass) {
        const StoreRecord *record = &store.compile_passes[pass];
        graph->pass_ids[pass] = record->id;
        graph->shape_hash = hash_bytes(graph->shape_hash, &record->unknown, sizeof record->unknown);
        graph->shape_hash = hash_bytes(graph->shape_hash, &record->use_count, sizeof record->use_count);
        if (last_unknown >= 0) add_edge(graph, (unsigned)last_unknown, pass);
        if (record->unknown) {
            bit_set(graph->unknown, pass);
            for (unsigned prior = 0; prior < pass; ++prior) add_edge(graph, prior, pass);
            last_unknown = (int)pass;
        }
        for (unsigned i = 0; i < record->use_count; ++i) {
            const StoreUse *use = &record->uses[i];
            uint64_t shape = (uint64_t)use->role | ((uint64_t)use->access << 32);
            graph->shape_hash = hash_bytes(graph->shape_hash, &shape, sizeof shape);
            if (!graph_use(graph, pass, use)) {
                bit_set(graph->unknown, pass);
                for (unsigned prior = 0; prior < pass; ++prior) add_edge(graph, prior, pass);
                last_unknown = (int)pass;
                continue;
            }
        }
    }
    if (invalid) {
        for (unsigned pass = 0; pass < graph->pass_count; ++pass) {
            bit_set(graph->unknown, pass);
            for (unsigned prior = 0; prior < pass; ++prior) add_edge(graph, prior, pass);
        }
    }
    update_guest_promotions(graph);

    int previous_index = atomic_load_explicit(&store.published, memory_order_acquire);
    const GraphSnapshot *previous = &store.snapshots[previous_index];
    int same_shape = graph->valid && previous->valid && previous->generation &&
        previous->shape_hash == graph->shape_hash && previous->pass_count == graph->pass_count;
    if (same_shape)
        atomic_fetch_add_explicit(&store.same_shape_submissions, 1, memory_order_relaxed);
    uint64_t changed = 0;
    for (unsigned i = 0; i < graph->resource_count; ++i) {
        GraphResource *resource = &graph->resources[i];
        const GraphResource *old = previous->generation ? find_resource(previous, &resource->key) : NULL;
        if (old && old->generation == resource->generation) continue;
        changed++;
        for (unsigned word = 0; word < STORE_GRAPH_WORDS; ++word) {
            graph->invalidated[word] |= resource->readers[word];
            graph->invalidated[word] |= resource->writers[word];
        }
    }
    if (previous->generation && !same_shape)
        for (unsigned pass = 0; pass < graph->pass_count; ++pass) bit_set(graph->invalidated, pass);
    for (unsigned word = 0; word < STORE_GRAPH_WORDS; ++word) graph->invalidated[word] |= graph->unknown[word];
    expand_cone(graph, graph->invalidated);
    uint64_t generation = atomic_fetch_add_explicit(&store.graph_generation, 1, memory_order_relaxed) + 1;
    graph->generation = generation;
    atomic_fetch_add_explicit(&store.changed_resources, changed, memory_order_relaxed);
    atomic_fetch_add_explicit(&store.invalidated_passes, bit_count(graph->invalidated), memory_order_relaxed);
    atomic_store_explicit(&store.published, index, memory_order_release);
}

static void *compile_graphs(void *unused) {
    (void)unused;
    size_t pass_count = 0;
    uint64_t seen_loss = 0;
    int submission_invalid = 0;
    for (;;) {
        uint64_t read = atomic_load_explicit(&store.read, memory_order_relaxed);
        uint64_t write = atomic_load_explicit(&store.write, memory_order_acquire);
        if (read == write) {
            if (atomic_load_explicit(&store.stopping, memory_order_relaxed)) break;
            struct timespec pause = {.tv_nsec = 1000000};
            nanosleep(&pause, NULL);
            continue;
        }
        const StoreRecord *record = &store.records[read % STORE_RING_CAPACITY];
        if (record->loss_epoch != seen_loss) {
            seen_loss = record->loss_epoch;
            submission_invalid = 1;
        }
        if (record->kind == STORE_RECORD_SUBMIT) {
            compile_submission(pass_count, record->id, submission_invalid);
            pass_count = 0;
            submission_invalid = 0;
        } else if (pass_count < STORE_PASS_LIMIT) {
            store.compile_passes[pass_count++] = *record;
        } else {
            atomic_fetch_add_explicit(&store.dropped_passes, 1, memory_order_relaxed);
            submission_invalid = 1;
        }
        atomic_store_explicit(&store.read, read + 1, memory_order_release);
        atomic_store_explicit(&store.processed, read + 1, memory_order_release);
    }
    return NULL;
}

static void initialize_store(void) {
    const char *enabled = getenv("TOTK_PERIODIC_FORMS");
    if (enabled && !strcmp(enabled, "0")) return;
    store.records = calloc(STORE_RING_CAPACITY, sizeof *store.records);
    store.compile_passes = calloc(STORE_PASS_LIMIT, sizeof *store.compile_passes);
    store.snapshots = calloc(STORE_SNAPSHOTS, sizeof *store.snapshots);
    store.promotions = calloc(STORE_PROMOTION_CAPACITY, sizeof *store.promotions);
    if (!store.records || !store.compile_passes || !store.snapshots || !store.promotions) goto fail;
    atomic_store_explicit(&store.published, 0, memory_order_relaxed);
    if (pthread_create(&store.compiler, NULL, compile_graphs, NULL)) goto fail;
    atomic_store_explicit(&store.active, 1, memory_order_release);
    atexit(periodic_store_finish);
    return;
fail:
    free(store.records); free(store.compile_passes); free(store.snapshots); free(store.promotions);
    store.records = NULL; store.compile_passes = NULL; store.snapshots = NULL; store.promotions = NULL;
}

void periodic_store_init(void) { pthread_once(&store_once, initialize_store); }
int periodic_store_active(void) { return atomic_load_explicit(&store.active, memory_order_relaxed); }

void periodic_store_set_guest_tracker(PeriodicGuestProtectFn protect_range,
                                      PeriodicGuestChangedFn range_changed,
                                      PeriodicGuestEpochFn current_epoch) {
    atomic_store_explicit(&store.guest_epoch, current_epoch, memory_order_release);
    atomic_store_explicit(&store.guest_changed, range_changed, memory_order_release);
    atomic_store_explicit(&store.protect_guest, protect_range, memory_order_release);
}

uint64_t periodic_store_guest_generation(PeriodicResourceKey key, uint64_t fallback_generation) {
    if (!periodic_store_active() || key.kind != 2 || !store.promotions) return fallback_generation;
    GuestPromotion *promotion = promotion_find(&key, 0);
    if (!promotion || atomic_load_explicit(&promotion->state, memory_order_acquire) != PROMOTION_STABLE)
        return fallback_generation;
    PeriodicGuestEpochFn epoch = atomic_load_explicit(&store.guest_epoch, memory_order_acquire);
    if (!epoch) return fallback_generation;
    uint64_t dirty_epoch = epoch();
    uint64_t checked = atomic_load_explicit(&promotion->checked_epoch, memory_order_relaxed);
    if (checked == dirty_epoch) return 1;
    PeriodicGuestChangedFn changed = atomic_load_explicit(&store.guest_changed, memory_order_acquire);
    if (!changed || changed(key.offset, key.length, promotion->stamp)) {
        uint32_t stable = PROMOTION_STABLE;
        if (atomic_compare_exchange_strong_explicit(&promotion->state, &stable, PROMOTION_REJECTED,
                                                    memory_order_acq_rel, memory_order_acquire))
            atomic_fetch_add_explicit(&store.rejected_guest_ranges, 1, memory_order_relaxed);
        return fallback_generation;
    }
    atomic_store_explicit(&promotion->checked_epoch, dirty_epoch, memory_order_relaxed);
    return 1;
}

void periodic_store_pass_begin(uint64_t pass_id) {
    if (!periodic_store_active()) return;
    if (store.open) {
        store.open->unknown = 1;
        periodic_store_pass_end();
    }
    uint64_t position;
    StoreRecord *record = reserve_record(&position);
    if (!record) {
        atomic_fetch_add_explicit(&store.dropped_passes, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&store.loss_epoch, 1, memory_order_relaxed);
        return;
    }
    memset(record, 0, offsetof(StoreRecord, uses));
    record->kind = STORE_RECORD_PASS;
    record->id = pass_id;
    record->loss_epoch = atomic_load_explicit(&store.loss_epoch, memory_order_relaxed);
    store.open = record;
    memset(store.use_buckets, 0xff, sizeof store.use_buckets);
}

void periodic_store_resource(PeriodicResourceKey key, uint64_t generation,
                             uint32_t role, uint8_t access) {
    StoreRecord *record = store.open;
    if (!record || !key.owner || !(access & (PERIODIC_RESOURCE_READ | PERIODIC_RESOURCE_WRITE))) return;
    if (!key.length) key.length = 1;
    int range_resource = key.kind == 2 || key.kind == 4;
    uint64_t use_hash = range_resource ? owner_hash(&key) ^ mix64(role) : key_hash(&key);
    unsigned bucket = (unsigned)(use_hash & (STORE_USE_BUCKETS - 1));
    for (unsigned probes = 0; probes < STORE_USE_BUCKETS; ++probes) {
        int16_t entry = store.use_buckets[bucket];
        if (entry < 0) {
            if (record->use_count == STORE_USE_LIMIT) { record->unknown = 1; return; }
            unsigned index = record->use_count++;
            record->uses[index] = (StoreUse){.key=key, .generation=generation, .role=role, .access=access};
            store.use_buckets[bucket] = (int16_t)index;
            return;
        }
        StoreUse *use = &record->uses[(unsigned)entry];
        if (key_equal(&use->key, &key)) {
            use->access |= access;
            if (generation > use->generation) use->generation = generation;
            return;
        }
        if (range_resource && use->key.kind == key.kind && use->key.owner == key.owner &&
            use->key.plane == key.plane && use->role == role) {
            uint64_t begin = use->key.offset < key.offset ? use->key.offset : key.offset;
            uint64_t use_end = use->key.offset + use->key.length;
            uint64_t key_end = key.offset + key.length;
            uint64_t end = use_end > key_end ? use_end : key_end;
            if (end < begin) { record->unknown = 1; return; }
            use->key.offset = begin;
            use->key.length = end - begin;
            use->access |= access;
            if (generation > use->generation) use->generation = generation;
            return;
        }
        bucket = (bucket + 1) & (STORE_USE_BUCKETS - 1);
    }
    record->unknown = 1;
}

void periodic_store_unknown(void) { if (store.open) store.open->unknown = 1; }

void periodic_store_pass_end(void) {
    if (!store.open) return;
    uint64_t position = store.producer_write;
    if (store.open->unknown) atomic_fetch_add_explicit(&store.unknown_passes, 1, memory_order_relaxed);
    store.open = NULL;
    publish_record(position);
    atomic_fetch_add_explicit(&store.observed_passes, 1, memory_order_relaxed);
}

void periodic_store_submit(uint64_t submission_id) {
    if (!periodic_store_active()) return;
    if (store.open) periodic_store_pass_end();
    uint64_t position;
    StoreRecord *record = reserve_record(&position);
    if (!record) {
        atomic_fetch_add_explicit(&store.dropped_passes, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&store.loss_epoch, 1, memory_order_relaxed);
        return;
    }
    memset(record, 0, offsetof(StoreRecord, uses));
    record->kind = STORE_RECORD_SUBMIT;
    record->id = submission_id;
    record->loss_epoch = atomic_load_explicit(&store.loss_epoch, memory_order_relaxed);
    publish_record(position);
    atomic_fetch_add_explicit(&store.observed_submissions, 1, memory_order_relaxed);
}

void periodic_store_flush(void) {
    if (!periodic_store_active()) return;
    for (;;) {
        uint64_t processed = atomic_load_explicit(&store.processed, memory_order_acquire);
        uint64_t write = atomic_load_explicit(&store.write, memory_order_acquire);
        if (processed == write) return;
        struct timespec pause = {.tv_nsec = 100000};
        nanosleep(&pause, NULL);
    }
}

static const GraphSnapshot *acquire_graph(int *index_out) {
    for (;;) {
        int index = atomic_load_explicit(&store.published, memory_order_acquire);
        GraphSnapshot *graph = &store.snapshots[index];
        atomic_fetch_add_explicit(&graph->readers, 1, memory_order_acquire);
        if (index == atomic_load_explicit(&store.published, memory_order_acquire)) {
            *index_out = index;
            return graph;
        }
        atomic_fetch_sub_explicit(&graph->readers, 1, memory_order_release);
    }
}

static void release_graph(int index) {
    atomic_fetch_sub_explicit(&store.snapshots[index].readers, 1, memory_order_release);
}

PeriodicStoreStats periodic_store_stats(void) {
    periodic_store_flush();
    int index; const GraphSnapshot *graph = acquire_graph(&index);
    PeriodicStoreStats stats = {
        .observed_passes=atomic_load_explicit(&store.observed_passes, memory_order_relaxed),
        .observed_submissions=atomic_load_explicit(&store.observed_submissions, memory_order_relaxed),
        .dropped_passes=atomic_load_explicit(&store.dropped_passes, memory_order_relaxed),
        .unknown_passes=atomic_load_explicit(&store.unknown_passes, memory_order_relaxed),
        .same_shape_submissions=atomic_load_explicit(&store.same_shape_submissions, memory_order_relaxed),
        .changed_resources=atomic_load_explicit(&store.changed_resources, memory_order_relaxed),
        .invalidated_passes=atomic_load_explicit(&store.invalidated_passes, memory_order_relaxed),
        .promoted_guest_ranges=atomic_load_explicit(&store.promoted_guest_ranges, memory_order_relaxed),
        .rejected_guest_ranges=atomic_load_explicit(&store.rejected_guest_ranges, memory_order_relaxed),
        .graph_generation=atomic_load_explicit(&store.graph_generation, memory_order_relaxed),
        .pass_count=graph->pass_count,
        .resource_count=graph->resource_count,
        .graph_valid=graph->valid,
    };
    release_graph(index);
    return stats;
}

int periodic_store_execution_plan(PeriodicExecutionPlan *plan) {
    if (!plan || !periodic_store_active()) return 0;
    int index; const GraphSnapshot *graph = acquire_graph(&index);
    *plan = (PeriodicExecutionPlan) {
        .graph_generation=graph->generation,
        .submission_id=graph->submission_id,
        .pass_count=graph->pass_count,
        .graph_valid=graph->valid,
    };
    _Static_assert(STORE_GRAPH_WORDS == PERIODIC_EXECUTION_WORDS, "execution bitset ABI");
    _Static_assert(STORE_PASS_LIMIT == PERIODIC_EXECUTION_PASSES, "execution pass ABI");
    memcpy(plan->execute, graph->invalidated, sizeof plan->execute);
    for (unsigned pass = 0; pass < graph->pass_count; ++pass) {
        if (!periodic_form_for_pass(graph->pass_ids[pass], &plan->form_ids[pass], &plan->form_anchors[pass])) continue;
        bit_set(plan->retained, pass);
        plan->retained_pass_count++;
    }
    release_graph(index);
    return plan->graph_generation != 0;
}

size_t periodic_store_resource_cone(PeriodicResourceKey key, uint64_t *pass_ids, size_t capacity) {
    if (!periodic_store_active()) return 0;
    int index; const GraphSnapshot *graph = acquire_graph(&index);
    uint64_t bits[STORE_GRAPH_WORDS] = {0};
    unsigned owner_bucket = (unsigned)(owner_hash(&key) % STORE_RESOURCE_BUCKETS);
    for (uint32_t entry = graph->owner_buckets[owner_bucket]; entry;
         entry = graph->resources[entry - 1].owner_next) {
        const GraphResource *resource = &graph->resources[entry - 1];
        if (!key_overlaps(&resource->key, &key)) continue;
        for (unsigned word = 0; word < STORE_GRAPH_WORDS; ++word) {
            bits[word] |= resource->readers[word];
            bits[word] |= resource->writers[word];
        }
    }
    for (unsigned word = 0; word < STORE_GRAPH_WORDS; ++word) bits[word] |= graph->unknown[word];
    expand_cone(graph, bits);
    size_t count = 0;
    for (unsigned pass = 0; pass < graph->pass_count; ++pass) if (bits[pass >> 6] & (UINT64_C(1) << (pass & 63))) {
        if (count < capacity && pass_ids) pass_ids[count] = graph->pass_ids[pass];
        count++;
    }
    release_graph(index);
    return count;
}

void periodic_store_finish(void) {
    if (!periodic_store_active()) return;
    periodic_store_flush();
    if (!atomic_exchange_explicit(&store.active, 0, memory_order_acq_rel)) return;
    atomic_store_explicit(&store.stopping, 1, memory_order_release);
    pthread_join(store.compiler, NULL);
    if (atomic_load_explicit(&store.observed_submissions, memory_order_relaxed))
        fprintf(stderr, "[totk] periodic store: %llu submissions, %llu passes, %llu promoted guest ranges, %llu rejected, %llu graph generations\n",
                (unsigned long long)atomic_load_explicit(&store.observed_submissions, memory_order_relaxed),
                (unsigned long long)atomic_load_explicit(&store.observed_passes, memory_order_relaxed),
                (unsigned long long)atomic_load_explicit(&store.promoted_guest_ranges, memory_order_relaxed),
                (unsigned long long)atomic_load_explicit(&store.rejected_guest_ranges, memory_order_relaxed),
                (unsigned long long)atomic_load_explicit(&store.graph_generation, memory_order_relaxed));
    free(store.records); free(store.compile_passes); free(store.snapshots); free(store.promotions);
    store.records = NULL; store.compile_passes = NULL; store.snapshots = NULL; store.promotions = NULL;
}

static uint64_t probe_dirty_epoch = 10;
static int probe_range_dirty;
static uint64_t probe_epoch(void) { return probe_dirty_epoch; }
static uint64_t probe_protect(uint64_t address, uint64_t length) {
    return address && length ? probe_dirty_epoch : 0;
}
static int probe_changed(uint64_t address, uint64_t length, uint64_t stamp) {
    (void)address; (void)length; (void)stamp;
    return probe_range_dirty;
}

static PeriodicResourceKey probe_key(uint64_t owner) {
    return (PeriodicResourceKey){.owner=owner, .length=1, .kind=1};
}

int periodic_store_probe(void) {
    setenv("TOTK_PERIODIC_FORMS", "1", 1);
    periodic_store_init();
    if (!periodic_store_active()) return 1;
    periodic_store_set_guest_tracker(probe_protect, probe_changed, probe_epoch);
    PeriodicResourceKey a = probe_key(1), b = probe_key(2), c = probe_key(3);
    for (uint64_t submission = 1; submission <= 3; ++submission) {
        uint64_t generation = submission == 3 ? 2 : 1;
        periodic_store_pass_begin(10); periodic_store_resource(a, generation, 100, PERIODIC_RESOURCE_WRITE); periodic_store_pass_end();
        periodic_store_pass_begin(20); periodic_store_resource(a, generation, 100, PERIODIC_RESOURCE_READ);
        periodic_store_resource(b, 1, 200, PERIODIC_RESOURCE_WRITE); periodic_store_pass_end();
        periodic_store_pass_begin(30); periodic_store_resource(c, 1, 300, PERIODIC_RESOURCE_READ); periodic_store_pass_end();
        periodic_store_pass_begin(40); periodic_store_pass_end();
        periodic_store_submit(submission);
        periodic_store_flush();
    }
    uint64_t cone[8] = {0};
    size_t n = periodic_store_resource_cone(a, cone, 8);
    if (n != 2 || cone[0] != 10 || cone[1] != 20) return 1;
    n = periodic_store_resource_cone(c, cone, 8);
    if (n != 1 || cone[0] != 30) return 1;
    PeriodicResourceKey rotated = a; rotated.owner = 4;
    periodic_store_pass_begin(10); periodic_store_resource(rotated, 1, 100, PERIODIC_RESOURCE_WRITE); periodic_store_pass_end();
    periodic_store_pass_begin(20); periodic_store_resource(rotated, 1, 100, PERIODIC_RESOURCE_READ);
    periodic_store_resource(b, 1, 200, PERIODIC_RESOURCE_WRITE); periodic_store_pass_end();
    periodic_store_pass_begin(30); periodic_store_resource(c, 1, 300, PERIODIC_RESOURCE_READ); periodic_store_pass_end();
    periodic_store_pass_begin(40); periodic_store_pass_end(); periodic_store_submit(4); periodic_store_flush();
    PeriodicStoreStats stats = periodic_store_stats();
    if (stats.pass_count != 4 || stats.resource_count != 3 || !stats.graph_valid || stats.dropped_passes ||
        stats.observed_passes != 16 || stats.observed_submissions != 4 ||
        stats.same_shape_submissions != 3 || stats.graph_generation != 4) return 1;
    PeriodicResourceKey pool = {.owner=8, .offset=64, .length=128, .kind=2};
    PeriodicResourceKey overlap = {.owner=8, .offset=128, .length=64, .kind=2};
    PeriodicResourceKey disjoint = {.owner=8, .offset=256, .length=64, .kind=2};
    periodic_store_pass_begin(50); periodic_store_resource(pool, 1, 400, PERIODIC_RESOURCE_WRITE); periodic_store_pass_end();
    periodic_store_pass_begin(60); periodic_store_resource(overlap, 1, 401, PERIODIC_RESOURCE_READ); periodic_store_pass_end();
    periodic_store_pass_begin(70); periodic_store_resource(disjoint, 1, 402, PERIODIC_RESOURCE_READ); periodic_store_pass_end();
    periodic_store_submit(5); periodic_store_flush();
    n = periodic_store_resource_cone(pool, cone, 8);
    if (n != 2 || cone[0] != 50 || cone[1] != 60) return 1;
    n = periodic_store_resource_cone(disjoint, cone, 8);
    if (n != 1 || cone[0] != 70) return 1;
    PeriodicExecutionPlan plan;
    if (!periodic_store_execution_plan(&plan) || plan.submission_id != 5 || plan.pass_count != 3 || !plan.graph_valid)
        return 1;
    PeriodicResourceKey stable = {.owner=1, .offset=4096, .length=256, .kind=2};
    for (uint64_t submission = 6; submission <= 8; ++submission) {
        uint64_t generation = periodic_store_guest_generation(stable, submission);
        periodic_store_pass_begin(80); periodic_store_resource(stable, generation, 500, PERIODIC_RESOURCE_READ); periodic_store_pass_end();
        periodic_store_submit(submission); periodic_store_flush();
    }
    if (periodic_store_guest_generation(stable, 9) != 1) return 1;
    probe_dirty_epoch++;
    if (periodic_store_guest_generation(stable, 9) != 1) return 1;
    probe_range_dirty = 1; probe_dirty_epoch++;
    if (periodic_store_guest_generation(stable, 9) != 9) return 1;
    PeriodicStoreStats promoted = periodic_store_stats();
    if (promoted.promoted_guest_ranges != 1 || promoted.rejected_guest_ranges < 2) return 1;
    PeriodicResourceKey access_change = probe_key(90);
    periodic_store_pass_begin(90); periodic_store_resource(access_change, 1, 900, PERIODIC_RESOURCE_READ); periodic_store_pass_end();
    periodic_store_submit(10); periodic_store_flush();
    periodic_store_pass_begin(90); periodic_store_resource(access_change, 1, 900, PERIODIC_RESOURCE_WRITE); periodic_store_pass_end();
    periodic_store_submit(11); periodic_store_flush();
    if (!periodic_store_execution_plan(&plan) || !(plan.execute[0] & 1)) return 1;
    periodic_form_init();
    const uint64_t form_begin[8] = {1}, form_draw[8] = {3, 0, 12}, form_end[8] = {0};
    for (uint64_t anchor = 0; anchor < 3; ++anchor) {
        periodic_observe_packet(100, form_begin, 1, 0, 0, PERIODIC_PACKET_PASS_BEGIN, NULL, 9000 + anchor);
        periodic_observe_packet(101, form_draw, 5, 2, 0, PERIODIC_PACKET_DRAW, NULL, 0);
        periodic_observe_packet(102, form_end, 1, 0, 0, PERIODIC_PACKET_PASS_END, NULL, 0);
        periodic_anchor_publish(anchor);
    }
    periodic_form_flush();
    periodic_store_pass_begin(9002); periodic_store_resource(access_change, 1, 900, PERIODIC_RESOURCE_READ); periodic_store_pass_end();
    periodic_store_submit(12); periodic_store_flush();
    if (!periodic_store_execution_plan(&plan) || plan.retained_pass_count != 1 ||
        !(plan.retained[0] & 1) || plan.form_ids[0] != 1 || plan.form_anchors[0] != 2) return 1;
    enum { BENCH_NOTES = 4096, BENCH_ROUNDS = 9 };
    double samples[BENCH_ROUNDS];
    for (unsigned round = 0; round < BENCH_ROUNDS; ++round) {
        periodic_store_pass_begin(100 + round);
        uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        for (unsigned i = 0; i < BENCH_NOTES; ++i)
            periodic_store_resource(a, 2, 100, PERIODIC_RESOURCE_READ);
        samples[round] = (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start) / (double)BENCH_NOTES;
        periodic_store_pass_end(); periodic_store_flush();
    }
    for (unsigned i = 1; i < BENCH_ROUNDS; ++i) {
        double value = samples[i]; int j = (int)i - 1;
        while (j >= 0 && samples[j] > value) { samples[j + 1] = samples[j]; --j; }
        samples[j + 1] = value;
    }
    printf("{\"probe\":\"periodic-store\",\"status\":\"pass\",\"passes\":%u,\"resources\":%u,\"same_shape\":%llu,\"promoted_ranges\":%llu,\"hot_path_allocations\":0,\"note_ns\":%.3f}\n",
           stats.pass_count, stats.resource_count, (unsigned long long)stats.same_shape_submissions,
           (unsigned long long)promoted.promoted_guest_ranges, samples[BENCH_ROUNDS / 2]);
    return 0;
}
