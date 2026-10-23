// Catalog of the text the host shows the player (ui_text.h). Rows follow the game's text languages; a new string gets
// an id in ui_text.h and a column in every row (an empty entry falls back to English).
#include <string.h>

#include "ov.h"
#include "ui_text.h"

typedef struct { const char *lang, *text[UI_TEXT_COUNT]; } Row;

// Columns: TITLE, PROMPT, DEFAULT_NAME (at most 9 UTF-16 units, no '@' '%' '\'), OK, CANCEL, PAD_HINT.
static const Row rows[] = {
    {"en", {"Text entry", "Enter a name", "Horse", "OK", "Cancel", "Controller: A = OK, B = Cancel"}},
    {"ja", {"文字入力", "名前を入力してください", "ウマ", "OK", "キャンセル", "コントローラー: A = OK、B = キャンセル"}},
    {"fr", {"Saisie de texte", "Entrez un nom", "Cheval", "OK", "Annuler", "Manette : A = OK, B = Annuler"}},
    {"de", {"Texteingabe", "Gib einen Namen ein", "Pferd", "OK", "Abbrechen", "Controller: A = OK, B = Abbrechen"}},
    {"it", {"Inserimento testo", "Inserisci un nome", "Cavallo", "OK", "Annulla", "Controller: A = OK, B = Annulla"}},
    {"es", {"Introducir texto", "Introduce un nombre", "Caballo", "Aceptar", "Cancelar",
            "Mando: A = Aceptar, B = Cancelar"}},
    {"nl", {"Tekstinvoer", "Voer een naam in", "Paard", "OK", "Annuleren", "Controller: A = OK, B = Annuleren"}},
    {"pt", {"Inserir texto", "Introduz um nome", "Cavalo", "OK", "Cancelar", "Comando: A = OK, B = Cancelar"}},
    {"ru", {"Ввод текста", "Введите имя", "Лошадь", "ОК", "Отмена", "Контроллер: A — ОК, B — отмена"}},
    {"zh-Hans", {"文字输入", "请输入名字", "马儿", "确定", "取消", "手柄：A 确定，B 取消"}},
    {"zh-Hant", {"文字輸入", "請輸入名字", "馬兒", "確定", "取消", "手把：A 確定，B 取消"}},
    {"ko", {"문자 입력", "이름을 입력하세요", "말", "확인", "취소", "컨트롤러: A 확인, B 취소"}},
};

const char *ui_text_lang(const char *code) {
    static const struct { const char *code, *lang; } map[] = {
        {"en-US", "en"}, {"en-GB", "en"}, {"fr-CA", "fr"}, {"es-419", "es"}, {"pt-BR", "pt"},
        {"zh-CN", "zh-Hans"}, {"zh-TW", "zh-Hant"},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (!strcmp(code, map[i].code)) return map[i].lang;
    return code;
}

const char *ui_text_in(const char *lang, UiTextId id) {
    if ((unsigned)id >= UI_TEXT_COUNT) return "";
    for (size_t i = 0; i < sizeof rows / sizeof *rows; i++)
        if (!strcmp(rows[i].lang, lang) && rows[i].text[id] && *rows[i].text[id]) return rows[i].text[id];
    return rows[0].text[id];
}

const char *ui_text(UiTextId id) { return ui_text_in(ui_text_lang(ov_language()), id); }
