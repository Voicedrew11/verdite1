#ifndef VERDITE_MENU_SETTINGS_H
#define VERDITE_MENU_SETTINGS_H
// The settings as pages inside a game's own menu, stepped through with a pad:
// Up and Down pick a row, Left and Right step its value, the shoulders turn
// the page. Game-agnostic and holding no text: the game names the rows by
// option id, draws them with its own routines and spells their labels and
// values in its own font and language.
//
// A step is set live through the option table, so it can be seen at once, and
// is staged: keep() makes the visit's changes stay, put_back() sets every
// changed option back to its value at open(). Between open() and either of
// them, changed() says whether to ask the player before leaving.
//
// Rows the platform lacks a feature for are left out, and rows with fewer than
// two stops, then empty pages. A row the table does not show now, or has
// disabled, stays in place, dimmed, so the rows do not move under the cursor.
#include "verdite/options.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace verdite {

struct MenuSettingsRow {
    std::string option;               // an option table id ("display.aspect")
    std::vector<OptionValue> stops;   // the values Left and Right walk; empty: a Choice's choices, a Toggle's off and on, a Number's steps
};

struct MenuSettingsPage {
    std::string id;   // the game's name for it, to find its title ("display")
    std::vector<MenuSettingsRow> rows;
};

// A row as the game draws it now.
struct MenuSettingsRowView {
    std::string_view option;
    OptionValue value;
    bool enabled = true;   // false: drawn dimmed, steps refused
    bool selected = false;
};

struct MenuSettingsView {
    std::string_view page;   // its id
    std::size_t page_index = 0, page_count = 0;
    std::vector<MenuSettingsRowView> rows;
};

class MenuSettings {
public:
    // The table is borrowed and must outlive this.
    MenuSettings(OptionTable &table, std::vector<MenuSettingsPage> pages, PlatformFeatures has);

    // Starts a visit: the page last shown, its first row, nothing staged.
    void open();

    void move_row(int delta);    // wraps
    void move_page(int delta);   // wraps, to the page's first row

    // Sets the selected row's value |delta| stops on, going round the ends;
    // a value between stops goes to the nearer stop on that side. False, with
    // nothing changed, for a disabled row or one with a single stop.
    bool step(int delta);

    // The cursor on a row of the current page, as a pointer chose it.
    void select_row(std::size_t row);

    bool changed() const;   // a staged value differs from its value at open()
    void keep();            // the changes stay; the visit ends
    void put_back();        // every changed option set back; the visit ends

    MenuSettingsView view() const;

private:
    OptionTable *table_;
    std::vector<MenuSettingsPage> pages_;
    std::size_t page_ = 0, row_ = 0;
    std::vector<std::pair<std::string, OptionValue>> opened_with_;   // option, its value at its first step
};

} // namespace verdite

#endif // VERDITE_MENU_SETTINGS_H
