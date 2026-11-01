#include "periodic_gpu_encode.h"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>
int periodic_gpu_encode_probe(void) {
    setenv("TOTK_PERIODIC_GPU_ENCODE", "1", 1);
    @autoreleasepool {
        NSError *error = nil;
        id<MTLDevice> d = MTLCreateSystemDefaultDevice();
        NSString *source =
            @"#include <metal_stdlib>\nusing namespace metal;\nstruct VertexArguments { device float2 *offset [[id(0)]]; };\nstruct FragmentArguments {\n    device float4 *color [[id(0)]];\n    texture2d<float> image [[id(1)]];\n    sampler sampling [[id(2)]];\n};\nvertex float4 v(uint i [[vertex_id]], uint instance [[instance_id]], constant VertexArguments &args [[buffer(0)]],\n               device const float2 *vertices [[buffer(16)]]) {\n    return float4(vertices[i] + *args.offset + float2(float(instance & 1u)*0.03, float(instance >> 1u)*0.02), 0, 1);\n}\nfragment float4 f(constant FragmentArguments &args [[buffer(0)]]) {\n    return *args.color * args.image.sample(args.sampling, float2(0.5));\n}\n";
        MTLCompileOptions *co = [MTLCompileOptions new];
        co.languageVersion = MTLLanguageVersion4_0;
        co.mathMode = MTLMathModeSafe;
        co.mathFloatingPointFunctions = MTLMathFloatingPointFunctionsPrecise;
        id<MTLLibrary> lib = [d newLibraryWithSource:source options:co error:&error];
        if (!lib) {
            fprintf(stderr, "%s\n", error.description.UTF8String);
            return 1;
        }
        id<MTLFunction> vf = [lib newFunctionWithName:@"v"], ff = [lib newFunctionWithName:@"f"];
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = vf;
        pd.fragmentFunction = ff;
        pd.supportIndirectCommandBuffers = YES;
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        id<MTLRenderPipelineState> ps = [d newRenderPipelineStateWithDescriptor:pd error:&error];
        if (!ps) {
            fprintf(stderr, "%s\n", error.description.UTF8String);
            return 2;
        }
        MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
        dd.depthCompareFunction = MTLCompareFunctionAlways;
        MTLResidencySetDescriptor *rd = [MTLResidencySetDescriptor new];
        rd.initialCapacity = 64;
        id<MTLResidencySet> res = [d newResidencySetWithDescriptor:rd error:&error];
        const float colors[2][4] = {{0.8f, 0.2f, 0.1f, 1}, {0.1f, 0.7f, 0.3f, 1}};
        const float offsets[2][2] = {{-0.5f, 0}, {0.5f, 0}};
        const float vertices[2][8] = {{-0.45f, -0.9f, 0.45f, -0.9f, 0, 0.9f, -0.3f, -0.7f},
                                      {-0.4f, -0.8f, 0.4f, -0.8f, 0, 0.8f, -0.3f, -0.6f}};
        const uint16_t indices[2][9] = {{0, 1, 2, 1, 2, 3, 3, 2, 1}, {3, 3, 3, 0, 1, 2, 1, 2, 3}};
        uint32_t indices32[2][9];
        for (unsigned j = 0; j < 2; ++j)
            for (unsigned i = 0; i < 9; ++i)
                indices32[j][i] = indices[j][i];
        id<MTLBuffer> color[2], offset[2], vb[2], ib[2], ib32[2], va[2], fa[2];
        id<MTLArgumentEncoder> ve = [vf newArgumentEncoderWithBufferIndex:0],
                               fe = [ff newArgumentEncoderWithBufferIndex:0];
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.supportArgumentBuffers = YES;
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
        id<MTLSamplerState> sampler = [d newSamplerStateWithDescriptor:sd];
        MTLTextureDescriptor *td =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                               width:1
                                                              height:1
                                                           mipmapped:NO];
        td.storageMode = MTLStorageModeShared;
        td.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> white = [d newTextureWithDescriptor:td];
        uint32_t pixel = 0xffffffff;
        [white replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
                 mipmapLevel:0
                   withBytes:&pixel
                 bytesPerRow:4];
        [res addAllocation:white];
        for (unsigned i = 0; i < 2; ++i) {
            color[i] = [d newBufferWithBytes:colors[i]
                                      length:sizeof colors[i]
                                     options:MTLResourceStorageModeShared];
            offset[i] = [d newBufferWithBytes:offsets[i]
                                       length:sizeof offsets[i]
                                      options:MTLResourceStorageModeShared];
            vb[i] = [d newBufferWithBytes:vertices[i]
                                   length:sizeof vertices[i]
                                  options:MTLResourceStorageModeShared];
            ib[i] = [d newBufferWithBytes:indices[i]
                                   length:sizeof indices[i]
                                  options:MTLResourceStorageModeShared];
            ib32[i] = [d newBufferWithBytes:indices32[i]
                                     length:sizeof indices32[i]
                                    options:MTLResourceStorageModeShared];
            va[i] = [d newBufferWithLength:ve.encodedLength options:MTLResourceStorageModeShared];
            fa[i] = [d newBufferWithLength:fe.encodedLength options:MTLResourceStorageModeShared];
            [ve setArgumentBuffer:va[i] offset:0];
            [ve setBuffer:offset[i] offset:0 atIndex:0];
            [fe setArgumentBuffer:fa[i] offset:0];
            [fe setBuffer:color[i] offset:0 atIndex:0];
            [fe setTexture:white atIndex:1];
            [fe setSamplerState:sampler atIndex:2];
            for (id<MTLAllocation> a in
                 @[ color[i], offset[i], vb[i], ib[i], ib32[i], va[i], fa[i] ])
                [res addAllocation:a];
        }
        periodic_gpu_encode_init(d, 8);
        if (!periodic_gpu_encode_active())
            return 7;
        id<MTLTexture> actual[2], reference[2];
        td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                width:32
                                                               height:16
                                                            mipmapped:NO];
        td.storageMode = MTLStorageModeShared;
        td.usage = MTLTextureUsageRenderTarget;
        for (unsigned i = 0; i < 2; ++i) {
            actual[i] = [d newTextureWithDescriptor:td];
            reference[i] = [d newTextureWithDescriptor:td];
            [res addAllocation:actual[i]];
            [res addAllocation:reference[i]];
        }
        MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new];
        ad.maxBufferBindCount = 31;
        ad.initializeBindings = YES;
        id<MTL4ArgumentTable> vt = [d newArgumentTableWithDescriptor:ad error:&error],
                              ft = [d newArgumentTableWithDescriptor:ad error:&error];
        id<MTL4CommandQueue> q = [d newMTL4CommandQueue];
        [res commit];
        PeriodicGPUChunkTicket previous[2] = {0};
        for (unsigned round = 0; round < 6; ++round) {
            dispatch_semaphore_t done = dispatch_semaphore_create(0);
            _Atomic int failed = 0;
            _Atomic int *failure = &failed;
            for (unsigned flight = 0; flight < 2; ++flight) {
                id<MTL4CommandAllocator> a =
                    [d newCommandAllocatorWithDescriptor:[MTL4CommandAllocatorDescriptor new]
                                                   error:&error];
                id<MTL4CommandBuffer> cb = [d newCommandBuffer];
                [cb beginCommandBufferWithAllocator:a];
                [cb useResidencySet:res];
                PeriodicGPUChunkTicket ticket = periodic_gpu_encode_begin(cb);
                if (!ticket.chunk)
                    return 8;
                if (round == 1 && flight == 0)
                    periodic_gpu_encode_complete(previous[flight]);
                previous[flight] = ticket;
                id<MTLRenderPipelineState> ephemeral =
                    [d newRenderPipelineStateWithDescriptor:pd error:&error];
                id<MTLDepthStencilState> local_depth = [d newDepthStencilStateWithDescriptor:dd];
                if (!periodic_gpu_encode_hold(ticket, (__bridge void *)ephemeral, 1) ||
                    !periodic_gpu_encode_hold(ticket, (__bridge void *)local_depth, 0))
                    return 9;
                PeriodicGPUEncodeRow rows[2] = {0};
                for (unsigned i = 0; i < 2; ++i) {
                    unsigned j = (i + flight + round) % 2;
                    rows[i] =
                        (PeriodicGPUEncodeRow){.pipeline = ephemeral.gpuResourceID._impl,
                                               .depth_stencil = local_depth.gpuResourceID._impl,
                                               .index_address = ib[j].gpuAddress + (j ? 6 : 0),
                                               .index_length = 6,
                                               .vertex_mask = 1u | (1u << 16),
                                               .fragment_mask = 1,
                                               .kind = PERIODIC_GPU_INDEX16,
                                               .primitive = MTLPrimitiveTypeTriangle,
                                               .count = 3,
                                               .instances = 1,
                                               .cull = MTLCullModeNone,
                                               .winding = MTLWindingClockwise,
                                               .fill = MTLTriangleFillModeFill,
                                               .clip = MTLDepthClipModeClip};
                    unsigned kind = round == 4    ? PERIODIC_GPU_ARRAYS
                                    : (round & 1) ? PERIODIC_GPU_INDEX32
                                                  : PERIODIC_GPU_INDEX16;
                    unsigned width = kind == PERIODIC_GPU_INDEX32 ? 4 : 2;
                    int base_vertex = (round == 2 || round == 3) ? -1 : round == 5 ? 1 : 0;
                    unsigned index_start = 3 * (j + (base_vertex < 0));
                    rows[i].kind = kind;
                    rows[i].index_address =
                        (kind == PERIODIC_GPU_INDEX32 ? ib32[j].gpuAddress : ib[j].gpuAddress) +
                        index_start * width;
                    rows[i].index_length = 3 * width;
                    rows[i].base_vertex = base_vertex;
                    rows[i].first = kind == PERIODIC_GPU_ARRAYS ? (i + flight) % 2 : 0;
                    rows[i].count = i && flight ? 2 : 3;
                    rows[i].instances = 1 + ((i + round) & 1);
                    rows[i].base_instance = (flight + round + i) & 1;
                    rows[i].vertex[0] = va[j].gpuAddress;
                    rows[i].vertex[16] = vb[j].gpuAddress;
                    rows[i].fragment[0] = fa[j].gpuAddress;
                    if (!round && !flight && !i) {
                        PeriodicGPUEncodeRow bad = rows[i];
                        bad.index_length = 2;
                        if (periodic_gpu_encode_append(ticket, &bad, NULL))
                            return 10;
                    }
                    uint32_t index = UINT32_MAX;
                    if (!periodic_gpu_encode_append(ticket, &rows[i], &index) || index != i)
                        return 11;
                }
                for (unsigned ref = 0; ref < 2; ++ref) {
                    MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new];
                    rp.colorAttachments[0].texture = ref ? reference[flight] : actual[flight];
                    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                    id<MTL4RenderCommandEncoder> r = [cb renderCommandEncoderWithDescriptor:rp];
                    [r barrierAfterQueueStages:MTLStageAll
                                  beforeStages:MTLStageAll
                             visibilityOptions:MTL4VisibilityOptionDevice |
                                               MTL4VisibilityOptionResourceAlias];
                    [r setRenderPipelineState:ephemeral];
                    [r setDepthStencilState:local_depth];
                    [r setCullMode:MTLCullModeNone];
                    [r setFrontFacingWinding:MTLWindingClockwise];
                    [r setTriangleFillMode:MTLTriangleFillModeFill];
                    [r setDepthClipMode:MTLDepthClipModeClip];
                    [r setDepthBias:0 slopeScale:0 clamp:0];
                    [r setArgumentTable:vt atStages:MTLRenderStageVertex];
                    [r setArgumentTable:ft atStages:MTLRenderStageFragment];
                    if (ref) {
                        for (unsigned i = 0; i < 2; ++i) {
                            [vt setAddress:rows[i].vertex[0] atIndex:0];
                            [vt setAddress:rows[i].vertex[16] atIndex:16];
                            [ft setAddress:rows[i].fragment[0] atIndex:0];
                            if (rows[i].kind == PERIODIC_GPU_ARRAYS)
                                [r drawPrimitives:MTLPrimitiveTypeTriangle
                                      vertexStart:rows[i].first
                                      vertexCount:rows[i].count
                                    instanceCount:rows[i].instances
                                     baseInstance:rows[i].base_instance];
                            else
                                [r drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                              indexCount:rows[i].count
                                               indexType:rows[i].kind == PERIODIC_GPU_INDEX32
                                                             ? MTLIndexTypeUInt32
                                                             : MTLIndexTypeUInt16
                                             indexBuffer:rows[i].index_address
                                       indexBufferLength:rows[i].index_length
                                           instanceCount:rows[i].instances
                                              baseVertex:rows[i].base_vertex
                                            baseInstance:rows[i].base_instance];
                        }
                    } else {
                        [vt setAddress:va[0].gpuAddress atIndex:0];
                        [vt setAddress:vb[0].gpuAddress atIndex:16];
                        [ft setAddress:fa[0].gpuAddress atIndex:0];
                        if (!periodic_gpu_encode_execute(ticket, r, 0, 2))
                            return 12;
                    }
                    [r endEncoding];
                }
                ephemeral = nil;
                local_depth = nil;
                periodic_gpu_encode_seal(ticket);
                [cb endCommandBuffer];
                MTL4CommitOptions *opts = [MTL4CommitOptions new];
                [opts addFeedbackHandler:^(id<MTL4CommitFeedback> f) {
                    if (f.error) {
                        atomic_store(failure, 1);
                        fprintf(stderr, "%s\n", f.error.description.UTF8String);
                    }
                    periodic_gpu_encode_complete(ticket);
                    dispatch_semaphore_signal(done);
                }];
                id<MTL4CommandBuffer> buffers[] = {cb};
                [q commit:buffers count:1 options:opts];
            }
            for (unsigned i = 0; i < 2; ++i)
                if (dispatch_semaphore_wait(done,
                                            dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)))
                    return 3;
            if (atomic_load(&failed))
                return 4;
            for (unsigned i = 0; i < 2; ++i) {
                uint8_t actual_bytes[2048], reference_bytes[2048];
                [actual[i] getBytes:actual_bytes
                        bytesPerRow:128
                         fromRegion:MTLRegionMake2D(0, 0, 32, 16)
                        mipmapLevel:0];
                [reference[i] getBytes:reference_bytes
                           bytesPerRow:128
                            fromRegion:MTLRegionMake2D(0, 0, 32, 16)
                           mipmapLevel:0];
                if (memcmp(actual_bytes, reference_bytes, sizeof actual_bytes)) {
                    fprintf(stderr, "pixels differ round%u flight%u\n", round, i);
                    return 5;
                }
                unsigned red = 0, green = 0;
                for (unsigned p = 0; p < sizeof actual_bytes; p += 4) {
                    red += actual_bytes[p] > 100 && actual_bytes[p + 1] < 100;
                    green += actual_bytes[p + 1] > 100 && actual_bytes[p] < 100;
                }
                if (!red && !green)
                    return 6;
                if (!i && (!red || !green))
                    return 6;
            }
        }
        // Empty chunks still have a well-formed zero-grid indirect dispatch.
        id<MTL4CommandAllocator> allocator =
            [d newCommandAllocatorWithDescriptor:[MTL4CommandAllocatorDescriptor new] error:&error];
        id<MTL4CommandBuffer> empty = [d newCommandBuffer];
        [empty beginCommandBufferWithAllocator:allocator];
        PeriodicGPUChunkTicket ticket = periodic_gpu_encode_begin(empty);
        if (!ticket.chunk)
            return 13;
        periodic_gpu_encode_seal(ticket);
        [empty endCommandBuffer];
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        __block int failed = 0;
        MTL4CommitOptions *opts = [MTL4CommitOptions new];
        [opts addFeedbackHandler:^(id<MTL4CommitFeedback> feedback) {
            failed = feedback.error != nil;
            periodic_gpu_encode_complete(ticket);
            dispatch_semaphore_signal(done);
        }];
        id<MTL4CommandBuffer> buffers[] = {empty};
        [q commit:buffers count:1 options:opts];
        if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) ||
            failed)
            return 14;
        PeriodicGPUEncodeStats stats = periodic_gpu_encode_stats();
        if (stats.chunks != 13 || stats.rows != 24 || stats.executed_ranges != 12 ||
            stats.executed_draws != 24 || stats.invalid_rows != 1 || stats.stale_completions != 1 ||
            stats.busy_fallbacks || stats.capacity_fallbacks)
            return 15;
        puts(
            "{\"probe\":\"gpu-command-generation\",\"status\":\"pass\",\"cpu_draw_encoding\":0,\"gpu_encoded_draws\":24,\"draw_kinds\":[\"arrays\",\"index16\",\"index32\"],\"dynamic_draw_arguments\":true,\"negative_base_vertex\":true,\"late_dispatch_count\":true,\"separate_per_draw_bindings\":true,\"nested_texture_sampler\":true,\"pipeline_depth_retained\":true,\"two_flights\":true,\"parameter_changes\":true,\"reference_pixels_equal\":true,\"stale_completion_rejected\":true,\"bounded_indices\":true,\"empty_chunk\":true}");
        return 0;
    }
}
