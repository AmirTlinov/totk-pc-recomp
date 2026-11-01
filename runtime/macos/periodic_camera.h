// Single replay-thread writer, concurrent nonblocking camera readers.
#pragma once
#include <stdatomic.h>
#include <stdint.h>

typedef struct {
    _Atomic uint64_t sequence;
    _Atomic uint64_t frame;
    _Atomic uint64_t projection;
} PeriodicCameraSnapshot;

void periodic_camera_snapshot_store(PeriodicCameraSnapshot *snapshot,
    uint64_t frame, uint64_t projection);
int periodic_camera_snapshot_load(const PeriodicCameraSnapshot *snapshot,
    uint64_t frame, uint64_t *projection);
