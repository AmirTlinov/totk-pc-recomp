// The in-game menu core: Dear ImGui state, the event queue the window adapter fills, gamepad navigation, the Vulkan back
// end on our device and the menu's pages. Free of any window system (menu.h). ImGui runs on the present path only,
// under gfx_window.c's present lock; the event functions only queue.
//
// The Vulkan set-up follows LAS Recomp's runtime/nvn/vulkan/imgui_glue.cpp (https://github.com/peiikai/LASRecomp @
// da35b83a9611d046a9a681aae668d7085954d37f, MIT, Copyright (c) 2026 Peiikai): dynamic rendering into the swapchain
// image, an internal descriptor pool, vertex buffers rotated over more frames than are in flight.
#include <atomic>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"

extern "C" {
#include "cfg.h"
#include "input.h"
#include "input_map.h"
#include "platform/platform.h"
#include "ui/menu.h"
// host.h and vk.h pull in cpu.h, which is C only: the three functions the menu needs from them, as declared there.
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void vk_queue_lock(void);
void vk_queue_unlock(void);
}

// Roboto Medium (Apache-2.0, ui/fonts/LICENSE-Roboto.txt) with Latin and Cyrillic glyphs.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"   // #embed in C++
static const unsigned char roboto_ttf[] = {
#embed "fonts/Roboto-Medium.ttf"
};
#pragma clang diagnostic pop

// ---- open state, any thread
static std::atomic<int> g_open;     // the menu is open
static std::atomic<int> g_reset;    // opened or closed since the last laid-out frame: drop stale input
static std::atomic<int> g_combo;    // Minus and Plus were held at the last menu_pad_combo

static void set_open(int open) {
    if (g_open.exchange(open) == open) return;
    g_reset.store(1);
    input_menu(open);
    rt_log("menu: %s", open ? "open" : "closed");
}

// TOTK_MENU_TEST=<frame>: the presented frame the test window opens at; 0 when unset (the menu cannot open, no pages yet)
static uint64_t test_frame(void) {
    static std::atomic<long long> f{-1};
    long long v = f.load(std::memory_order_relaxed);
    if (v < 0) {
        const char *e = cfg_get("TOTK_MENU_TEST");
        v = e ? atoll(e) : 0;
        if (v < 0) v = 0;
        f.store(v);
    }
    return (uint64_t)v;
}

extern "C" int menu_available(void) { return test_frame() != 0; }
extern "C" int menu_is_open(void) { return g_open.load(std::memory_order_relaxed); }
extern "C" void menu_toggle(void) {
    if (menu_available()) set_open(!g_open.load());
}

extern "C" void menu_pad_combo(int held) {
    if (g_combo.exchange(held) != held && held) menu_toggle();
}

// ---- events from the window adapter, queued while the menu is open; the present path hands them to ImGui
enum { EV_KEY, EV_POS, EV_BUTTON, EV_WHEEL, EV_FOCUS, EV_MAX = 256 };
struct Ev { int type, a, b; float x, y; };
static PlatMutex ev_lock = PLAT_MUTEX_INIT;
static Ev ev_q[EV_MAX];
static int ev_n;

static void push(Ev e) {
    if (!menu_is_open()) return;
    plat_mutex_lock(&ev_lock);
    if (e.type == EV_POS && ev_n && ev_q[ev_n - 1].type == EV_POS) ev_q[ev_n - 1] = e;   // only the newest position
    else if (ev_n < EV_MAX) ev_q[ev_n++] = e;
    plat_mutex_unlock(&ev_lock);
}

extern "C" void menu_event_key(int key, int down) { push({EV_KEY, key, down, 0, 0}); }
extern "C" void menu_event_mouse_pos(float x, float y) { push({EV_POS, 0, 0, x, y}); }
extern "C" void menu_event_mouse_button(int button, int down) { push({EV_BUTTON, button, down, 0, 0}); }
extern "C" void menu_event_wheel(float dx, float dy) { push({EV_WHEEL, 0, 0, dx, dy}); }
extern "C" void menu_event_focus(int focused) { push({EV_FOCUS, focused, 0, 0, 0}); }

static void drain_events(ImGuiIO &io) {
    Ev q[EV_MAX];
    plat_mutex_lock(&ev_lock);
    int n = ev_n;
    for (int i = 0; i < n; i++) q[i] = ev_q[i];
    ev_n = 0;
    plat_mutex_unlock(&ev_lock);
    for (int i = 0; i < n; i++) {
        const Ev &e = q[i];
        switch (e.type) {
        case EV_KEY: io.AddKeyEvent((ImGuiKey)e.a, e.b != 0); break;
        case EV_POS: io.AddMousePosEvent(e.x, e.y); break;
        case EV_BUTTON: io.AddMouseButtonEvent(e.a, e.b != 0); break;
        case EV_WHEEL: io.AddMouseWheelEvent(e.x, e.y); break;
        case EV_FOCUS: io.AddFocusEvent(e.a != 0); break;
        }
    }
}

