#include "periodic_compiler.h"
#include "periodic_form.h"

#import <Metal/Metal.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { COMPILER_FORM_LIMIT = 4096, COMPILER_RING_CAPACITY = 4096 };

static struct {
    uint64_t *form_ids;
    _Alignas(64) _Atomic uint64_t read;
    _Alignas(64) _Atomic uint64_t write;
    _Atomic uint64_t processed;
    uint64_t producer_write, producer_read_cache;
    _Atomic(PeriodicCompiledForm *) programs[COMPILER_FORM_LIMIT];
    _Atomic uint64_t queued_forms, compiled_forms, dropped_forms;
    _Atomic uint64_t clusters, draws, eligible_draws;
    _Atomic int active, stopping;
    pthread_t thread;
} compiler;

static pthread_once_t compiler_once = PTHREAD_ONCE_INIT;

static void queue_form(uint64_t form_id) {
    if (!atomic_load_explicit(&compiler.active, memory_order_relaxed)) return;
    uint64_t write = compiler.producer_write;
    uint64_t read = compiler.producer_read_cache;
    if (write - read == COMPILER_RING_CAPACITY) {
        read = atomic_load_explicit(&compiler.read, memory_order_acquire);
        compiler.producer_read_cache = read;
    }
    if (write - read == COMPILER_RING_CAPACITY) {
        atomic_fetch_add_explicit(&compiler.dropped_forms, 1, memory_order_relaxed);
        return;
    }
    compiler.form_ids[write % COMPILER_RING_CAPACITY] = form_id;
    compiler.producer_write = write + 1;
    atomic_store_explicit(&compiler.write, write + 1, memory_order_release);
    atomic_fetch_add_explicit(&compiler.queued_forms, 1, memory_order_relaxed);
}

static void finish_cluster(PeriodicICBCluster *clusters, uint32_t *count,
                           uint32_t first, uint32_t commands, uint32_t draws,
                           uint32_t dynamic_slots, uint32_t resource_slots,
                           int after_argument_changes, uint32_t *eligible_draws) {
    if (!commands || !draws) return;
    int eligible = after_argument_changes && draws >= 2 && commands <= PERIODIC_ICB_PACKET_LIMIT;
    clusters[(*count)++] = (PeriodicICBCluster) {
        .first_command=first,
        .command_count=commands,
        .draw_count=draws,
        .dynamic_slot_count=dynamic_slots,
        .resource_slot_count=resource_slots,
        .icb_eligible=eligible,
    };
    if (eligible) *eligible_draws += draws;
}

static PeriodicCompiledForm *compile_form(uint64_t form_id) {
    PeriodicFormSnapshot snapshot;
    if (!periodic_form_snapshot(form_id, &snapshot) || !snapshot.command_count) return NULL;
    PeriodicICBCluster *clusters = calloc(snapshot.command_count, sizeof *clusters);
    if (!clusters) return NULL;
    int64_t last_argument_change = -1;
    for (uint32_t i = 0; i < snapshot.command_count; ++i)
        if (snapshot.commands[i].flags & PERIODIC_PACKET_ARGUMENT_CHANGE)
            last_argument_change = i;
    uint32_t cluster_count = 0, first = 0, commands = 0, draws = 0;
    uint32_t dynamic_slots = 0, resource_slots = 0;
    uint32_t total_draws = 0, eligible_draws = 0, total_dynamic = 0, total_resources = 0;
    for (uint32_t i = 0; i < snapshot.command_count; ++i) {
        const PeriodicFormCommand *command = &snapshot.commands[i];
        if (!(command->flags & PERIODIC_PACKET_ICB_COMMAND)) {
            finish_cluster(clusters, &cluster_count, first, commands, draws, dynamic_slots,
                           resource_slots, (int64_t)first > last_argument_change,
                           &eligible_draws);
            commands = draws = dynamic_slots = resource_slots = 0;
            continue;
        }
        uint32_t dynamic = (uint32_t)__builtin_popcount(command->dynamic_mask);
        uint32_t resources = (uint32_t)__builtin_popcount(command->resource_mask);
        total_dynamic += dynamic;
        total_resources += resources;
        if (!commands) first = i;
        commands++;
        dynamic_slots += dynamic;
        resource_slots += resources;
        if (command->flags & PERIODIC_PACKET_DRAW) { draws++; total_draws++; }
    }
    finish_cluster(clusters, &cluster_count, first, commands, draws, dynamic_slots,
                   resource_slots, (int64_t)first > last_argument_change,
                   &eligible_draws);
    PeriodicCompiledForm *program = calloc(1, sizeof *program);
    if (!program) { free(clusters); return NULL; }
    *program = (PeriodicCompiledForm) {
        .form_id=form_id,
        .command_count=(uint32_t)snapshot.command_count,
        .cluster_count=cluster_count,
        .draw_count=total_draws,
        .eligible_draw_count=eligible_draws,
        .dynamic_slot_count=total_dynamic,
        .resource_slot_count=total_resources,
        .clusters=clusters,
    };
    return program;
}

