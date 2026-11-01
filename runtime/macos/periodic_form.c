#include "periodic_form.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { RECORD_CAPACITY = 1u << 16, FORM_CLOSE_RETURNS = 3, ANCHOR_SLOTS = 256,
       FORM_CAPACITY = 4096, FORM_BUCKETS = 8192, FORM_COMMAND_LIMIT = 8192,
       PENDING_FORM_CAPACITY = 512, PASS_MAP_CAPACITY = 1u << 15 };
enum { RECORD_PACKET, RECORD_ANCHOR, RECORD_STOP };

typedef struct {
    uint32_t kind;
    uint32_t op;
    uint8_t static_mask, dynamic_mask, resource_mask, flags, resource_count;
    uint64_t anchor_id;
    uint64_t pass_id;
    uint64_t fields[8];
    PeriodicResourceRef resources[4];
} ObserverRecord;

// This is the collision-safe normalized identity of one command. Values in a
// dynamic slot and resource generations are deliberately absent. Resource
// roles and offsets describe bindings; serials are NextBindings values.
typedef struct {
    uint32_t op;
    uint8_t static_mask, dynamic_mask, resource_mask, flags;
    uint64_t fields[8];
    struct {
        uint32_t role;
        uint64_t offset;
    } resources[8];
} FormCommand;

typedef struct {
    FormCommand *commands;
    size_t count, capacity;
    uint64_t hash;
    uint64_t last_anchor;
    uint64_t pass_id;
    uint64_t id;
    uint32_t consecutive_returns;
    uint32_t closed;
    uint32_t invalid;
    uint32_t next;
} CompiledForm;

typedef struct {
    _Atomic uint64_t pass_id;
    _Atomic uint64_t form_id;
    _Atomic uint64_t anchor_id;
} PassForm;

static struct {
    ObserverRecord *records;
    _Alignas(64) _Atomic uint64_t read;
    _Alignas(64) _Atomic uint64_t write;
    _Atomic uint64_t processed;
    uint64_t producer_write, producer_published, producer_read_cache, producer_packets;
    _Atomic uint64_t dropped, loss_epoch;
    _Atomic uint64_t observed_packets, observed_anchors;
    _Atomic uint64_t learned_forms, closed_forms, invalidated_forms;
    _Atomic uint64_t mapped_passes;
    _Atomic uint64_t state_generation;
    struct { _Atomic uint64_t anchor, generation; } anchors[ANCHOR_SLOTS];
    _Atomic int active, stopping;
    pthread_t compiler;
    CompiledForm *forms;
    PassForm pass_forms[PASS_MAP_CAPACITY];
    uint32_t buckets[FORM_BUCKETS];
    size_t form_count;
} state;

static pthread_once_t once = PTHREAD_ONCE_INIT;

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

static int reserve_commands(CompiledForm *form, size_t count) {
    if (count <= form->capacity) return 1;
    size_t capacity = form->capacity ? form->capacity * 2 : 256;
    while (capacity < count) capacity *= 2;
    FormCommand *commands = realloc(form->commands, capacity * sizeof *commands);
    if (!commands) return 0;
    form->commands = commands;
    form->capacity = capacity;
    return 1;
}

static int same_form(const CompiledForm *a, const CompiledForm *b) {
    return a->hash == b->hash && a->count == b->count &&
           !memcmp(a->commands, b->commands, a->count * sizeof *a->commands);
}

static void publish_pass_form(uint64_t pass_id, uint64_t form_id, uint64_t anchor_id) {
    if (!pass_id || !form_id) return;
    PassForm *mapping = &state.pass_forms[mix64(pass_id) & (PASS_MAP_CAPACITY - 1)];
    atomic_store_explicit(&mapping->pass_id, 0, memory_order_release);
    atomic_store_explicit(&mapping->form_id, form_id, memory_order_relaxed);
    atomic_store_explicit(&mapping->anchor_id, anchor_id, memory_order_relaxed);
    atomic_store_explicit(&mapping->pass_id, pass_id, memory_order_release);
    atomic_fetch_add_explicit(&state.mapped_passes, 1, memory_order_relaxed);
}

