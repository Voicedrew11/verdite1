#ifndef KF_PLATFORM_SETTINGS_PAGE_H
#define KF_PLATFORM_SETTINGS_PAGE_H
// The port's settings as pages in the game's own menu (SYSTEM > SETTINGS),
// through verdite/menu_settings.h over the overlay's option table. The game's
// loop moves and steps with the pad and draws settings_page_view().
//
// Text is in the menu font's terms for the language asked: English is
// capitals, digits, '/' and spaces; Japanese is kana without small kana,
// 'ー', digits and spaces (kf/game/menu_text.h spells both).
//
// Steps apply live. While they differ from what the page opened with, the
// settings file is not written: save keeps them, discard puts them back, and
// quitting with the page open puts them back too.
#include <kf/lib/types.h>
#include <kf/platform/language.h>

#include <string>
#include <vector>

namespace kf {

struct SettingsPageRow {
    std::string label;
    std::string value;
    bool enabled = true;
    bool selected = false;
};

struct SettingsPageView {
    std::string title;
    s32 page_number = 1;   // from 1
    s32 page_count = 1;
    std::vector<SettingsPageRow> rows;
};

void settings_page_open();
void settings_page_move_row(s32 delta);
void settings_page_move_page(s32 delta);
void settings_page_select_row(s32 row);
// The selected row's value delta stops on, round its ends. False when the row
// cannot change now (dimmed).
bool settings_page_step(s32 delta);
bool settings_page_changed();
void settings_page_save();
void settings_page_discard();
SettingsPageView settings_page_view(Language language);

// The top menu's row that opens the page, in the language asked.
std::string settings_page_entry_text(Language language);
// The leaving question's two answers, in the language asked.
std::string settings_page_save_text(Language language);
std::string settings_page_discard_text(Language language);

} // namespace kf

#endif // KF_PLATFORM_SETTINGS_PAGE_H