static void *compile_forms(void *unused) {
    (void)unused;
    for (;;) {
        uint64_t read = atomic_load_explicit(&compiler.read, memory_order_relaxed);
        uint64_t write = atomic_load_explicit(&compiler.write, memory_order_acquire);
        if (read == write) {
            if (atomic_load_explicit(&compiler.stopping, memory_order_relaxed)) break;
            struct timespec pause = {.tv_nsec=1000000};
            nanosleep(&pause, NULL);
            continue;
        }
        uint64_t form_id = compiler.form_ids[read % COMPILER_RING_CAPACITY];
        if (form_id && form_id <= COMPILER_FORM_LIMIT &&
            !atomic_load_explicit(&compiler.programs[form_id - 1], memory_order_acquire)) {
            PeriodicCompiledForm *program = compile_form(form_id);
            if (program) {
                atomic_store_explicit(&compiler.programs[form_id - 1], program, memory_order_release);
                atomic_fetch_add_explicit(&compiler.compiled_forms, 1, memory_order_relaxed);
                atomic_fetch_add_explicit(&compiler.clusters, program->cluster_count, memory_order_relaxed);
                atomic_fetch_add_explicit(&compiler.draws, program->draw_count, memory_order_relaxed);
                atomic_fetch_add_explicit(&compiler.eligible_draws, program->eligible_draw_count, memory_order_relaxed);
            } else {
                atomic_fetch_add_explicit(&compiler.dropped_forms, 1, memory_order_relaxed);
            }
        }
        atomic_store_explicit(&compiler.read, read + 1, memory_order_release);
        atomic_store_explicit(&compiler.processed, read + 1, memory_order_release);
    }
    return NULL;
}

static void initialize_compiler(void) {
    compiler.form_ids = calloc(COMPILER_RING_CAPACITY, sizeof *compiler.form_ids);
    if (!compiler.form_ids) return;
    if (pthread_create(&compiler.thread, NULL, compile_forms, NULL)) {
        free(compiler.form_ids); compiler.form_ids = NULL; return;
    }
    atomic_store_explicit(&compiler.active, 1, memory_order_release);
    periodic_form_set_closed_callback(queue_form);
    atexit(periodic_compiler_finish);
}

void periodic_compiler_init(void) {
    periodic_form_init();
    pthread_once(&compiler_once, initialize_compiler);
}

int periodic_compiler_active(void) { return atomic_load_explicit(&compiler.active, memory_order_relaxed); }

const PeriodicCompiledForm *periodic_compiler_form(uint64_t form_id) {
    if (!periodic_compiler_active() || !form_id || form_id > COMPILER_FORM_LIMIT) return NULL;
    return atomic_load_explicit(&compiler.programs[form_id - 1], memory_order_acquire);
}

void periodic_compiler_flush(void) {
    if (!periodic_compiler_active()) return;
    for (;;) {
        uint64_t processed = atomic_load_explicit(&compiler.processed, memory_order_acquire);
        uint64_t write = atomic_load_explicit(&compiler.write, memory_order_acquire);
        if (processed == write) return;
        struct timespec pause = {.tv_nsec=100000};
        nanosleep(&pause, NULL);
    }
}

PeriodicCompilerStats periodic_compiler_stats(void) {
    periodic_compiler_flush();
    return (PeriodicCompilerStats) {
        .queued_forms=atomic_load_explicit(&compiler.queued_forms, memory_order_relaxed),
        .compiled_forms=atomic_load_explicit(&compiler.compiled_forms, memory_order_relaxed),
        .dropped_forms=atomic_load_explicit(&compiler.dropped_forms, memory_order_relaxed),
        .clusters=atomic_load_explicit(&compiler.clusters, memory_order_relaxed),
        .draws=atomic_load_explicit(&compiler.draws, memory_order_relaxed),
        .eligible_draws=atomic_load_explicit(&compiler.eligible_draws, memory_order_relaxed),
    };
}

