// Text the host shows the player (runtime/ui_text.c): one catalog, keyed by stable ids, in the game's language.
#pragma once

typedef enum {
    UI_SWKBD_TITLE,          // caption of the text-entry window
    UI_SWKBD_PROMPT,         // prompt when the game gives no header, guide or sub text (TotK names a horse)
    UI_SWKBD_DEFAULT_NAME,   // text the entry starts with when the game gives none, so a controller can confirm it
    UI_SWKBD_OK,
    UI_SWKBD_CANCEL,
    UI_SWKBD_PAD_HINT,       // how a controller answers
    UI_TEXT_COUNT
} UiTextId;

// UTF-8 text for id in the language of ov_language(); English when the catalog has no row for that language.
const char *ui_text(UiTextId id);
// The same for an explicit catalog language ("en", "ja", "fr", "de", "it", "es", "nl", "pt", "ru", "zh-Hans",
// "zh-Hant", "ko"), for the probe.
const char *ui_text_in(const char *lang, UiTextId id);
// The catalog language for a Switch language code ("en-GB" -> "en", "zh-CN" -> "zh-Hans", ...).
const char *ui_text_lang(const char *switch_code);