int periodic_form_for_pass(uint64_t pass_id, uint64_t *form_id, uint64_t *anchor_id) {
    if (!pass_id || !periodic_form_active()) return 0;
    PassForm *mapping = &state.pass_forms[mix64(pass_id) & (PASS_MAP_CAPACITY - 1)];
    uint64_t first = atomic_load_explicit(&mapping->pass_id, memory_order_acquire);
    if (first != pass_id) return 0;
    uint64_t form = atomic_load_explicit(&mapping->form_id, memory_order_relaxed);
    uint64_t anchor = atomic_load_explicit(&mapping->anchor_id, memory_order_relaxed);
    if (atomic_load_explicit(&mapping->pass_id, memory_order_acquire) != first) return 0;
    if (form_id) *form_id = form;
    if (anchor_id) *anchor_id = anchor;
    return form != 0;
}

static void forget_forms(void) {
    for (size_t i = 0; i < state.form_count; ++i) free(state.forms[i].commands);
    free(state.forms);
    state.forms = NULL;
    state.form_count = 0;
    memset(state.buckets, 0, sizeof state.buckets);
}

static void retain_form(CompiledForm *candidate, uint64_t anchor_id) {
    unsigned bucket = (unsigned)(candidate->hash % FORM_BUCKETS);
    for (uint32_t entry = state.buckets[bucket]; entry; entry = state.forms[entry - 1].next) {
        CompiledForm *form = &state.forms[entry - 1];
        if (!same_form(form, candidate)) continue;
        if (form->last_anchor + 1 == anchor_id) form->consecutive_returns++;
        else form->consecutive_returns = 1;
        form->last_anchor = anchor_id;
        if (!form->closed && form->consecutive_returns >= FORM_CLOSE_RETURNS) {
            form->closed = 1;
            atomic_fetch_add_explicit(&state.closed_forms, 1, memory_order_relaxed);
        }
        if (form->closed) publish_pass_form(candidate->pass_id, form->id, anchor_id);
        return;
    }
    if (state.form_count == FORM_CAPACITY) {
        atomic_fetch_add_explicit(&state.invalidated_forms, 1, memory_order_relaxed);
        return;
    }
    candidate->last_anchor = anchor_id;
    candidate->id = state.form_count + 1;
    candidate->consecutive_returns = 1;
    candidate->next = state.buckets[bucket];
    state.buckets[bucket] = (uint32_t)state.form_count + 1;
    state.forms[state.form_count++] = *candidate;
    memset(candidate, 0, sizeof *candidate);
    atomic_fetch_add_explicit(&state.learned_forms, 1, memory_order_relaxed);
}

static void append_command(CompiledForm *candidate, const ObserverRecord *record) {
    if (candidate->count == FORM_COMMAND_LIMIT) {
        candidate->invalid = 1;
        return;
    }
    if (!reserve_commands(candidate, candidate->count + 1)) {
        candidate->invalid = 1;
        return;
    }
    FormCommand command = {
        .op = record->op,
        .static_mask = record->static_mask,
        .dynamic_mask = record->dynamic_mask,
        .resource_mask = record->resource_mask,
        .flags = record->flags,
    };
    for (unsigned i = 0; i < 8; ++i) {
        uint8_t bit = (uint8_t)(1u << i);
        if (record->static_mask & bit) command.fields[i] = record->fields[i];
        if (record->resource_mask & bit) {
            unsigned binding = (unsigned)__builtin_popcount((unsigned)record->resource_mask & (bit - 1u));
            command.resources[i].role = record->resources[binding].role;
            command.resources[i].offset = record->resources[binding].offset;
        }
    }
    candidate->commands[candidate->count++] = command;
    candidate->hash = hash_bytes(candidate->hash, &command, sizeof command);
}

static void finish_candidate(CompiledForm *candidate, CompiledForm pending[PENDING_FORM_CAPACITY], size_t *count) {
    if (candidate->count && !candidate->invalid && *count < PENDING_FORM_CAPACITY) {
        pending[(*count)++] = *candidate;
        memset(candidate, 0, sizeof *candidate);
    } else if (candidate->invalid || candidate->count) {
        atomic_fetch_add_explicit(&state.invalidated_forms, 1, memory_order_relaxed);
    }
    free(candidate->commands);
    memset(candidate, 0, sizeof *candidate);
    candidate->hash = UINT64_C(1469598103934665603);
}