// ---- gamepad navigation: the Switch buttons of our SDL3 gamepad to ImGui's gamepad keys. Nintendo layout: A (east)
// activates, B (south) cancels.
static const struct { int bit; ImGuiKey key; } pad_keys[] = {
    {B_A, ImGuiKey_GamepadFaceDown}, {B_B, ImGuiKey_GamepadFaceRight}, {B_X, ImGuiKey_GamepadFaceUp},
    {B_Y, ImGuiKey_GamepadFaceLeft}, {B_L, ImGuiKey_GamepadL1}, {B_R, ImGuiKey_GamepadR1},
    {B_ZL, ImGuiKey_GamepadL2}, {B_ZR, ImGuiKey_GamepadR2}, {B_LEFT, ImGuiKey_GamepadDpadLeft},
    {B_UP, ImGuiKey_GamepadDpadUp}, {B_RIGHT, ImGuiKey_GamepadDpadRight}, {B_DOWN, ImGuiKey_GamepadDpadDown},
};
static uint64_t pad_prev;     // buttons at the last frame; a button held when the menu opened counts once released
static float stick_prev[4];   // left stick left, right, up, down as ImGui analog values

static void pad_events(ImGuiIO &io, int reset) {
    uint64_t b;
    int32_t lx, ly;
    int have = input_pad_read(&b, &lx, &ly);
    if (have) io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    else io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    if (reset) {
        pad_prev = b;
        for (float &s : stick_prev) s = 0;
        return;
    }
    for (const auto &k : pad_keys) {
        uint64_t m = 1ull << k.bit;
        if ((b ^ pad_prev) & m) io.AddKeyEvent(k.key, (b & m) != 0);
    }
    pad_prev = b;
    const float dead = 0.3f;
    float v[4] = {-lx / 32767.0f, lx / 32767.0f, ly / 32767.0f, -ly / 32767.0f};
    static const ImGuiKey sk[4] = {ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, ImGuiKey_GamepadLStickUp,
                                   ImGuiKey_GamepadLStickDown};
    for (int i = 0; i < 4; i++) {
        float a = v[i] > dead ? (v[i] - dead) / (1 - dead) : 0;
        if (a != stick_prev[i]) io.AddKeyAnalogEvent(sk[i], a > 0.1f, a);
        stick_prev[i] = a;
    }
}

// ---- ImGui and its Vulkan back end (present path)
static int g_ctx_ok, g_failed;    // context made; set-up failed for good (the menu stays closed)
static int g_vk_ok;               // back end initialised for g_vk_fmt
static VkFormat g_vk_fmt;
static uint64_t g_last_ns;
static uint64_t g_cost_ns, g_cost_frames;   // CPU time of layout and recording while open, logged per 600 frames

static void make_context(uint32_t h) {
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendPlatformName = "totk_menu";
    float scale = h >= 720 ? h / 1080.0f : 720 / 1080.0f;
    ImFontConfig fc;
    fc.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF((void *)roboto_ttf, (int)sizeof roboto_ttf, 26.0f * scale, &fc,
                                   io.Fonts->GetGlyphRangesCyrillic());
    ImGui::StyleColorsDark();
    ImGuiStyle &st = ImGui::GetStyle();
    st.WindowRounding = 6;
    st.FrameRounding = 4;
    st.WindowPadding = ImVec2(18, 14);
    st.ItemSpacing = ImVec2(12, 10);
    st.ScaleAllSizes(scale);
    g_ctx_ok = 1;
}

static void vk_down(void) {
    if (!g_vk_ok) return;
    ImGui_ImplVulkan_Shutdown();
    g_vk_ok = 0;
}

