#include "periodic_state.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

static _Atomic int stopped;
static void *publish(void *unused) {
    (void)unused;
    for (uint64_t i = 0; i < 100000; ++i) {
        assert(periodic_anchor_publish(i) == i * 3 + 1);
        assert(periodic_phase_publish() == i * 3 + 2);
        assert(periodic_phase_publish() == i * 3 + 3);
    }
    atomic_store(&stopped, 1); return NULL;
}
static void *read_anchors(void *unused) {
    (void)unused;
    do {
        uint64_t latest = periodic_latest_anchor();
        if (latest == UINT64_MAX) continue;
        // Read both a recent identity and a displaced ring slot. A racing
        // publication may be unavailable; it must never name another state.
        uint64_t ids[3] = {latest, latest > 128 ? latest - 128 : 0, latest > 256 ? latest - 256 : 0};
        for (unsigned j = 0; j < 3; ++j) {
            uint64_t generation = periodic_anchor_generation(ids[j]);
            assert(!generation || generation == ids[j] * 3 + 1);
        }
    } while (!atomic_load(&stopped));
    return NULL;
}
int main(void) {
    pthread_t producer, readers[3];
    assert(periodic_latest_anchor() == UINT64_MAX);
    assert(!periodic_anchor_generation(0));
    for (unsigned i = 0; i < 3; ++i) assert(!pthread_create(&readers[i], NULL, read_anchors, NULL));
    assert(!pthread_create(&producer, NULL, publish, NULL));
    assert(!pthread_join(producer, NULL));
    for (unsigned i = 0; i < 3; ++i) assert(!pthread_join(readers[i], NULL));
    puts("publication identities: concurrent wraparound passed");
}
