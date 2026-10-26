// The in-game menu (ui/menu.cpp, Dear ImGui) and its seams to the platform side: the window and its input (ui/menu_win.cpp
// on Windows; an SDL3 window gets its own adapter later), the gamepad (input_pad_read, input.h) and the present path
// (gfx_window.c). The menu core calls no window system: events come in through menu_event_*, the drawing happens in the
// present path's command buffer.
//
// No pages yet (the tabs and options are still being agreed): the menu opens only for the test hook
// TOTK_MENU_TEST=<frame>, which opens a text-free test window at that presented frame, saves one swapchain shot 30
// frames later into TOTK_MENU_TEST_DIR (default: the working directory) and closes it. While the test hook is set, F1 or
// Esc on the keyboard and Minus and Plus held together on the gamepad also open and close it. While it is open the game
// gets no input (input_menu, input.h) and keeps running; the cursor is released from the mouse camera.
//
// Threads: the event functions are called by the window thread, menu_pad_combo by whichever thread polls the gamepad, the
// overlay functions by the present path under its lock. ImGui itself runs on the present path only.
#pragma once
#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- state, any thread
int menu_available(void);                 // the menu can open (for now only with the test hook TOTK_MENU_TEST)
int menu_is_open(void);
void menu_toggle(void);                   // F1 / Esc on the keyboard
void menu_pad_combo(int held);            // Minus and Plus held together on the gamepad (toggles once per hold)

// ---- events from the window adapter, used only while the menu is open. key: an ImGuiKey; coordinates in pixels of the
// window's client area, which is the swapchain image.
void menu_event_key(int key, int down);
void menu_event_mouse_pos(float x, float y);
void menu_event_mouse_button(int button, int down);   // 0 left, 1 right, 2 middle
void menu_event_wheel(float dx, float dy);
void menu_event_focus(int focused);

// ---- the overlay, drawn into the swapchain image after the game image (gfx_window.c, present lock held)
typedef struct {
    VkInstance instance;
    VkPhysicalDevice phys;
    VkDevice device;
    uint32_t queue_family;
    VkQueue queue;                        // submissions take vk_queue_lock
    VkFormat format;                      // of the swapchain image, a colour attachment
    uint32_t width, height;
} MenuTarget;
// Lays out this frame's overlay; 1 when there is something to draw. frame: the presented frame (0 unknown). *shot: a
// path when the test hook wants this frame's swapchain image saved, else NULL. The first open starts ImGui's Vulkan
// back end and uploads the font. With the menu closed it costs two loads and a compare.
int menu_overlay_frame(uint64_t frame, const MenuTarget *t, const char **shot);
// Records the frame menu_overlay_frame laid out into cb, which is inside dynamic rendering on the image.
void menu_overlay_draw(VkCommandBuffer cb);
void menu_vk_release(void);               // before the device goes away (gfx_window_release_vk)

// ---- what the menu asks of the platform side (gfx_window.c)
void gfx_window_apply_settings(void);     // TOTK_FULLSCREEN or TOTK_WINDOW_SIZE changed: the window follows
void gfx_window_request_close(void);      // as the window's close button: the process ends

#ifdef __cplusplus
}
#endif
