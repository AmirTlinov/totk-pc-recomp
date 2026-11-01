#include "periodic_camera.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>

enum { SLOT_COUNT = 64, WRITES = 250000 };
static PeriodicCameraSnapshot slots[SLOT_COUNT];
static _Atomic unsigned hold_writer, writer_entered, release_writer, done;
static _Atomic uint64_t latest_frame, accepted;

void periodic_camera_snapshot_after_projection(const PeriodicCameraSnapshot *snapshot) {
    (void)snapshot;
    if (!atomic_load_explicit(&hold_writer, memory_order_acquire)) return;
    atomic_store_explicit(&writer_entered, 1, memory_order_release);
    while (!atomic_load_explicit(&release_writer, memory_order_acquire)) sched_yield();
}

static uint64_t value(uint64_t frame) { return frame ^ UINT64_C(0x12345678abcdef00); }
static void *paused_writer(void *unused) {
    (void)unused;
    periodic_camera_snapshot_store(&slots[1], 65, value(65));
    return NULL;
}

static void deterministic_replacement(void) {
    uint64_t projection = 0;
    assert(!periodic_camera_snapshot_load(&slots[1], 1, &projection));
    periodic_camera_snapshot_store(&slots[1], 1, value(1));
    assert(periodic_camera_snapshot_load(&slots[1], 1, &projection) && projection == value(1));
    atomic_store(&hold_writer, 1);
    pthread_t writer;
    assert(!pthread_create(&writer, NULL, paused_writer, NULL));
    while (!atomic_load_explicit(&writer_entered, memory_order_acquire)) sched_yield();
    // The real writer is stopped after changing projection but before changing
    // frame. The former frame/projection/frame reader accepted value(65) here.
    assert(atomic_load(&slots[1].frame) == 2);
    assert(atomic_load(&slots[1].projection) == value(65));
    assert(!periodic_camera_snapshot_load(&slots[1], 1, &projection));
    assert(!periodic_camera_snapshot_load(&slots[1], 65, &projection));
    atomic_store_explicit(&release_writer, 1, memory_order_release);
    assert(!pthread_join(writer, NULL));
    atomic_store(&hold_writer, 0);
    assert(!periodic_camera_snapshot_load(&slots[1], 1, &projection));
    assert(periodic_camera_snapshot_load(&slots[1], 65, &projection) && projection == value(65));
    periodic_camera_snapshot_store(&slots[1], UINT64_MAX, 0);
    assert(!periodic_camera_snapshot_load(&slots[1], UINT64_MAX, &projection));
    assert(periodic_camera_snapshot_load(&slots[1], 65, &projection) && projection == value(65));
}

static void *stress_writer(void *unused) {
    (void)unused;
    for (uint64_t frame = 128; frame < WRITES + 128; ++frame) {
        periodic_camera_snapshot_store(&slots[frame % SLOT_COUNT], frame, value(frame));
        atomic_store_explicit(&latest_frame, frame, memory_order_release);
    }
    atomic_store_explicit(&done, 1, memory_order_release);
    return NULL;
}
static void *stress_reader(void *unused) {
    (void)unused;
    do {
        uint64_t frame = atomic_load_explicit(&latest_frame, memory_order_acquire), projection;
        if (frame && periodic_camera_snapshot_load(&slots[frame % SLOT_COUNT], frame, &projection)) {
            assert(projection == value(frame));
            atomic_fetch_add_explicit(&accepted, 1, memory_order_relaxed);
        }
        // Deliberately query the previous occupant while the writer reuses it.
        if (frame >= SLOT_COUNT && periodic_camera_snapshot_load(
                &slots[frame % SLOT_COUNT], frame - SLOT_COUNT, &projection))
            assert(projection == value(frame - SLOT_COUNT));
    } while (!atomic_load_explicit(&done, memory_order_acquire));
    return NULL;
}

int main(void) {
    deterministic_replacement();
    pthread_t writer, readers[3];
    for (unsigned i = 0; i < 3; ++i) assert(!pthread_create(&readers[i], NULL, stress_reader, NULL));
    assert(!pthread_create(&writer, NULL, stress_writer, NULL));
    assert(!pthread_join(writer, NULL));
    for (unsigned i = 0; i < 3; ++i) assert(!pthread_join(readers[i], NULL));
    assert(atomic_load(&accepted));
    printf("periodic camera: interrupted publication and %u ring writes passed (%llu reads)\n",
        WRITES, (unsigned long long)atomic_load(&accepted));
    return 0;
}
