// --probe-gfx-abi: every host object of nvn_obj.h fits the storage nvn.h reserves
// for its NVN type, the layouts the game reads directly (NVNtextureView, the command buffer's inline fast-path
// fields) sit where TOTK's NVN 55 code expects them, and the GPU address window lies above every guest region.
// The same facts are static assertions in nvn_obj.h; this mode prints the sizes so a change shows its margin.
// Needs no game files.
#include <stddef.h>
#include <stdio.h>

#include "gfx.h"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("  FAIL: " __VA_ARGS__); putchar('\n'); } } while (0)

typedef struct { const char *host, *nvn; size_t size, reserved; } Fit;
#define FIT(T, H) {#T, #H, sizeof(T), sizeof(H)}
static const Fit fits[] = {
    FIT(dev_builder_t, NVNdeviceBuilder),       FIT(dev_t_, NVNdevice),
    FIT(queue_builder_t, NVNqueueBuilder),      FIT(queue_t, NVNqueue),
    FIT(pool_builder_t, NVNmemoryPoolBuilder),  FIT(pool_t, NVNmemoryPool),
    FIT(dpool_t, NVNtexturePool),               FIT(dpool_t, NVNsamplerPool),
    FIT(tex_builder_t, NVNtextureBuilder),      FIT(tex_t, NVNtexture),
    FIT(view_t, NVNtextureView),
    FIT(samp_builder_t, NVNsamplerBuilder),     FIT(samp_t, NVNsampler),
    FIT(prog_t, NVNprogram),
    FIT(sync_t, NVNsync),
    FIT(ev_builder_t, NVNeventBuilder),         FIT(ev_t, NVNevent),
    FIT(win_builder_t, NVNwindowBuilder),       FIT(win_t, NVNwindow),
    FIT(cmd_t, NVNcommandBuffer),
    FIT(color_st, NVNcolorState),               FIT(chmask_st, NVNchannelMaskState),
    FIT(blend_st, NVNblendState),               FIT(ms_st, NVNmultisampleState),
    FIT(poly_st, NVNpolygonState),              FIT(ds_st, NVNdepthStencilState),
    FIT(va_st, NVNvertexAttribState),           FIT(vs_st, NVNvertexStreamState),
};

_Static_assert(sizeof(NVNtextureView) == 16, "NVNtextureView is 16 bytes in TOTK's NVN 55");
_Static_assert(offsetof(cmd_t, inline_cur) == 0x20 && offsetof(cmd_t, inline_end) == 0x28, "cmd_t inline fields");
_Static_assert(RT_GPU_VA_BASE >= RT_ADDRESS_LIMIT, "GPU address window overlaps the guest address space");

// Write watch: a committed heap page and an alias page, written once, are reported once.
static void check_write_watch(void) {
    const RtGuestLayout *g = mem_layout();
    uint64_t heap = 0, pages[8], n;
    CHECK(mem_set_heap_size(0x200000, &heap) == 0 && heap == g->heap.base, "SetHeapSize 2 MiB");
    mem_written_pages(heap, 0x200000, pages, 8);   // drop whatever the commit itself marked
    volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)heap;
    p[0x5000] = 1;
    n = mem_written_pages(heap, 0x200000, pages, 8);
    CHECK(n == 1 && pages[0] == heap + 0x5000, "heap write: %llu pages reported (want 1 at %#llx)",
          (unsigned long long)n, (unsigned long long)(heap + 0x5000));
    printf("write-watch: heap page written once: %llu reported at +%#llx\n", (unsigned long long)n,
           n ? (unsigned long long)(pages[0] - heap) : 0ull);
    n = mem_written_pages(heap, 0x200000, pages, 8);
    CHECK(n == 0, "second call reported %llu pages (want 0)", (unsigned long long)n);
    printf("write-watch: second call: %llu reported\n", (unsigned long long)n);
    p[0x7000] = 2;
    p[0x9000] = 3;
    p[0xA000] = 4;
    n = mem_written_pages(heap, 0x200000, pages, 2);
    uint64_t m = mem_written_pages(heap, 0x200000, pages + 2, 6);
    printf("write-watch: three pages written, max 2: %llu then %llu\n", (unsigned long long)n, (unsigned long long)m);
    CHECK(n == 2 && m == 1, "max 2 reported %llu, then %llu (want 2, 1)", (unsigned long long)n, (unsigned long long)m);

    uint64_t a = g->alias.base + 0x10000;
    CHECK(mem_map_physical(a, 0x4000) == 0, "MapPhysicalMemory");
    mem_written_pages(a, 0x4000, pages, 8);
    *(volatile uint8_t *)(uintptr_t)(a + 0x1000) = 1;
    n = mem_written_pages(a, 0x4000, pages, 8);
    CHECK(n == 1 && pages[0] == a + 0x1000, "alias write: %llu pages reported (want 1)", (unsigned long long)n);
    CHECK(mem_written_pages(a, 0x4000, pages, 8) == 0, "alias: second call reported pages");
    CHECK(mem_unmap_physical(a, 0x4000) == 0, "UnmapPhysicalMemory");
    CHECK(mem_written_pages(RT_KERNEL_BASE, 0x1000, pages, 8) == UINT64_MAX, "kernel region is not write-watched");
    CHECK(mem_set_heap_size(0, &heap) == 0, "SetHeapSize 0");
    printf("write-watch: alias page written once: reported once, then not\n");
}

int probe_gfx_abi(void) {
    size_t tightest = (size_t)-1;
    const char *tight = "";
    for (size_t i = 0; i < sizeof fits / sizeof *fits; i++) {
        const Fit *f = &fits[i];
        CHECK(f->size <= f->reserved, "%s is %zu bytes, %s reserves %zu", f->host, f->size, f->nvn, f->reserved);
        if (f->reserved - f->size < tightest) tightest = f->reserved - f->size, tight = f->host;
    }
    CHECK(sizeof(NVNtextureView) == 16 && sizeof(view_t) == 16, "NVNtextureView %zu bytes, view_t %zu (want 16)",
          sizeof(NVNtextureView), sizeof(view_t));
    CHECK(offsetof(cmd_t, guest_flags) == 0x10, "cmd_t.guest_flags at %#zx (want 0x10)", offsetof(cmd_t, guest_flags));
    CHECK(offsetof(cmd_t, inline_cur) == 0x20, "cmd_t.inline_cur at %#zx (want 0x20)", offsetof(cmd_t, inline_cur));
    CHECK(offsetof(cmd_t, inline_end) == 0x28, "cmd_t.inline_end at %#zx (want 0x28)", offsetof(cmd_t, inline_end));
    CHECK(sizeof(samp_par) == 44, "samp_par is %zu bytes (want 44)", sizeof(samp_par));
    CHECK(RT_GPU_VA_BASE >= RT_ADDRESS_LIMIT && RT_GPU_VA_SIZE > 0, "GPU address window %#llx overlaps the guest space",
          RT_GPU_VA_BASE);
    check_write_watch();
    printf("gfx-abi: %d layouts fit (tightest %s, %zu bytes spare), cmd_t inline fields at +0x20/+0x28, "
           "NVNtextureView %zu bytes\n", (int)(sizeof fits / sizeof *fits), tight, tightest, sizeof(NVNtextureView));
    if (failures) {
        printf("probe-gfx-abi: FAILED (%d of %d checks)\n", failures, checks);
        return 1;
    }
    printf("probe-gfx-abi: passed\n");
    return 0;
}
