#include "periodic_store.h"
#include "periodic_form.h"
#include "periodic_compiler.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    STORE_RING_CAPACITY = 4096,
    STORE_PASS_LIMIT = 2048,
    STORE_USE_LIMIT = 128,
    STORE_USE_BUCKETS = 256,
    STORE_RESOURCE_LIMIT = 4096,
    STORE_RESOURCE_BUCKETS = 8192,
    STORE_PROMOTION_CAPACITY = 32768,
    STORE_PROMOTION_PROBES = 16,
    STORE_PROMOTION_RETURNS = 3,
    STORE_PROMOTION_MAX_BYTES = 64 * 1024 * 1024,
    STORE_VERSION_CAPACITY = 1u << 15,
    STORE_VERSION_PROBES = 16,
    STORE_OWNER_PHASE_CAPACITY = 1024,
    STORE_OWNER_CAPACITY = 2048,
    STORE_OWNER_PENDING_CAPACITY = 2048,
    STORE_OWNER_PROBES = 16,
    STORE_TRANSITION_CAPACITY = 4096,
    STORE_TRANSITION_PROBES = 16,
    STORE_GRAPH_WORDS = (STORE_PASS_LIMIT + 63) / 64,
    STORE_SNAPSHOTS = 3,
};
enum { STORE_RECORD_PASS, STORE_RECORD_SUBMIT, STORE_RECORD_DRAIN };
enum { PROMOTION_EMPTY, PROMOTION_WATCHING, PROMOTION_STABLE, PROMOTION_REJECTED };

typedef struct {
    PeriodicResourceKey key;
    uint64_t generation;
    uint64_t causal_version;
    uint32_t role;
    uint8_t access;
} StoreUse;

typedef struct {
    uint32_t kind;
    uint16_t use_count;
    uint8_t unknown;
    uint64_t id;
    uint64_t loss_epoch;
    PeriodicResourceKey owner;
    uint64_t owner_generation;
    uint32_t owner_role;
    uint32_t phase;
    uint8_t owner_valid;
    uint8_t signature_valid;
    uint64_t command_signature;
    uint64_t form_observation;
    StoreUse uses[STORE_USE_LIMIT];
} StoreRecord;

typedef struct {
    PeriodicResourceKey key;
    uint64_t generation;
    uint64_t version;
    uint8_t occupied;
} ResourceVersion;

typedef struct {
    PeriodicResourceKey owner;
    uint64_t generation;
    uint32_t role;
    uint32_t next_phase;
    uint8_t occupied;
} OwnerPhase;

typedef struct {
    _Atomic uint64_t tag;
    _Atomic uint64_t ready_epoch;
    PeriodicResourceKey owner;
    uint64_t owner_generation;
    uint64_t form_id;
    uint32_t role;
    uint32_t phase;
    uint16_t use_count;
    uint64_t command_signature;
    StoreUse uses[STORE_USE_LIMIT];
} OwnerContinuation;

typedef struct {
    uint64_t hash;
    uint64_t last_observation;
    PeriodicResourceKey owner;
    uint64_t owner_generation;
    uint64_t form_id;
    uint32_t role;
    uint32_t phase;
    uint16_t use_count;
    uint64_t command_signature;
    StoreUse uses[STORE_USE_LIMIT];
    uint8_t occupied;
} OwnerCandidate;

typedef struct {
    uint64_t hash;
    uint64_t base_version;
    uint64_t last_observation;
    PeriodicResourceKey owner;
    uint64_t owner_generation;
    uint64_t command_signature;
    uint32_t role;
    uint32_t phase;
    uint16_t use_count;
    uint16_t write_count;
    StoreUse uses[STORE_USE_LIMIT];
    uint8_t occupied;
} ResultTransition;

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
    StoreRecord *pending_owners;
    GraphSnapshot *snapshots;
    GuestPromotion *promotions;
    ResourceVersion *versions;
    OwnerContinuation *owners;
    OwnerCandidate *owner_candidates;
    ResultTransition *result_transitions;
    OwnerPhase owner_phases[STORE_OWNER_PHASE_CAPACITY];
    _Alignas(64) _Atomic uint64_t read;
    _Alignas(64) _Atomic uint64_t write;
    _Atomic uint64_t processed;
    uint64_t producer_write, producer_read_cache;
    StoreRecord *open;
    StoreRecord *pending_completion;
    uint64_t pending_completion_position;
    int16_t use_buckets[STORE_USE_BUCKETS];
    _Atomic uint64_t observed_passes, observed_submissions, dropped_passes, unknown_passes;
    _Atomic uint64_t ring_drops, submission_overflow;
    _Atomic uint64_t loss_epoch;
    _Atomic uint64_t same_shape_submissions, changed_resources, invalidated_passes;
    _Atomic uint64_t graph_generation;
    _Atomic uint64_t promoted_guest_ranges, rejected_guest_ranges;
    _Atomic uint64_t owner_returns, owner_certificates;
    _Atomic uint64_t deferred_owner_drops, transition_overflow, transition_replacements;
    _Atomic uint64_t ready_certificate_epoch, published_certificate_epoch;
    uint64_t next_resource_version;
    uint64_t candidate_observation;
    uint64_t transition_observation;
    size_t pending_owner_count;
    _Atomic(PeriodicGuestProtectFn) protect_guest;
    _Atomic(PeriodicGuestChangedFn) guest_changed;
    _Atomic(PeriodicGuestEpochFn) guest_epoch;
    _Atomic int published;
    _Atomic int active, stopping;
    pthread_t compiler;
} store;