static int vk_up(const MenuTarget *t) {
    if (g_vk_ok && g_vk_fmt == t->format) return 1;
    if (g_vk_ok) {   // the swapchain changed format: pipelines for the new one
        vk_queue_lock();
        vkQueueWaitIdle(t->queue);
        vk_queue_unlock();
        vk_down();
    }
    ImGui_ImplVulkan_InitInfo ii = {};
    ii.ApiVersion = VK_API_VERSION_1_3;
    ii.Instance = t->instance;
    ii.PhysicalDevice = t->phys;
    ii.Device = t->device;
    ii.QueueFamily = t->queue_family;
    ii.Queue = t->queue;
    ii.DescriptorPoolSize = 16;
    ii.MinImageCount = 2;
    ii.ImageCount = 4;   // vertex buffers rotate per recorded frame: more than gfx_window.c's 2 frames in flight
    ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.UseDynamicRendering = true;
    g_vk_fmt = t->format;
    ii.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    ii.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    ii.PipelineRenderingCreateInfo.pColorAttachmentFormats = &g_vk_fmt;
    if (!ImGui_ImplVulkan_Init(&ii)) return 0;
    g_vk_ok = 1;
    vk_queue_lock();   // the font upload submits on the queue and waits for it
    int ok = ImGui_ImplVulkan_CreateFontsTexture();
    vk_queue_unlock();
    if (!ok) vk_down();
    return ok;
}

extern "C" void menu_vk_release(void) {
    if (g_ctx_ok) vk_down();
}

// ---- the test window (TOTK_MENU_TEST): no words, so no catalog entry; the pages and the translation catalog replace it.
// A dimmed frame, a centred window with a progress bar (font glyphs: the percentage) and a colour swatch.
static void layout_test(uint32_t w, uint32_t h, float progress) {
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2((float)w, (float)h), IM_COL32(0, 0, 0, 120));
    ImGui::SetNextWindowPos(ImVec2(w * 0.5f, h * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w * 0.32f, 0), ImGuiCond_Always);
    ImGui::Begin("##menu_test", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::ProgressBar(progress, ImVec2(-1, 0));
    ImGui::Spacing();
    ImGui::ColorButton("##swatch", ImVec4(0.2f, 0.6f, 0.9f, 1), ImGuiColorEditFlags_NoTooltip,
                       ImVec2(ImGui::GetContentRegionAvail().x, 40));
    // Esc on the keyboard, B on the gamepad: close
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) set_open(0);
    ImGui::End();
}

enum { TEST_SHOT_AFTER = 30 };   // frames between the test window opening and its shot

extern "C" int menu_overlay_frame(uint64_t frame, const MenuTarget *t, const char **shot) {
    static uint64_t opened_at;   // frame of the test hook's open; ~0 once its shot is taken
    *shot = NULL;
    if (!g_open.load(std::memory_order_relaxed)) {
        uint64_t at = test_frame();
        if (!at || opened_at || frame < at) return 0;
        opened_at = frame;
        set_open(1);
    }
    if (g_failed) {
        set_open(0);
        return 0;
    }
    uint64_t t0 = plat_now_ns();
    if (!g_ctx_ok) make_context(t->height);
    if (!vk_up(t)) {
        rt_log("menu: Dear ImGui Vulkan back end set-up failed; the menu stays closed");
        g_failed = 1;
        set_open(0);
        return 0;
    }
    ImGuiIO &io = ImGui::GetIO();
    int reset = g_reset.exchange(0);
    if (reset) {
        io.ClearInputKeys();
        io.ClearInputMouse();
        g_last_ns = 0;
    }
    io.DisplaySize = ImVec2((float)t->width, (float)t->height);
    double dt = g_last_ns ? (t0 - g_last_ns) / 1e9 : 1 / 60.0;
    io.DeltaTime = (float)(dt < 1e-4 ? 1e-4 : dt > 0.1 ? 0.1 : dt);
    g_last_ns = t0;
    drain_events(io);
    pad_events(io, reset);
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    uint64_t since = opened_at != ~0ull && frame > opened_at ? frame - opened_at : 0;
    layout_test(t->width, t->height, since >= TEST_SHOT_AFTER ? 1.0f : (float)since / TEST_SHOT_AFTER);
    ImGui::Render();
    g_cost_ns += plat_now_ns() - t0;
    if (opened_at != ~0ull && since >= TEST_SHOT_AFTER) {   // this frame's image goes to a file, then the window closes
        static char path[512];
        const char *dir = cfg_get("TOTK_MENU_TEST_DIR");
        snprintf(path, sizeof path, "%s/menu-test-%llu.png", dir ? dir : ".", (unsigned long long)frame);
        *shot = path;
        opened_at = ~0ull;
        set_open(0);   // the next frame lays out nothing
    }
    return ImGui::GetDrawData()->TotalVtxCount > 0;
}

extern "C" void menu_overlay_draw(VkCommandBuffer cb) {
    uint64_t t0 = plat_now_ns();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cb);
    g_cost_ns += plat_now_ns() - t0;
    if (++g_cost_frames == 600) {
        rt_log("menu: %.3f ms CPU per open frame (layout and recording, mean of 600)", g_cost_ns / 600 / 1e6);
        g_cost_ns = g_cost_frames = 0;
    }
}
