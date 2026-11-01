// Addresses of actual game render-object invocations, before NVN recording.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t owner, generation, implementation;
    uint32_t role, variant;
} NativeSceneAddress;
typedef struct {
    uint64_t recording;
    size_t index;
} NativeSceneInvocation;

// Implemented by the NVN recorder. The returned token addresses this invocation
// in this recorder; it is not a closure certificate or a global frame phase.
NativeSceneInvocation nvn_scene_begin(void *recorder, NativeSceneAddress address);
// An abandoned recording cannot finish an invocation in its replacement.
int nvn_scene_end(void *recorder, NativeSceneInvocation token);