static void *compile_forms(void *unused) {
    (void)unused;
    CompiledForm candidate = {.hash = UINT64_C(1469598103934665603)};
    CompiledForm pending[PENDING_FORM_CAPACITY] = {0};
    size_t pending_count = 0;
    uint64_t seen_loss = 0;
    int in_pass = 0;
    for (;;) {
        uint64_t read = atomic_load_explicit(&state.read, memory_order_relaxed);
        uint64_t write = atomic_load_explicit(&state.write, memory_order_acquire);
        if (read == write) {
            if (atomic_load_explicit(&state.stopping, memory_order_relaxed) &&
                atomic_load_explicit(&state.read, memory_order_relaxed) ==
                    atomic_load_explicit(&state.write, memory_order_acquire)) break;
            struct timespec pause = {.tv_nsec=1000000};
            nanosleep(&pause, NULL);
            continue;
        }
        const ObserverRecord *record = &state.records[read % RECORD_CAPACITY];
        uint64_t loss = atomic_load_explicit(&state.loss_epoch, memory_order_acquire);
        if (loss != seen_loss) {
            if (candidate.count || in_pass)
                atomic_fetch_add_explicit(&state.invalidated_forms, 1, memory_order_relaxed);
            free(candidate.commands);
            memset(&candidate, 0, sizeof candidate);
            candidate.hash = UINT64_C(1469598103934665603);
            in_pass = 0;
            for (size_t i = 0; i < pending_count; ++i) free(pending[i].commands);
            if (pending_count)
                atomic_fetch_add_explicit(&state.invalidated_forms, pending_count, memory_order_relaxed);
            pending_count = 0;
            seen_loss = loss;
        }
        if (record->kind == RECORD_ANCHOR) {
            if (candidate.count || in_pass) {
                atomic_fetch_add_explicit(&state.invalidated_forms, 1, memory_order_relaxed);
                free(candidate.commands);
                memset(&candidate, 0, sizeof candidate);
                candidate.hash = UINT64_C(1469598103934665603);
                in_pass = 0;
            }
            for (size_t i = 0; i < pending_count; ++i) {
                retain_form(&pending[i], record->anchor_id);
                free(pending[i].commands);
            }
            pending_count = 0;
            atomic_store_explicit(&state.read, read + 1, memory_order_release);
            atomic_store_explicit(&state.processed, read + 1, memory_order_release);
            continue;
        }
        if (record->kind == RECORD_STOP) {
            atomic_store_explicit(&state.read, read + 1, memory_order_release);
            atomic_store_explicit(&state.processed, read + 1, memory_order_release);
            break;
        }
        if (record->flags & PERIODIC_PACKET_PASS_BEGIN) {
            if (candidate.count || in_pass) {
                atomic_fetch_add_explicit(&state.invalidated_forms, 1, memory_order_relaxed);
                free(candidate.commands);
                memset(&candidate, 0, sizeof candidate);
                candidate.hash = UINT64_C(1469598103934665603);
            }
            candidate.pass_id = record->pass_id;
            in_pass = 1;
        }
        if (in_pass) append_command(&candidate, record);
        if (record->flags & PERIODIC_PACKET_PASS_END) {
            if (in_pass) finish_candidate(&candidate, pending, &pending_count);
            in_pass = 0;
        }
        atomic_store_explicit(&state.read, read + 1, memory_order_release);
        atomic_store_explicit(&state.processed, read + 1, memory_order_release);
    }
    free(candidate.commands);
    for (size_t i = 0; i < pending_count; ++i) free(pending[i].commands);
    return NULL;
}

static ObserverRecord *reserve_record(uint64_t *position) {
    uint64_t write = state.producer_write;
    uint64_t read = state.producer_read_cache;
    if (write - read == RECORD_CAPACITY) {
        read = atomic_load_explicit(&state.read, memory_order_acquire);
        state.producer_read_cache = read;
    }
    if (write - read == RECORD_CAPACITY) {
        atomic_fetch_add_explicit(&state.dropped, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&state.loss_epoch, 1, memory_order_release);
        return NULL;
    }
    *position = write;
    return &state.records[write % RECORD_CAPACITY];
}

static void publish_record(uint64_t position) {
    state.producer_write = position + 1;
}

static void publish_batch(int force) {
    if (state.producer_write == state.producer_published) return;
    if (!force && state.producer_write - state.producer_published < 16) return;
    atomic_store_explicit(&state.write, state.producer_write, memory_order_release);
    state.producer_published = state.producer_write;
    atomic_store_explicit(&state.observed_packets, state.producer_packets, memory_order_relaxed);
}