void periodic_compiler_finish(void) {
    if (!periodic_compiler_active()) return;
    periodic_form_set_closed_callback(NULL);
    periodic_compiler_flush();
    if (!atomic_exchange_explicit(&compiler.active, 0, memory_order_acq_rel)) return;
    atomic_store_explicit(&compiler.stopping, 1, memory_order_release);
    pthread_join(compiler.thread, NULL);
    PeriodicCompilerStats stats = periodic_compiler_stats();
    if (stats.queued_forms)
        fprintf(stderr, "[totk] periodic compiler: %llu forms, %llu clusters, %llu draws, %llu ICB-eligible, %llu dropped\n",
                (unsigned long long)stats.compiled_forms, (unsigned long long)stats.clusters,
                (unsigned long long)stats.draws, (unsigned long long)stats.eligible_draws,
                (unsigned long long)stats.dropped_forms);
    for (unsigned i = 0; i < COMPILER_FORM_LIMIT; ++i) {
        PeriodicCompiledForm *program = atomic_load_explicit(&compiler.programs[i], memory_order_relaxed);
        if (program) { free((void *)program->clusters); free(program); }
    }
    free(compiler.form_ids); compiler.form_ids = NULL;
}

static int probe_metal_icb(void) {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) { fprintf(stderr, "periodic ICB probe: no Metal device\n"); return 0; }
        NSError *error = nil;
        NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "vertex float4 periodic_vertex(uint vertex_id [[vertex_id]]) { const float2 p[3] = {float2(-1,-1),float2(3,-1),float2(-1,3)}; return float4(p[vertex_id],0,1); }\n"
            "fragment float4 periodic_fragment() { return float4(0.25,0.5,0.75,1); }\n";
        id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
        MTLRenderPipelineDescriptor *pipeline_descriptor = [MTLRenderPipelineDescriptor new];
        pipeline_descriptor.vertexFunction = [library newFunctionWithName:@"periodic_vertex"];
        pipeline_descriptor.fragmentFunction = [library newFunctionWithName:@"periodic_fragment"];
        pipeline_descriptor.supportIndirectCommandBuffers = YES;
        pipeline_descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        id<MTLRenderPipelineState> pipeline = library ?
            [device newRenderPipelineStateWithDescriptor:pipeline_descriptor error:&error] : nil;
        MTLTextureDescriptor *texture_descriptor = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
        texture_descriptor.usage = MTLTextureUsageRenderTarget;
        texture_descriptor.storageMode = MTLStorageModeShared;
        id<MTLTexture> texture = [device newTextureWithDescriptor:texture_descriptor];
        MTLIndirectCommandBufferDescriptor *icb_descriptor = [MTLIndirectCommandBufferDescriptor new];
        icb_descriptor.commandTypes = MTLIndirectCommandTypeDraw;
        icb_descriptor.inheritPipelineState = NO;
        icb_descriptor.inheritBuffers = YES;
        id<MTLIndirectCommandBuffer> icb = [device newIndirectCommandBufferWithDescriptor:icb_descriptor
                                                                          maxCommandCount:2
                                                                                  options:MTLResourceStorageModeShared];
        if (!pipeline || !texture || !icb) {
            fprintf(stderr, "periodic ICB probe: pipeline/resource creation failed: %s\n", error.localizedDescription.UTF8String ?: "unknown");
            return 0;
        }
        for (NSUInteger index = 0; index < 2; ++index) {
            id<MTLIndirectRenderCommand> command = [icb indirectRenderCommandAtIndex:index];
            [command setRenderPipelineState:pipeline];
            [command drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3 instanceCount:1 baseInstance:0];
        }
        MTLResidencySetDescriptor *residency_descriptor = [MTLResidencySetDescriptor new];
        residency_descriptor.initialCapacity = 3;
        id<MTLResidencySet> residency = [device newResidencySetWithDescriptor:residency_descriptor error:&error];
        id<MTL4CommandQueue> queue = [device newMTL4CommandQueue];
        MTL4CommandAllocatorDescriptor *allocator_descriptor = [MTL4CommandAllocatorDescriptor new];
        id<MTL4CommandAllocator> allocator = [device newCommandAllocatorWithDescriptor:allocator_descriptor error:&error];
        id<MTL4CommandBuffer> command_buffer = [device newCommandBuffer];
        if (!residency || !queue || !allocator || !command_buffer) {
            fprintf(stderr, "periodic ICB probe: Metal 4 queue creation failed: %s\n", error.localizedDescription.UTF8String ?: "unknown");
            return 0;
        }
        [residency addAllocation:(id<MTLAllocation>)texture];
        [residency addAllocation:(id<MTLAllocation>)icb];
        [residency addAllocation:(id<MTLAllocation>)pipeline];
        [residency commit];
        [command_buffer beginCommandBufferWithAllocator:allocator];
        [command_buffer useResidencySet:residency];
        MTL4RenderPassDescriptor *render_pass = [MTL4RenderPassDescriptor new];
        render_pass.colorAttachments[0].texture = texture;
        render_pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        render_pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        render_pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        id<MTL4RenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:render_pass];
        if (!encoder) { fprintf(stderr, "periodic ICB probe: render encoder creation failed\n"); return 0; }
        [encoder executeCommandsInBuffer:icb withRange:NSMakeRange(0, 2)];
        [encoder endEncoding];
        [command_buffer endCommandBuffer];
        id<MTL4CommandBuffer> buffers[] = {command_buffer};
        dispatch_semaphore_t completion = dispatch_semaphore_create(0);
        __block NSError *gpu_error = nil;
        MTL4CommitOptions *options = [MTL4CommitOptions new];
        [options addFeedbackHandler:^(id<MTL4CommitFeedback> feedback) {
            gpu_error = feedback.error;
            dispatch_semaphore_signal(completion);
        }];
        [queue commit:buffers count:1 options:options];
        if (dispatch_semaphore_wait(completion, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC))) {
            fprintf(stderr, "periodic ICB probe: GPU timeout\n"); return 0;
        }
        if (gpu_error) { fprintf(stderr, "periodic ICB probe: GPU error: %s\n", gpu_error.localizedDescription.UTF8String); return 0; }
        uint8_t pixels[4 * 4 * 4] = {0};
        [texture getBytes:pixels bytesPerRow:16 fromRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0];
        const uint8_t *center = pixels + (2 * 4 + 2) * 4;
        int correct = center[0] >= 62 && center[0] <= 66 && center[1] >= 126 && center[1] <= 130 &&
                      center[2] >= 190 && center[2] <= 194 && center[3] == 255;
        if (!correct) fprintf(stderr, "periodic ICB probe: center pixel is %u,%u,%u,%u\n", center[0], center[1], center[2], center[3]);
        return correct;
    }
}

