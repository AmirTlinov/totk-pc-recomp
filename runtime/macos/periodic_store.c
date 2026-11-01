#include "periodic_store.h"

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
    STORE_GRAPH_WORDS = (STORE_PASS_LIMIT + 63) / 64,
    STORE_SNAPSHOTS = 3,
};
enum { STORE_RECORD_PASS, STORE_RECORD_SUBMIT };

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
    int32_t last_writer;
    uint32_t next;
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
    uint32_t pass_count;
    uint32_t resource_count;
    uint32_t valid;
} GraphSnapshot;

static struct {
    StoreRecord *records;
    StoreRecord *compile_passes;
    GraphSnapshot *snapshots;
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
            GraphResource *resource = graph_resource(graph, &use->key, 1);
            if (!resource) {
                bit_set(graph->unknown, pass);
                for (unsigned prior = 0; prior < pass; ++prior) add_edge(graph, prior, pass);
                last_unknown = (int)pass;
                continue;
            }
            if (use->generation > resource->generation) resource->generation = use->generation;
            if ((use->access & (PERIODIC_RESOURCE_READ | PERIODIC_RESOURCE_WRITE)) && resource->last_writer >= 0)
                add_edge(graph, (unsigned)resource->last_writer, pass);
            if (use->access & PERIODIC_RESOURCE_READ) bit_set(resource->readers, pass);
            if (use->access & PERIODIC_RESOURCE_WRITE) resource->last_writer = (int32_t)pass;
        }
    }
    if (invalid) {
        for (unsigned pass = 0; pass < graph->pass_count; ++pass) {
            bit_set(graph->unknown, pass);
            for (unsigned prior = 0; prior < pass; ++prior) add_edge(graph, prior, pass);
        }
    }

    int previous_index = atomic_load_explicit(&store.published, memory_order_acquire);
    const GraphSnapshot *previous = &store.snapshots[previous_index];
    if (graph->valid && previous->valid && previous->generation && previous->shape_hash == graph->shape_hash &&
        previous->pass_count == graph->pass_count)
        atomic_fetch_add_explicit(&store.same_shape_submissions, 1, memory_order_relaxed);
    uint64_t changed = 0;
    for (unsigned i = 0; i < graph->resource_count; ++i) {
        GraphResource *resource = &graph->resources[i];
        const GraphResource *old = previous->generation ? find_resource(previous, &resource->key) : NULL;
        if (old && old->generation == resource->generation) continue;
        changed++;
        for (unsigned word = 0; word < STORE_GRAPH_WORDS; ++word)
            graph->invalidated[word] |= resource->readers[word];
    }
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
    if (!store.records || !store.compile_passes || !store.snapshots) goto fail;
    atomic_store_explicit(&store.published, 0, memory_order_relaxed);
    if (pthread_create(&store.compiler, NULL, compile_graphs, NULL)) goto fail;
    atomic_store_explicit(&store.active, 1, memory_order_release);
    atexit(periodic_store_finish);
    return;
fail:
    free(store.records); free(store.compile_passes); free(store.snapshots);
    store.records = NULL; store.compile_passes = NULL; store.snapshots = NULL;
}

void periodic_store_init(void) { pthread_once(&store_once, initialize_store); }
int periodic_store_active(void) { return atomic_load_explicit(&store.active, memory_order_relaxed); }

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
    unsigned bucket = (unsigned)(key_hash(&key) & (STORE_USE_BUCKETS - 1));
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
        .graph_generation=atomic_load_explicit(&store.graph_generation, memory_order_relaxed),
        .pass_count=graph->pass_count,
        .resource_count=graph->resource_count,
        .graph_valid=graph->valid,
    };
    release_graph(index);
    return stats;
}

size_t periodic_store_resource_cone(PeriodicResourceKey key, uint64_t *pass_ids, size_t capacity) {
    if (!periodic_store_active()) return 0;
    int index; const GraphSnapshot *graph = acquire_graph(&index);
    uint64_t bits[STORE_GRAPH_WORDS] = {0};
    const GraphResource *resource = find_resource(graph, &key);
    if (resource) memcpy(bits, resource->readers, sizeof bits);
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
    free(store.records); free(store.compile_passes); free(store.snapshots);
    store.records = NULL; store.compile_passes = NULL; store.snapshots = NULL;
}

static PeriodicResourceKey probe_key(uint64_t owner) {
    return (PeriodicResourceKey){.owner=owner, .length=1, .kind=1};
}

int periodic_store_probe(void) {
    setenv("TOTK_PERIODIC_FORMS", "1", 1);
    periodic_store_init();
    if (!periodic_store_active()) return 1;
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
    if (n != 1 || cone[0] != 20) return 1;
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
    printf("{\"probe\":\"periodic-store\",\"status\":\"pass\",\"passes\":%u,\"resources\":%u,\"same_shape\":%llu,\"hot_path_allocations\":0,\"note_ns\":%.3f}\n",
           stats.pass_count, stats.resource_count, (unsigned long long)stats.same_shape_submissions,
           samples[BENCH_ROUNDS / 2]);
    return 0;
}