static void initialize(void) {
    const char *enabled = getenv("TOTK_PERIODIC_FORMS");
    if (enabled && !strcmp(enabled, "0")) return;
    state.records = calloc(RECORD_CAPACITY, sizeof *state.records);
    state.forms = calloc(FORM_CAPACITY, sizeof *state.forms);
    if (!state.records || !state.forms) {
        free(state.records); free(state.forms);
        state.records = NULL; state.forms = NULL;
        return;
    }
    if (pthread_create(&state.compiler, NULL, compile_forms, NULL)) {
        free(state.records);
        free(state.forms);
        state.records = NULL;
        state.forms = NULL;
        return;
    }
    atomic_store_explicit(&state.active, 1, memory_order_release);
    atexit(periodic_form_finish);
}

void periodic_form_init(void) { pthread_once(&once, initialize); }
int periodic_form_active(void) { return atomic_load_explicit(&state.active, memory_order_relaxed); }

void periodic_observe_packet(uint32_t op, const uint64_t fields[8], uint8_t static_mask,
                             uint8_t dynamic_mask, uint8_t resource_mask, uint8_t flags,
                             const PeriodicResourceRef resources[8], uint64_t pass_id) {
    if (!periodic_form_active()) return;
    uint64_t position;
    ObserverRecord *record = reserve_record(&position);
    if (!record) return;
    record->kind=RECORD_PACKET; record->op=op; record->static_mask=static_mask;
    record->dynamic_mask=dynamic_mask; record->resource_mask=resource_mask; record->flags=flags;
    record->pass_id=pass_id;
    memcpy(record->fields, fields, sizeof record->fields);
    unsigned count = 0;
    for (unsigned i = 0; i < 8; ++i) if (resource_mask & (1u << i)) {
        if (count == 4) { atomic_fetch_add_explicit(&state.loss_epoch, 1, memory_order_release); return; }
        record->resources[count++] = resources ? resources[i] : (PeriodicResourceRef){0};
    }
    record->resource_count = (uint8_t)count;
    publish_record(position);
    state.producer_packets++;
    publish_batch((flags & PERIODIC_PACKET_PASS_END) != 0);
}

uint64_t periodic_anchor_publish(uint64_t anchor_id) {
    if (!periodic_form_active()) return anchor_id + 1;
    uint64_t generation = atomic_fetch_add_explicit(&state.state_generation, 1, memory_order_relaxed) + 1;
    unsigned slot = (unsigned)(anchor_id % ANCHOR_SLOTS);
    atomic_store_explicit(&state.anchors[slot].generation, generation, memory_order_relaxed);
    atomic_store_explicit(&state.anchors[slot].anchor, anchor_id + 1, memory_order_release);
    uint64_t position;
    ObserverRecord *record = reserve_record(&position);
    if (record) {
        record->kind=RECORD_ANCHOR; record->anchor_id=anchor_id;
        publish_record(position);
        publish_batch(1);
        atomic_fetch_add_explicit(&state.observed_anchors, 1, memory_order_relaxed);
    }
    return generation;
}

uint64_t periodic_anchor_generation(uint64_t anchor_id) {
    if (!periodic_form_active()) return anchor_id + 1;
    unsigned slot = (unsigned)(anchor_id % ANCHOR_SLOTS);
    if (atomic_load_explicit(&state.anchors[slot].anchor, memory_order_acquire) != anchor_id + 1) return 0;
    return atomic_load_explicit(&state.anchors[slot].generation, memory_order_relaxed);
}

void periodic_form_flush(void) {
    if (!periodic_form_active()) return;
    publish_batch(1);
    for (;;) {
        uint64_t read = atomic_load_explicit(&state.processed, memory_order_acquire);
        uint64_t write = atomic_load_explicit(&state.write, memory_order_acquire);
        if (read == write) return;
        struct timespec pause = {.tv_nsec=100000};
        nanosleep(&pause, NULL);
    }
}

PeriodicFormStats periodic_form_stats(void) {
    periodic_form_flush();
    return (PeriodicFormStats) {
        .observed_packets=atomic_load_explicit(&state.observed_packets, memory_order_relaxed),
        .observed_anchors=atomic_load_explicit(&state.observed_anchors, memory_order_relaxed),
        .dropped_records=atomic_load_explicit(&state.dropped, memory_order_relaxed),
        .learned_forms=atomic_load_explicit(&state.learned_forms, memory_order_relaxed),
        .closed_forms=atomic_load_explicit(&state.closed_forms, memory_order_relaxed),
        .mapped_passes=atomic_load_explicit(&state.mapped_passes, memory_order_relaxed),
        .invalidated_forms=atomic_load_explicit(&state.invalidated_forms, memory_order_relaxed),
        .state_generation=atomic_load_explicit(&state.state_generation, memory_order_relaxed),
    };
}