static pthread_once_t store_once = PTHREAD_ONCE_INIT;

static void publish_record(uint64_t position);

static uint64_t hash_bytes(uint64_t hash, const void *data, size_t size) {
    const unsigned char *p = data;
    for (size_t i = 0; i < size; ++i) {
        hash ^= p[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void copy_store_record(StoreRecord *destination, const StoreRecord *source) {
    size_t size = offsetof(StoreRecord, uses) + (size_t)source->use_count * sizeof *source->uses;
    memcpy(destination, source, size);
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

static uint64_t continuation_hash(const PeriodicResourceKey *owner, uint64_t generation,
                                  uint32_t role, uint32_t phase) {
    return owner_hash(owner) ^ mix64(generation) ^ mix64((uint64_t)role << 32 | phase);
}

static uint64_t entry_hash(const StoreRecord *record) {
    uint64_t hash = continuation_hash(&record->owner, record->owner_generation,
                                      record->owner_role, record->phase);
    hash = hash_bytes(hash, &record->use_count, sizeof record->use_count);
    hash = hash_bytes(hash, &record->command_signature, sizeof record->command_signature);
    for (unsigned i = 0; i < record->use_count; ++i) {
        const StoreUse *use = &record->uses[i];
        hash = hash_bytes(hash, &use->key, sizeof use->key);
        hash = hash_bytes(hash, &use->generation, sizeof use->generation);
        hash = hash_bytes(hash, &use->causal_version, sizeof use->causal_version);
        hash = hash_bytes(hash, &use->role, sizeof use->role);
        hash = hash_bytes(hash, &use->access, sizeof use->access);
    }
    return hash ? hash : 1;
}

static int use_entry_equal(const StoreUse *expected, const StoreUse *current) {
    return expected->generation == current->generation &&
           expected->causal_version == current->causal_version &&
           expected->role == current->role && expected->access == current->access &&
           key_equal(&expected->key, &current->key);
}

static int uses_entry_equal(const StoreUse *expected, const StoreRecord *current) {
    for (unsigned i = 0; i < current->use_count; ++i)
        if (!use_entry_equal(&expected[i], &current->uses[i])) return 0;
    return 1;
}

static uint64_t transition_hash(const StoreRecord *record) {
    uint64_t hash = continuation_hash(&record->owner, record->owner_generation,
                                      record->owner_role, record->phase);
    hash = hash_bytes(hash, &record->use_count, sizeof record->use_count);
    hash = hash_bytes(hash, &record->command_signature, sizeof record->command_signature);
    for (unsigned i = 0; i < record->use_count; ++i) {
        const StoreUse *use = &record->uses[i];
        hash = hash_bytes(hash, &use->key, sizeof use->key);
        hash = hash_bytes(hash, &use->generation, sizeof use->generation);
        if (use->access & PERIODIC_RESOURCE_READ)
            hash = hash_bytes(hash, &use->causal_version, sizeof use->causal_version);
        hash = hash_bytes(hash, &use->role, sizeof use->role);
        hash = hash_bytes(hash, &use->access, sizeof use->access);
    }
    return hash ? hash : 1;
}

static int transition_equal(const ResultTransition *transition, const StoreRecord *record) {
    if (transition->owner_generation != record->owner_generation ||
        transition->role != record->owner_role || transition->phase != record->phase ||
        transition->use_count != record->use_count ||
        transition->command_signature != record->command_signature ||
        !key_equal(&transition->owner, &record->owner)) return 0;
    for (unsigned i = 0; i < record->use_count; ++i) {
        const StoreUse *expected = &transition->uses[i], *current = &record->uses[i];
        if (expected->generation != current->generation || expected->role != current->role ||
            expected->access != current->access || !key_equal(&expected->key, &current->key) ||
            ((current->access & PERIODIC_RESOURCE_READ) &&
             expected->causal_version != current->causal_version)) return 0;
    }
    return 1;
}

static ResultTransition *transition_assign(ResultTransition *transition, const StoreRecord *record,
                                           uint64_t hash, uint64_t observation) {
    uint16_t writes = 0;
    for (unsigned i = 0; i < record->use_count; ++i)
        writes += (record->uses[i].access & PERIODIC_RESOURCE_WRITE) != 0;
    if (!writes || UINT64_MAX - store.next_resource_version < writes) {
        atomic_fetch_add_explicit(&store.transition_overflow, 1, memory_order_relaxed);
        return NULL;
    }
    transition->hash = hash;
    transition->base_version = store.next_resource_version + 1;
    transition->last_observation = observation;
    store.next_resource_version += writes;
    transition->owner = record->owner;
    transition->owner_generation = record->owner_generation;
    transition->command_signature = record->command_signature;
    transition->role = record->owner_role;
    transition->phase = record->phase;
    transition->use_count = record->use_count;
    transition->write_count = writes;
    memcpy(transition->uses, record->uses,
           (size_t)record->use_count * sizeof *record->uses);
    transition->occupied = 1;
    return transition;
}

static ResultTransition *result_transition(const StoreRecord *record) {
    if (!record->owner_valid) return NULL;
    uint64_t hash = transition_hash(record);
    uint64_t observation = ++store.transition_observation;
    unsigned bucket = (unsigned)(hash & (STORE_TRANSITION_CAPACITY - 1));
    ResultTransition *oldest = NULL;
    for (unsigned probe = 0; probe < STORE_TRANSITION_PROBES; ++probe) {
        ResultTransition *transition = &store.result_transitions[bucket];
        if (!transition->occupied)
            return transition_assign(transition, record, hash, observation);
        if (transition->hash == hash && transition_equal(transition, record)) {
            transition->last_observation = observation;
            return transition;
        }
        if (!oldest || transition->last_observation < oldest->last_observation) oldest = transition;
        bucket = (bucket + 1) & (STORE_TRANSITION_CAPACITY - 1);
    }
    if (!oldest) {
        atomic_fetch_add_explicit(&store.transition_overflow, 1, memory_order_relaxed);
        return NULL;
    }
    atomic_fetch_add_explicit(&store.transition_replacements, 1, memory_order_relaxed);
    return transition_assign(oldest, record, hash, observation);
}

static ResourceVersion *resource_version(const PeriodicResourceKey *key, uint64_t generation) {
    unsigned bucket = (unsigned)(key_hash(key) & (STORE_VERSION_CAPACITY - 1));
    for (unsigned probe = 0; probe < STORE_VERSION_PROBES; ++probe) {
        ResourceVersion *entry = &store.versions[bucket];
        if (!entry->occupied) {
            entry->key = *key;
            entry->generation = generation;
            entry->version = ++store.next_resource_version;
            entry->occupied = 1;
            return entry;
        }
        if (key_equal(&entry->key, key)) {
            if (entry->generation != generation) {
                entry->generation = generation;
                entry->version = ++store.next_resource_version;
            }
            return entry;
        }
        bucket = (bucket + 1) & (STORE_VERSION_CAPACITY - 1);
    }
    return NULL;
}

static uint32_t owner_phase(const PeriodicResourceKey *owner, uint64_t generation, uint32_t role) {
    uint64_t hash = continuation_hash(owner, generation, role, 0);
    unsigned bucket = (unsigned)(hash & (STORE_OWNER_PHASE_CAPACITY - 1));
    for (unsigned probe = 0; probe < STORE_OWNER_PROBES; ++probe) {
        OwnerPhase *entry = &store.owner_phases[bucket];
        if (!entry->occupied) {
            entry->owner = *owner; entry->generation = generation; entry->role = role;
            entry->next_phase = 1; entry->occupied = 1;
            return 0;
        }
        if (entry->generation == generation && entry->role == role && key_equal(&entry->owner, owner))
            return entry->next_phase++;
        bucket = (bucket + 1) & (STORE_OWNER_PHASE_CAPACITY - 1);
    }
    return UINT32_MAX;
}

static int owner_identity_equal(const OwnerContinuation *owner, const StoreRecord *record) {
    return owner->owner_generation == record->owner_generation && owner->role == record->owner_role &&
           owner->phase == record->phase && key_equal(&owner->owner, &record->owner);
}

static int causal_entry_equal(const OwnerContinuation *owner, const StoreRecord *record,
                              uint64_t form_id) {
    if (owner->form_id != form_id || owner->use_count != record->use_count ||
        owner->command_signature != record->command_signature ||
        !owner_identity_equal(owner, record)) return 0;
    return uses_entry_equal(owner->uses, record);
}

static int candidate_entry_equal(const OwnerCandidate *candidate, const StoreRecord *record,
                                 uint64_t form_id) {
    return candidate->form_id == form_id && candidate->use_count == record->use_count &&
           candidate->command_signature == record->command_signature &&
           candidate->owner_generation == record->owner_generation &&
           candidate->role == record->owner_role && candidate->phase == record->phase &&
           key_equal(&candidate->owner, &record->owner) &&
           uses_entry_equal(candidate->uses, record);
}

static void candidate_assign(OwnerCandidate *candidate, const StoreRecord *record,
                             uint64_t form_id, uint64_t observation) {
    candidate->hash = entry_hash(record);
    candidate->last_observation = observation;
    candidate->owner = record->owner;
    candidate->owner_generation = record->owner_generation;
    candidate->form_id = form_id;
    candidate->role = record->owner_role;
    candidate->phase = record->phase;
    candidate->use_count = record->use_count;
    candidate->command_signature = record->command_signature;
    memcpy(candidate->uses, record->uses,
           (size_t)record->use_count * sizeof *record->uses);
    candidate->occupied = 1;
}

static int owner_candidate_returned(const StoreRecord *record, uint64_t form_id) {
    uint64_t hash = entry_hash(record);
    uint64_t observation = ++store.candidate_observation;
    unsigned bucket = (unsigned)(hash & (STORE_OWNER_CAPACITY - 1));
    for (unsigned probe = 0; probe < STORE_OWNER_PROBES; ++probe) {
        OwnerCandidate *candidate = &store.owner_candidates[bucket];
        if (!candidate->occupied) break;
        if (candidate->hash == hash && candidate_entry_equal(candidate, record, form_id)) {
            candidate->last_observation = observation;
            return 1;
        }
        bucket = (bucket + 1) & (STORE_OWNER_CAPACITY - 1);
    }
    bucket = (unsigned)(hash & (STORE_OWNER_CAPACITY - 1));
    OwnerCandidate *oldest = NULL;
    for (unsigned probe = 0; probe < STORE_OWNER_PROBES; ++probe) {
        OwnerCandidate *candidate = &store.owner_candidates[bucket];
        if (!candidate->occupied) {
            candidate_assign(candidate, record, form_id, observation);
            return 0;
        }
        if (!oldest || candidate->last_observation < oldest->last_observation) oldest = candidate;
        bucket = (bucket + 1) & (STORE_OWNER_CAPACITY - 1);
    }
    // Provisional entries are compiler-owned and may be replaced. Certified
    // continuations live in the separate immutable table below.
    candidate_assign(oldest, record, form_id, observation);
    return 0;
}

static OwnerContinuation *owner_continuation(const StoreRecord *record, uint64_t form_id) {
    uint64_t hash = entry_hash(record);
    unsigned bucket = (unsigned)(hash & (STORE_OWNER_CAPACITY - 1));
    for (unsigned probe = 0; probe < STORE_OWNER_PROBES; ++probe) {
        OwnerContinuation *owner = &store.owners[bucket];
        uint64_t found = atomic_load_explicit(&owner->tag, memory_order_acquire);
        if (!found) return NULL;
        if (found == hash && causal_entry_equal(owner, record, form_id)) return owner;
        bucket = (bucket + 1) & (STORE_OWNER_CAPACITY - 1);
    }
    return NULL;
}

static OwnerContinuation *create_owner_continuation(const StoreRecord *record, uint64_t form_id) {
    uint64_t hash = entry_hash(record);
    unsigned bucket = (unsigned)(hash & (STORE_OWNER_CAPACITY - 1));
    for (unsigned probe = 0; probe < STORE_OWNER_PROBES; ++probe) {
        OwnerContinuation *owner = &store.owners[bucket];
        uint64_t found = atomic_load_explicit(&owner->tag, memory_order_acquire);
        if (!found) {
            owner->owner = record->owner;
            owner->owner_generation = record->owner_generation;
            owner->form_id = form_id;
            owner->role = record->owner_role;
            owner->phase = record->phase;
            owner->use_count = record->use_count;
            owner->command_signature = record->command_signature;
            memcpy(owner->uses, record->uses,
                   (size_t)record->use_count * sizeof *record->uses);
            atomic_store_explicit(&owner->ready_epoch, 0, memory_order_relaxed);
            atomic_store_explicit(&owner->tag, hash, memory_order_release);
            return owner;
        }
        bucket = (bucket + 1) & (STORE_OWNER_CAPACITY - 1);
    }
    return NULL;
}

static PeriodicPassDecision published_decision(const StoreRecord *record) {
    PeriodicPassDecision decision = {0};
    if (!record->owner_valid) return decision;
    uint64_t hash = entry_hash(record);
    unsigned bucket = (unsigned)(hash & (STORE_OWNER_CAPACITY - 1));
    uint64_t published = atomic_load_explicit(&store.published_certificate_epoch, memory_order_acquire);
    uint64_t best_epoch = 0;
    for (unsigned probe = 0; probe < STORE_OWNER_PROBES; ++probe) {
        OwnerContinuation *owner = &store.owners[bucket];
        uint64_t found = atomic_load_explicit(&owner->tag, memory_order_acquire);
        if (!found) break;
        uint64_t ready = atomic_load_explicit(&owner->ready_epoch, memory_order_acquire);
        if (found == hash && ready > best_epoch && ready <= published &&
            causal_entry_equal(owner, record, owner->form_id)) {
            best_epoch = ready;
            decision = (PeriodicPassDecision){.certificate_id=(uint64_t)bucket + 1,
                                              .form_id=owner->form_id};
        }
        bucket = (bucket + 1) & (STORE_OWNER_CAPACITY - 1);
    }
    return decision;
}

static int key_overlaps(const PeriodicResourceKey *a, const PeriodicResourceKey *b) {
    if (a->owner != b->owner || a->kind != b->kind || a->plane != b->plane) return 0;
    uint64_t al = a->length ? a->length : 1, bl = b->length ? b->length : 1;
    return a->offset <= b->offset ? al > b->offset - a->offset : bl > a->offset - b->offset;
}

static void prepare_causal_entry(StoreRecord *record) {
    record->owner_valid = 0;
    for (unsigned i = 0; i < record->use_count; ++i) {
        StoreUse *use = &record->uses[i];
        ResourceVersion *version = resource_version(&use->key, use->generation);
        if (!version) { record->unknown = 1; continue; }
        use->causal_version = version->version;
        if ((use->access & PERIODIC_RESOURCE_WRITE) && use->key.kind == 1 &&
            use->role >= 0x100 && use->role <= 0x109 &&
            (!record->owner_valid || use->role < record->owner_role)) {
            record->owner = use->key;
            record->owner_generation = use->generation;
            record->owner_role = use->role;
            record->owner_valid = 1;
        }
    }
    if (record->owner_valid) {
        record->phase = owner_phase(&record->owner, record->owner_generation, record->owner_role);
        if (record->phase == UINT32_MAX) { record->owner_valid = 0; record->unknown = 1; }
    }
    if (record->unknown || !record->signature_valid) record->owner_valid = 0;
}

static void complete_resource_versions(StoreRecord *record, int retained) {
    ResultTransition *transition = retained ? NULL : result_transition(record);
    uint16_t write_index = 0;
    for (unsigned i = 0; i < record->use_count; ++i) {
        StoreUse *use = &record->uses[i];
        if (!(use->access & PERIODIC_RESOURCE_WRITE) || retained) continue;
        uint16_t ordinal = write_index++;
        ResourceVersion *version = resource_version(&use->key, use->generation);
        if (!version) continue;
        version->version = transition ? transition->base_version + ordinal : ++store.next_resource_version;
    }
}

static void complete_pending_pass(int retained) {
    StoreRecord *record = store.pending_completion;
    if (!record) return;
    complete_resource_versions(record, retained);
    publish_record(store.pending_completion_position);
    atomic_fetch_add_explicit(&store.observed_passes, 1, memory_order_relaxed);
    store.pending_completion = NULL;
}

static int observe_owner_return(const StoreRecord *record) {
    if (!record->owner_valid) return 1;
    uint64_t form_id = 0;
    int status = periodic_form_observation_status(record->form_observation, &form_id);
    if (status < 0) return 0;
    if (!status) return 1;
    atomic_fetch_add_explicit(&store.owner_returns, 1, memory_order_relaxed);
    if (owner_continuation(record, form_id)) return 1;
    if (!owner_candidate_returned(record, form_id)) return 1;
    OwnerContinuation *owner = create_owner_continuation(record, form_id);
    if (!owner) return 1;
    if (!atomic_load_explicit(&owner->ready_epoch, memory_order_acquire)) {
        uint64_t epoch = atomic_fetch_add_explicit(&store.ready_certificate_epoch, 1,
                                                   memory_order_relaxed) + 1;
        atomic_store_explicit(&owner->ready_epoch, epoch, memory_order_release);
        periodic_form_certify(form_id);
        atomic_fetch_add_explicit(&store.owner_certificates, 1, memory_order_relaxed);
    }
    return 1;
}

static void defer_owner_return(const StoreRecord *record) {
    if (!record->owner_valid) return;
    if (store.pending_owner_count == STORE_OWNER_PENDING_CAPACITY) {
        atomic_fetch_add_explicit(&store.deferred_owner_drops, 1, memory_order_relaxed);
        return;
    }
    copy_store_record(&store.pending_owners[store.pending_owner_count++], record);
}

static void drain_owner_returns(void) {
    size_t retained = 0;
    for (size_t i = 0; i < store.pending_owner_count; ++i) {
        StoreRecord *record = &store.pending_owners[i];
        if (observe_owner_return(record)) continue;
        if (retained != i) copy_store_record(&store.pending_owners[retained], record);
        retained++;
    }
    store.pending_owner_count = retained;
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
    drain_owner_returns();
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
        if (!observe_owner_return(record)) defer_owner_return(record);
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
        if (record->kind == STORE_RECORD_DRAIN) {
            drain_owner_returns();
        } else if (record->kind == STORE_RECORD_SUBMIT) {
            compile_submission(pass_count, record->id, submission_invalid);
            pass_count = 0;
            submission_invalid = 0;
        } else if (pass_count < STORE_PASS_LIMIT) {
            copy_store_record(&store.compile_passes[pass_count++], record);
        } else {
            atomic_fetch_add_explicit(&store.dropped_passes, 1, memory_order_relaxed);
            atomic_fetch_add_explicit(&store.submission_overflow, 1, memory_order_relaxed);
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
    store.pending_owners = calloc(STORE_OWNER_PENDING_CAPACITY, sizeof *store.pending_owners);
    store.snapshots = calloc(STORE_SNAPSHOTS, sizeof *store.snapshots);
    store.promotions = calloc(STORE_PROMOTION_CAPACITY, sizeof *store.promotions);
    store.versions = calloc(STORE_VERSION_CAPACITY, sizeof *store.versions);
    store.owners = calloc(STORE_OWNER_CAPACITY, sizeof *store.owners);
    store.owner_candidates = calloc(STORE_OWNER_CAPACITY, sizeof *store.owner_candidates);
    store.result_transitions = calloc(STORE_TRANSITION_CAPACITY, sizeof *store.result_transitions);
    if (!store.records || !store.compile_passes || !store.pending_owners || !store.snapshots || !store.promotions ||
        !store.versions || !store.owners || !store.owner_candidates || !store.result_transitions) goto fail;
    atomic_store_explicit(&store.published, 0, memory_order_relaxed);
    if (pthread_create(&store.compiler, NULL, compile_graphs, NULL)) goto fail;
    atomic_store_explicit(&store.active, 1, memory_order_release);
    atexit(periodic_store_finish);
    return;
fail:
    free(store.records); free(store.compile_passes); free(store.pending_owners);
    free(store.snapshots); free(store.promotions);
    free(store.versions); free(store.owners); free(store.owner_candidates); free(store.result_transitions);
    store.records = NULL; store.compile_passes = NULL; store.pending_owners = NULL;
    store.snapshots = NULL; store.promotions = NULL;
    store.versions = NULL; store.owners = NULL; store.owner_candidates = NULL; store.result_transitions = NULL;
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
    // Missing renderer completion is conservative: the pass executed.
    complete_pending_pass(0);
    if (store.open) {
        store.open->unknown = 1;
        (void)periodic_store_pass_end();
        complete_pending_pass(0);
    }
    uint64_t position;
    StoreRecord *record = reserve_record(&position);
    if (!record) {
        atomic_fetch_add_explicit(&store.dropped_passes, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&store.ring_drops, 1, memory_order_relaxed);
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
            StoreUse *created = &record->uses[index];
            memset(created, 0, sizeof *created);
            created->key = key; created->generation = generation;
            created->role = role; created->access = access;
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

void periodic_store_pass_signature(uint64_t signature, uint64_t form_observation) {
    if (!store.open) return;
    store.open->command_signature = signature;
    store.open->form_observation = form_observation;
    store.open->signature_valid = form_observation != 0;
}

PeriodicPassDecision periodic_store_pass_end(void) {
    PeriodicPassDecision decision = {0};
    if (!store.open) return decision;
    uint64_t position = store.producer_write;
    prepare_causal_entry(store.open);
    decision = published_decision(store.open);
    if (store.open->unknown) atomic_fetch_add_explicit(&store.unknown_passes, 1, memory_order_relaxed);
    store.pending_completion = store.open;
    store.pending_completion_position = position;
    store.open = NULL;
    return decision;
}

void periodic_store_pass_complete(int retained) { complete_pending_pass(retained != 0); }

void periodic_store_submit(uint64_t submission_id) {
    if (!periodic_store_active()) return;
    if (store.open) (void)periodic_store_pass_end();
    complete_pending_pass(0);
    uint64_t position;
    StoreRecord *record = reserve_record(&position);
    if (!record) {
        atomic_fetch_add_explicit(&store.dropped_passes, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&store.ring_drops, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&store.loss_epoch, 1, memory_order_relaxed);
        return;
    }
    memset(record, 0, offsetof(StoreRecord, uses));
    record->kind = STORE_RECORD_SUBMIT;
    record->id = submission_id;
    record->loss_epoch = atomic_load_explicit(&store.loss_epoch, memory_order_relaxed);
    publish_record(position);
    memset(store.owner_phases, 0, sizeof store.owner_phases);
    atomic_fetch_add_explicit(&store.observed_submissions, 1, memory_order_relaxed);
}

void periodic_store_publish_certificates(uint64_t anchor_id) {
    (void)anchor_id;
    uint64_t ready = atomic_load_explicit(&store.ready_certificate_epoch, memory_order_acquire);
    atomic_store_explicit(&store.published_certificate_epoch, ready, memory_order_release);
}

static void wait_for_store_records(void) {
    for (;;) {
        uint64_t processed = atomic_load_explicit(&store.processed, memory_order_acquire);
        uint64_t write = atomic_load_explicit(&store.write, memory_order_acquire);
        if (processed == write) return;
        struct timespec pause = {.tv_nsec = 100000};
        nanosleep(&pause, NULL);
    }
}

void periodic_store_flush(void) {
    if (!periodic_store_active()) return;
    complete_pending_pass(0);
    wait_for_store_records();
    uint64_t position;
    StoreRecord *record = reserve_record(&position);
    if (!record) return;
    memset(record, 0, offsetof(StoreRecord, uses));
    record->kind = STORE_RECORD_DRAIN;
    record->loss_epoch = atomic_load_explicit(&store.loss_epoch, memory_order_relaxed);
    publish_record(position);
    wait_for_store_records();
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
        .ring_drops=atomic_load_explicit(&store.ring_drops, memory_order_relaxed),
        .submission_overflow=atomic_load_explicit(&store.submission_overflow, memory_order_relaxed),
        .unknown_passes=atomic_load_explicit(&store.unknown_passes, memory_order_relaxed),
        .same_shape_submissions=atomic_load_explicit(&store.same_shape_submissions, memory_order_relaxed),
        .changed_resources=atomic_load_explicit(&store.changed_resources, memory_order_relaxed),
        .invalidated_passes=atomic_load_explicit(&store.invalidated_passes, memory_order_relaxed),
        .promoted_guest_ranges=atomic_load_explicit(&store.promoted_guest_ranges, memory_order_relaxed),
        .rejected_guest_ranges=atomic_load_explicit(&store.rejected_guest_ranges, memory_order_relaxed),
        .graph_generation=atomic_load_explicit(&store.graph_generation, memory_order_relaxed),
        .owner_returns=atomic_load_explicit(&store.owner_returns, memory_order_relaxed),
        .owner_certificates=atomic_load_explicit(&store.owner_certificates, memory_order_relaxed),
        .published_certificates=atomic_load_explicit(&store.published_certificate_epoch, memory_order_relaxed),
        .deferred_owner_drops=atomic_load_explicit(&store.deferred_owner_drops, memory_order_relaxed),
        .transition_overflow=atomic_load_explicit(&store.transition_overflow, memory_order_relaxed),
        .transition_replacements=atomic_load_explicit(&store.transition_replacements, memory_order_relaxed),
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
        const PeriodicCompiledForm *program = periodic_compiler_form(plan->form_ids[pass]);
        if (program && program->eligible_draw_count) {
            bit_set(plan->icb, pass);
            plan->icb_pass_count++;
            for (uint32_t cluster = 0; cluster < program->cluster_count; ++cluster)
                plan->icb_cluster_count += program->clusters[cluster].icb_eligible != 0;
            plan->icb_draw_count += program->eligible_draw_count;
        }
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
    free(store.records); free(store.compile_passes); free(store.pending_owners);
    free(store.snapshots); free(store.promotions);
    free(store.versions); free(store.owners); free(store.owner_candidates); free(store.result_transitions);
    store.records = NULL; store.compile_passes = NULL; store.pending_owners = NULL;
    store.snapshots = NULL; store.promotions = NULL;
    store.versions = NULL; store.owners = NULL; store.owner_candidates = NULL; store.result_transitions = NULL;
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

static uint64_t probe_observe_form(uint64_t pass_id) {
    const uint64_t begin[8] = {1}, draw[8] = {3, 0, 12}, end[8] = {0};
    periodic_observe_packet(100, begin, 1, 0, 0,
        PERIODIC_PACKET_PASS_BEGIN | PERIODIC_PACKET_ICB_BREAK, NULL, pass_id);
    periodic_observe_packet(101, draw, 5, 2, 0,
        PERIODIC_PACKET_DRAW | PERIODIC_PACKET_ICB_COMMAND, NULL, 0);
    periodic_observe_packet(101, draw, 5, 2, 0,
        PERIODIC_PACKET_DRAW | PERIODIC_PACKET_ICB_COMMAND, NULL, 0);
    return periodic_observe_packet(102, end, 1, 0, 0,
        PERIODIC_PACKET_PASS_END | PERIODIC_PACKET_ICB_BREAK, NULL, 0);
}

static PeriodicPassDecision probe_owner_pass(uint64_t pass_id,
                                             PeriodicResourceKey input, uint64_t input_generation,
                                             PeriodicResourceKey output, uint64_t form_observation) {
    periodic_store_pass_begin(pass_id);
    periodic_store_resource(input, input_generation, 0x20, PERIODIC_RESOURCE_READ);
    periodic_store_resource(output, 1, 0x100, PERIODIC_RESOURCE_WRITE);
    periodic_store_pass_signature(UINT64_C(0x5ca1ab1e), form_observation);
    PeriodicPassDecision decision = periodic_store_pass_end();
    periodic_store_pass_complete(decision.certificate_id != 0);
    return decision;
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
    periodic_compiler_init();
    PeriodicResourceKey input_a = probe_key(1000), output_a = probe_key(1100);
    PeriodicResourceKey input_b = probe_key(1001), output_b = probe_key(1101);
    PeriodicResourceKey output_c = probe_key(1102);
    for (uint64_t recurrence = 0; recurrence < 3; ++recurrence) {
        uint64_t pass_a = 9000 + recurrence * 3, pass_b = pass_a + 1, pass_c = pass_a + 2;
        uint64_t observed_a = probe_observe_form(pass_a);
        uint64_t observed_b = probe_observe_form(pass_b);
        uint64_t observed_c = probe_observe_form(pass_c);
        periodic_anchor_publish(recurrence);
        if (recurrence) periodic_form_flush();
        PeriodicPassDecision decision_a = probe_owner_pass(pass_a, input_a, 1, output_a, observed_a);
        PeriodicPassDecision decision_b = probe_owner_pass(pass_b, input_b, 1, output_b, observed_b);
        PeriodicPassDecision decision_c = probe_owner_pass(pass_c, output_a, 1, output_c, observed_c);
        if (decision_a.certificate_id || decision_b.certificate_id || decision_c.certificate_id) return 1;
        periodic_store_submit(12 + recurrence); periodic_store_flush();
        if (!recurrence) { periodic_form_flush(); periodic_store_flush(); }
    }
    PeriodicStoreStats owner_stats = periodic_store_stats();
    if (owner_stats.owner_returns != 9 || owner_stats.owner_certificates != 3 ||
        owner_stats.published_certificates != 0) return 1;
    // A ready certificate is deliberately invisible before the next anchor.
    // A distinct transition into A also changes the exact entries of A and C;
    // replaying B with identical causes preserves B's logical content version.
    uint64_t observed_9009 = probe_observe_form(9009);
    uint64_t observed_9010 = probe_observe_form(9010);
    uint64_t observed_9011 = probe_observe_form(9011);
    periodic_form_flush();
    PeriodicPassDecision unpublished_a = probe_owner_pass(9009, input_a, 2, output_a, observed_9009);
    PeriodicPassDecision unpublished_b = probe_owner_pass(9010, input_b, 1, output_b, observed_9010);
    PeriodicPassDecision unpublished_c = probe_owner_pass(9011, output_a, 1, output_c, observed_9011);
    if (unpublished_a.certificate_id || unpublished_b.certificate_id || unpublished_c.certificate_id) return 1;
    periodic_store_submit(15); periodic_store_flush();
    periodic_anchor_publish(3);
    uint64_t observed_9012 = probe_observe_form(9012);
    uint64_t observed_9013 = probe_observe_form(9013);
    uint64_t observed_9014 = probe_observe_form(9014);
    periodic_form_flush();
    PeriodicPassDecision stale_a = probe_owner_pass(9012, input_a, 1, output_a, observed_9012);
    PeriodicPassDecision retained_b = probe_owner_pass(9013, input_b, 1, output_b, observed_9013);
    PeriodicPassDecision dependent_c = probe_owner_pass(9014, output_a, 1, output_c, observed_9014);
    if (stale_a.certificate_id || !retained_b.certificate_id || retained_b.form_id != 1 ||
        dependent_c.certificate_id) return 1;
    periodic_store_submit(16); periodic_store_flush(); periodic_compiler_flush();
    uint64_t observed_9015 = probe_observe_form(9015);
    uint64_t observed_9016 = probe_observe_form(9016);
    uint64_t observed_9017 = probe_observe_form(9017);
    periodic_form_flush();
    PeriodicPassDecision changed_a = probe_owner_pass(9015, input_a, 2, output_a, observed_9015);
    retained_b = probe_owner_pass(9016, input_b, 1, output_b, observed_9016);
    dependent_c = probe_owner_pass(9017, output_a, 1, output_c, observed_9017);
    if (changed_a.certificate_id || !retained_b.certificate_id || retained_b.form_id != 1 ||
        dependent_c.certificate_id) return 1;
    periodic_store_submit(17); periodic_store_flush(); periodic_compiler_flush();
    if (!periodic_store_execution_plan(&plan) || plan.pass_count != 3 ||
        (plan.execute[0] & UINT64_C(5)) != UINT64_C(5) ||
        (plan.execute[0] & UINT64_C(2))) return 1;
    owner_stats = periodic_store_stats();
    if (owner_stats.published_certificates != 3 || owner_stats.owner_certificates < 3 ||
        owner_stats.owner_returns < 18 || owner_stats.dropped_passes ||
        owner_stats.deferred_owner_drops || owner_stats.transition_overflow) return 1;
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
    printf("{\"probe\":\"periodic-store\",\"status\":\"pass\",\"passes\":%u,\"resources\":%u,\"same_shape\":%llu,\"promoted_ranges\":%llu,\"owner_certificates\":%llu,\"exact_output_entry\":true,\"dependent_owner_only\":true,\"anchor_publication\":true,\"hot_path_allocations\":0,\"note_ns\":%.3f}\n",
           stats.pass_count, stats.resource_count, (unsigned long long)stats.same_shape_submissions,
           (unsigned long long)promoted.promoted_guest_ranges,
           (unsigned long long)owner_stats.owner_certificates, samples[BENCH_ROUNDS / 2]);
    return 0;
}
