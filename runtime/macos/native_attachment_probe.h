// Included only in diagnostic builds. Exercise the full attachment ledger
// through the production render-pass path and verify all stored planes.
void mtl_probe_attachment_ledger(void) {
    mtl_init();
    enum { W = 16, H = 16 };
    int semantic_checks = 0;
    {
        typeof(S) saved = S;
        NvProg *program = [NvProg new]; NvShader *fragment = [NvShader new]; program->sh[1] = fragment;
        NSDictionary *manifest = @{@"route":@"B", @"bindings":@[], @"raster":@{},
            @"deps":@{@"fout":@{@"0":@"float", @"2":@"uint", @"outside":@"ignored"}}};
        char reflection_error[256];
        if (nvn_refl_from_json(manifest, &fragment->rf, reflection_error, sizeof reflection_error) ||
            fragment->rf.color_mask != ((1u << 0) | (1u << 2)))
            MDIE("attachment liveness lost deps.fout reflection: %s", reflection_error);
        semantic_checks++;
        S.chmask.mask = (0xfu << 0) | (0xfu << 4);
        if (draw_color_writes(program) != 1u) MDIE("attachment liveness treated channel mask or shader output as a write on its own");
        semantic_checks++;
        S.chmask.mask = (0xfu << 8);
        if (draw_color_writes(program) != (1u << 2)) MDIE("attachment liveness lost a real fragment write");
        semantic_checks++;
        program->udc_imgw = 1; program->udc_ok = 1; program->udc_sh[1] = (__bridge void *)fragment;
        if (draw_color_writes(program) != 0xff) MDIE("attachment liveness failed to keep writable-image aliasing conservative");
        semantic_checks++;
        S = saved;
    }
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:W height:H mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> colors[8], depth;
    tex_t ct[8] = {0}, dt = {0};
    for (unsigned i = 0; i < 8; i++) {
        colors[i] = [g_dev newTextureWithDescriptor:td];
        if (!colors[i]) MDIE("attachment ledger probe color allocation");
        res_add(colors[i]);
        ct[i].serial = 0xfffffe10 + i;
        ct[i].d.width = W; ct[i].d.height = H; ct[i].d.format = 0x25;
        serial_put(ct[i].serial, colors[i]);
    }
    td.pixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    depth = [g_dev newTextureWithDescriptor:td];
    if (!depth) MDIE("attachment ledger probe depth allocation");
    res_add(depth);
    dt.serial = 0xfffffe20;
    dt.d.width = W; dt.d.height = H; dt.d.format = 0x36;
    serial_put(dt.serial, depth);
    /* Metal must accept a fragment function which declares two outputs when the pipeline and pass expose only the live
       one. This is the exact API contract used when production omits a masked-out attachment. */
    td.pixelFormat = MTLPixelFormatRGBA8Unorm;
    id<MTLTexture> live_only = [g_dev newTextureWithDescriptor:td];
    if (!live_only) MDIE("attachment liveness probe allocation"); res_add(live_only);
    NSError *shader_error = nil;
    id<MTLLibrary> live_lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 live_vs(uint id [[vertex_id]]) { float2 p=float2((id<<1)&2,id&2); return float4(p*2-1,0,1); }\n"
        "struct O { float4 a [[color(0)]]; float4 b [[color(1)]]; };\n"
        "fragment O live_fs() { O o; o.a=float4(0.25,0.5,0.75,1); o.b=float4(1,0,1,1); return o; }\n"
        options:nil error:&shader_error];
    MTLRenderPipelineDescriptor *live_pd = [MTLRenderPipelineDescriptor new];
    live_pd.vertexFunction = [live_lib newFunctionWithName:@"live_vs"];
    live_pd.fragmentFunction = [live_lib newFunctionWithName:@"live_fs"];
    live_pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> live_ps = [g_dev newRenderPipelineStateWithDescriptor:live_pd error:&shader_error];
    if (!live_ps) MDIE("pipeline rejected an omitted inert fragment output: %s", shader_error.localizedDescription.UTF8String);
    semantic_checks++;
    id<MTLBuffer> cr = probe_buf(W * H * 4 * 8), zr = probe_buf(W * H * 4), sr = probe_buf(W * H), live_rb = probe_buf(W * H * 4);
    op_hdr end = {OP_END, 8};
    mtl_sub_begin(); mtl_exec((const uint8_t *)&end);
    MTL4RenderPassDescriptor *live_pass = [MTL4RenderPassDescriptor new];
    live_pass.colorAttachments[0].texture = live_only; live_pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    live_pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTL4RenderCommandEncoder> live_enc = enc_r_begin(live_pass); enc_r_pso(live_enc, live_ps);
    enc_r_draw(live_enc, MTLPrimitiveTypeTriangle, 0, 3); enc_r_end(live_enc);
    enc_b_copy_texture_buffer(blit(), live_only, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(W, H, 1), live_rb, 0, W * 4, W * H * 4);
    R.has_rt = 1; R.rt.n = 8; R.rt.depth = &dt;
    for (unsigned i = 0; i < 8; i++) {
        R.rt.color[i] = &ct[i]; R.clear_c[i] = 1;
        R.cc[i] = MTLClearColorMake((i + 1) / 255.0, .5, .75, 1);
    }
    R.clear_d = R.clear_s = 1; R.cd = .5; R.cs = 0xa5;
    rp_begin();
    if (g_nsd != 10 || sizeof g_sd / sizeof *g_sd < (size_t)g_nsd)
        MDIE("attachment ledger requires eight color entries plus depth and stencil");
    rp_end();
    for (unsigned i = 0; i < 8; i++)
        enc_b_copy_texture_buffer(blit(), colors[i], 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(W, H, 1), cr, i * W * H * 4, W * 4, W * H * 4);
    id<MTL4ComputeCommandEncoder> be = blit();
    [be copyFromTexture:depth sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1) toBuffer:zr destinationOffset:0 destinationBytesPerRow:W * 4 destinationBytesPerImage:W * H * 4 options:MTLBlitOptionDepthFromDepthStencil];
    [be copyFromTexture:depth sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1) toBuffer:sr destinationOffset:0 destinationBytesPerRow:W destinationBytesPerImage:W * H options:MTLBlitOptionStencilFromDepthStencil];
    mtl_sub_commit(); mtl_finish();
    for (unsigned i = 0; i < W * H; i++) {
        const uint8_t *p = (const uint8_t *)live_rb.contents + i * 4;
        if (abs(p[0] - 64) > 1 || abs(p[1] - 128) > 1 || abs(p[2] - 191) > 1 || p[3] != 255)
            MDIE("omitted-output probe changed pixel %u", i);
        semantic_checks++;
    }
    for (unsigned i = 0; i < W * H; i++) {
        for (unsigned a = 0; a < 8; a++) {
            const uint8_t *p = (const uint8_t *)cr.contents + (a * W * H + i) * 4;
            if (p[0] != a + 1 || abs(p[1] - 128) > 1 || abs(p[2] - 191) > 1 || p[3] != 255)
                MDIE("attachment ledger changed color %u pixel %u", a, i);
        }
        if (((const float *)zr.contents)[i] != .5f || ((const uint8_t *)sr.contents)[i] != 0xa5)
            MDIE("attachment ledger changed depth/stencil pixel %u", i);
    }
    memset(&R, 0, sizeof R); state_reset(); serial_del(dt.serial);
    for (unsigned i = 0; i < 8; i++) { serial_del(ct[i].serial); defer_obj_release(colors[i]); }
    for (id obj in @[depth, live_only, cr, zr, sr, live_rb]) defer_obj_release(obj);
    printf("{\"probe\":\"attachment-ledger\",\"status\":\"pass\",\"checks\":%u}\n", W * H * 34 + semantic_checks);
}
