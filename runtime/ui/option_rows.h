// Settings pages in two panes (ui/option_rows.cpp): option rows on the left, the selected (controller) or hovered (mouse)
// row's description on the right. Each row shows a setting of the cfg table (cfg.h) and changes it through cfg_set: A or
// a click steps it (toggle, next value), left and right step it. A row marks the default value, says "after restart" when
// the setting is read at start, and is greyed out with a note when an environment variable fixes it.
//
//   opt_begin("##screen", scale);
//   opt_toggle("TOTK_FULLSCREEN", UI_MENU_FULLSCREEN, UI_MENU_FULLSCREEN_DESC);
//   opt_end();
//
// key: the setting's TOTK_ name, which must have a cfg_meta row. Present path only (ImGui).
#pragma once
#include "imgui.h"

extern "C" {
#include "ui_text.h"
}

void opt_begin(const char *id, float scale);
void opt_end(void);
// The next page's first row takes the controller's cursor (opening the menu, switching tabs).
void opt_focus_first(void);

// The return value of every row: true when it changed the setting this frame.
bool opt_toggle(const char *key, UiTextId label, UiTextId desc);
// One name per value of the key's cfg_meta values list, in its order. When the key has no fixed default (an empty
// def), names[0] is the automatic choice (the key unset) and names[1..] follow the values.
bool opt_choice(const char *key, UiTextId label, const UiTextId *names, int n, UiTextId desc);
// A choice over a list the caller builds (devices, window sizes): values[i] is written, names[i] shown; "" means the key
// unset. cur: the index in effect, -1 to find cfg_value(key) in values (no match: the value shown as it is).
bool opt_choice_list(const char *key, UiTextId label, const char *const *values, const char *const *names, int n,
                     UiTextId desc, int cur = -1);
// lo..hi in steps of step (left and right held for a second go ten times as fast); fmt formats the value ("%.0f %%").
bool opt_slider(const char *key, UiTextId label, float lo, float hi, UiTextId desc, float step = 1,
                const char *fmt = "%g");

// totk.ini is written a moment after the last change (a held slider would rewrite it every frame) and when the menu
// closes. opt_save_tick: on every frame the menu lays out; force: write now if anything is pending. 1 while a write is
// pending.
int opt_save_tick(int force);