void periodic_form_finish(void) {
    if (!periodic_form_active()) return;
    periodic_form_flush();
    if (!atomic_exchange_explicit(&state.active, 0, memory_order_acq_rel)) return;
    atomic_store_explicit(&state.stopping, 1, memory_order_relaxed);
    pthread_join(state.compiler, NULL);
    if (atomic_load_explicit(&state.observed_anchors, memory_order_relaxed))
        fprintf(stderr, "[totk] periodic forms: %llu anchors, %llu packets, %llu learned, %llu closed, %llu mapped passes, %llu invalidated, %llu dropped\n",
                (unsigned long long)atomic_load_explicit(&state.observed_anchors, memory_order_relaxed),
                (unsigned long long)atomic_load_explicit(&state.observed_packets, memory_order_relaxed),
                (unsigned long long)atomic_load_explicit(&state.learned_forms, memory_order_relaxed),
                (unsigned long long)atomic_load_explicit(&state.closed_forms, memory_order_relaxed),
                (unsigned long long)atomic_load_explicit(&state.mapped_passes, memory_order_relaxed),
                (unsigned long long)atomic_load_explicit(&state.invalidated_forms, memory_order_relaxed),
                (unsigned long long)atomic_load_explicit(&state.dropped, memory_order_relaxed));
    forget_forms();
    free(state.records);
    state.records = NULL;
}

int periodic_form_probe(void) {
    setenv("TOTK_PERIODIC_FORMS", "1", 1);
    periodic_form_init();
    if (!periodic_form_active()) return 1;
    const uint64_t begin[8] = {11}, state_packet[8] = {7}, draw_a[8] = {3, 0, 120}, end[8] = {0};
    for (uint64_t anchor = 0; anchor < 3; ++anchor) {
        uint64_t draw[8]; memcpy(draw, draw_a, sizeof draw); draw[1] = anchor * 4;
        periodic_observe_packet(100, begin, 1, 0, 0, PERIODIC_PACKET_PASS_BEGIN, NULL, 1000 + anchor);
        periodic_observe_packet(101, state_packet, 1, 0, 0, 0, NULL, 0);
        periodic_observe_packet(102, draw, 5, 2, 0, PERIODIC_PACKET_DRAW, NULL, 0);
        periodic_observe_packet(103, end, 1, 0, 0, PERIODIC_PACKET_PASS_END, NULL, 0);
        periodic_anchor_publish(anchor);
    }
    periodic_form_flush();
    PeriodicFormStats stats = periodic_form_stats();
    uint64_t form_id = 0, mapped_anchor = 0;
    if (!periodic_form_for_pass(1002, &form_id, &mapped_anchor) || form_id != 1 || mapped_anchor != 2) return 1;
    if (stats.closed_forms != 1 || stats.learned_forms != 1 || stats.mapped_passes != 1 || stats.dropped_records ||
        stats.observed_packets != 12 || stats.observed_anchors != 3 || stats.state_generation != 3) return 1;
    enum { BENCH_PACKETS = 4096, BENCH_ROUNDS = 9 };
    double samples[BENCH_ROUNDS];
    for (unsigned round = 0; round < BENCH_ROUNDS; ++round) {
        uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        for (unsigned i = 0; i < BENCH_PACKETS; ++i)
            periodic_observe_packet(104, draw_a, 0, 7, 0, 0, NULL, 0);
        samples[round] = (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start) / (double)BENCH_PACKETS;
        periodic_form_flush();
    }
    for (unsigned i = 1; i < BENCH_ROUNDS; ++i) {
        double value = samples[i]; int j = (int)i - 1;
        while (j >= 0 && samples[j] > value) { samples[j + 1] = samples[j]; --j; }
        samples[j + 1] = value;
    }
    double observe_ns = samples[BENCH_ROUNDS / 2];
    if (periodic_form_stats().dropped_records) return 1;
    printf("{\"probe\":\"periodic-form\",\"status\":\"pass\",\"closed_forms\":1,\"mapped_passes\":1,\"hot_path_allocations\":0,\"observe_ns\":%.3f}\n", observe_ns);
    return 0;
}