int periodic_compiler_probe(void) {
    setenv("TOTK_PERIODIC_FORMS", "1", 1);
    periodic_compiler_init();
    if (!periodic_compiler_active()) return 1;
    const uint64_t begin[8] = {1}, pso[8] = {2}, address[8] = {0, 3, 4};
    const uint64_t draw[8] = {3, 0, 12}, boundary[8] = {0}, end[8] = {0};
    for (uint64_t anchor = 0; anchor < 2; ++anchor) {
        periodic_observe_packet(100, begin, 1, 0, 0, PERIODIC_PACKET_PASS_BEGIN | PERIODIC_PACKET_ICB_BREAK, NULL, 7000 + anchor);
        periodic_observe_packet(101, pso, 0, 0, 1, PERIODIC_PACKET_ICB_COMMAND, NULL, 0);
        periodic_observe_packet(103, draw, 1, 6, 0, PERIODIC_PACKET_DRAW | PERIODIC_PACKET_ICB_COMMAND, NULL, 0);
        periodic_observe_packet(102, address, 3, 0, 4,
            PERIODIC_PACKET_ICB_BREAK | PERIODIC_PACKET_ARGUMENT_CHANGE, NULL, 0);
        for (unsigned draw_index = 0; draw_index < 3; ++draw_index)
            periodic_observe_packet(103, draw, 1, 6, 0, PERIODIC_PACKET_DRAW | PERIODIC_PACKET_ICB_COMMAND, NULL, 0);
        periodic_observe_packet(104, boundary, 0, 1, 0, PERIODIC_PACKET_ICB_BREAK, NULL, 0);
        periodic_observe_packet(101, pso, 0, 0, 1, PERIODIC_PACKET_ICB_COMMAND, NULL, 0);
        periodic_observe_packet(103, draw, 1, 6, 0, PERIODIC_PACKET_DRAW | PERIODIC_PACKET_ICB_COMMAND, NULL, 0);
        periodic_observe_packet(105, end, 1, 0, 0, PERIODIC_PACKET_PASS_END | PERIODIC_PACKET_ICB_BREAK, NULL, 0);
        periodic_anchor_publish(anchor);
        periodic_form_flush();
        if (!anchor) {
            uint64_t template_id = 0;
            if (!periodic_form_template_for_pass(7000, &template_id) || template_id != 1 ||
                !periodic_form_certify(template_id)) return 1;
        }
    }
    periodic_form_flush(); periodic_compiler_flush();
    const PeriodicCompiledForm *program = periodic_compiler_form(1);
    PeriodicCompilerStats stats = periodic_compiler_stats();
    if (!program || program->cluster_count != 3 || program->draw_count != 5 ||
        program->eligible_draw_count != 3 || program->dynamic_slot_count != 10 ||
        program->resource_slot_count != 2 || stats.compiled_forms != 1 || stats.dropped_forms || !probe_metal_icb()) return 1;
    printf("{\"probe\":\"periodic-compiler\",\"status\":\"pass\",\"forms\":1,\"clusters\":3,\"draws\":5,\"icb_eligible_draws\":3,\"metal4_icb\":true}\n");
    return 0;
}
