// The SETTINGS page in the game's own menu: which settings it shows, and their
// labels and values as the menu font can spell them (kf/platform/settings_page.h).
// The Japanese text uses only the kana the font has: no small kana, no を.
#include "internal.hpp"
#include "mod_menu.h"
#include <kf/platform/coop.h>
#include <kf/platform/settings_page.h>
#include <verdite/menu_settings.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace kf {
namespace {

using verdite::OptionValue;

struct Text {
    std::string en;
    std::string ja;
};

struct ValueText {
    OptionValue value;
    Text text;
};

struct Row {
    const char *option;
    Text label;
    std::vector<ValueText> values;     // empty: ON/OFF for a toggle, the number for a number
    std::vector<OptionValue> stops;    // empty: the row's own
};

struct Page {
    const char *id;
    Text title;
    std::vector<Row> rows;
};

const Text on_text{"ON", "オン"};
const Text off_text{"OFF", "オフ"};
const Text entry_text{"PORT SETTINGS", "カスタム"};
const Text save_text{"SAVE CHANGES", "ほぞんする"};
const Text discard_text{"DISCARD", "もとにもどす"};

std::vector<ValueText> times(std::initializer_list<int> factors, std::optional<Text> one = std::nullopt) {
    std::vector<ValueText> values;
    for (const int factor : factors) {
        const std::string digits = std::to_string(factor);
        values.push_back({static_cast<double>(factor), {digits + "X", digits + "ばい"}});
    }
    if (one)
        values.front().text = *one;
    return values;
}

const std::vector<Page> &pages() {
    static const std::vector<Page> table = [] {
        std::vector<Page> made;
        made.push_back({"display", {"DISPLAY", "がめん"}, {
            {"display.fullscreen", {"FULLSCREEN", "フルスクリーン"}, {}, {}},
            {"display.aspect", {"ASPECT", "がめんのひりつ"},
             {{std::string("4:3"), {"4/3", "4/3"}}, {std::string("16:9"), {"16/9", "16/9"}},
              {std::string("16:10"), {"16/10", "16/10"}}, {std::string("21:9"), {"21/9", "21/9"}},
              {std::string("window"), {"WINDOW", "ウインドウ"}}}, {}},
            {"display.hud_at_edges", {"HUD AT EDGES", "ゲージのいち"}, {}, {}},
            {"display.resolution", {"RESOLUTION", "かいぞうど"},
             times({0, 1, 2, 3, 4, 5, 6, 7, 8}, Text{"WINDOW", "ウインドウ"}), {}},
            {"display.frame_rate", {"FRAME RATE", "フレームレート"},
             {{20.0, {"ORIGINAL", "オリジナル"}}, {0.0, {"MONITOR", "モニター"}}, {60.0, {"60", "60"}},
              {120.0, {"120", "120"}}, {144.0, {"144", "144"}}, {165.0, {"165", "165"}}, {240.0, {"240", "240"}},
              {-1.0, {"UNLIMITED", "むせいげん"}}}, {}},
            {"display.antialiasing", {"ANTIALIASING", "アンチエイリアス"},
             {{0.0, off_text}, {1.0, {"FXAA", "かんい"}}, {2.0, {"4X SSAA", "4ばい"}}, {3.0, {"9X SSAA", "9ばい"}}}, {}},
        }});
        made.push_back({"picture", {"PICTURE", "がしつ"}, {
            {"display.wiggle", {"WIGGLE", "ポリゴンのゆれ"}, {}, {}},
            {"display.true_color", {"TRUE COLOUR", "なめらかないろ"}, {}, {}},
            {"display.texture_filter", {"TEXTURES", "もよう"},
             {{0.0, {"NEAREST", "オリジナル"}}, {1.0, {"BILINEAR", "なめらか"}}, {2.0, {"3 POINT", "3てん"}},
              {3.0, {"SHARP", "するどい"}}}, {}},
            {"display.mipmaps", {"MIPMAPS", "とおくのもよう"}, {}, {}},
            {"display.anisotropy", {"ANISOTROPY", "ななめのもよう"}, times({1, 2, 4, 8, 16}, off_text), {}},
            {"display.fog", {"FOG", "きり"},
             {{0.0, {"CORNERS", "かど"}}, {1.0, {"DEPTH", "ふかさ"}}, {2.0, {"DISTANCE", "とおさ"}}}, {}},
        }});
        made.push_back({"light", {"LIGHT", "ひかり"}, {
            {"display.pixel_lighting", {"PIXEL LIGHT", "ピクセルのひかり"}, {}, {}},
            {"display.dynamic_lights", {"DYNAMIC LIGHT", "うごくひかり"}, {}, {}},
            {"display.shadows", {"SHADOWS", "かげ"}, {}, {}},
            {"display.flame_glow", {"FLAME GLOW", "ほのおのかがやき"}, {}, {}},
            {"display.ambient_occlusion", {"OCCLUSION", "すみのかげ"}, {}, {}},
            {"display.ao_quality", {"OCCL QUALITY", "すみのかげのしつ"},
             {{0.0, {"LOW", "ひくい"}}, {1.0, {"MEDIUM", "ふつう"}}, {2.0, {"HIGH", "たかい"}}}, {}},
        }});
        made.push_back({"gameplay", {"GAMEPLAY", "ゲーム"}, {
            {"gameplay.classic_mode", {"CLASSIC MODE", "オリジナルのまま"}, {}, {}},
            {"gameplay.tick_rate", {"TICK RATE", "せかいのはやさ"},
             {{30.0, {"30", "30"}}, {20.0, {"20", "20"}}, {15.0, {"15", "15"}}, {12.0, {"12", "12"}}}, {}},
            {"gameplay.enemy_turn_auto", {"ENEMY TURN", "てきのふりむき"},
             {{true, {"AUTO", "じどう"}}, {false, {"FIXED", "こてい"}}}, {}},
            {"gameplay.auto_use_keys", {"AUTO KEYS", "かぎのじどうしよう"}, {}, {}},
            {"audio.volume", {"VOLUME", "おとのおおきさ"}, {},
             {0.0, 10.0, 20.0, 30.0, 40.0, 50.0, 60.0, 70.0, 80.0, 90.0, 100.0}},
        }});
        made.push_back({"controls", {"CONTROLS", "そうさ"}, {
            {"controls.pad_layout", {"PAD BUTTONS", "ボタンはいち"},
             {{std::string("west"), {"WEST", "かいがい"}}, {std::string("jp"), {"JAPANESE", "にほん"}}}, {}},
            {"controls.menu_pointer", {"MOUSE IN MENUS", "マウスでえらぶ"}, {}, {}},
            {"analog.enabled", {"ANALOG", "アナログ"}, {}, {}},
            {"analog.sticks", {"STICKS", "レバー"},
             {{0.0, {"TWIN", "ふたつ"}}, {1.0, {"ONE", "ひとつ"}}}, {}},
        }});
        return made;
    }();
    return table;
}

// A page of a running mod's own menu (mod.json's "menu"), after the game's: its labels and values are
// the mod's, read from its rows.
struct ModPage {
    std::string page_id;
    std::string mod;
    std::string title;
    std::vector<verdite::ModMenuRow> rows;
};

// Off in coop, where mods do not run.
std::vector<ModPage> running_mod_pages() {
    std::vector<ModPage> made;
    if (coop_world_in_play())
        return made;
    for (const verdite::ModInfo &mod : ext::mods_list())
        if (mod.state == verdite::ModState::Running && !mod.held && !mod.menu.empty())
            made.push_back({"mod." + mod.id, mod.id, ext::mod_menu_capitals(mod.name), mod.menu});
    return made;
}

struct MenuState {
    std::optional<verdite::MenuSettings> settings;
    std::vector<ModPage> mod_pages;
    std::string mod_pages_signature;
};
MenuState menu_state;

std::string signature_of(const std::vector<ModPage> &mod_pages) {
    std::string signature;
    for (const ModPage &page : mod_pages) {
        signature += page.page_id + '\n' + page.title + '\n';
        for (const verdite::ModMenuRow &row : page.rows)
            signature += row.key + '\n';
    }
    return signature;
}

verdite::MenuSettings &build_menu() {
    std::vector<verdite::MenuSettingsPage> menu_pages;
    for (const Page &page : pages()) {
        verdite::MenuSettingsPage menu_page{page.id, {}};
        for (const Row &row : page.rows)
            menu_page.rows.push_back({row.option, row.stops});
        menu_pages.push_back(std::move(menu_page));
    }
    for (const ModPage &page : menu_state.mod_pages) {
        verdite::MenuSettingsPage menu_page{page.page_id, {}};
        for (const verdite::ModMenuRow &row : page.rows)
            menu_page.rows.push_back({ext::mod_menu_option_id(page.mod, row), {}});
        menu_pages.push_back(std::move(menu_page));
    }
    return menu_state.settings.emplace(ext::options(), std::move(menu_pages), ext::platform_features());
}

verdite::MenuSettings &menu() {
    if (menu_state.settings)
        return *menu_state.settings;
    return build_menu();
}

// Before a visit, as mods may have changed since the last. A visit's changes are kept or put back
// before the next open, so nothing staged is lost.
void keep_mod_pages_current() {
    ext::mod_menu_refresh_options();
    std::vector<ModPage> now = running_mod_pages();
    std::string signature = signature_of(now);
    if (menu_state.settings && signature == menu_state.mod_pages_signature)
        return;
    menu_state.mod_pages = std::move(now);
    menu_state.mod_pages_signature = std::move(signature);
    build_menu();
}

const ModPage *find_mod_page(std::string_view id) {
    const auto found = std::ranges::find(menu_state.mod_pages, id, &ModPage::page_id);
    return found == menu_state.mod_pages.end() ? nullptr : &*found;
}

// The row a mod page's view names: its option id is mods.<mod>.<key>.
const verdite::ModMenuRow *find_mod_row(const ModPage &page, std::string_view option) {
    const auto found = std::ranges::find_if(page.rows, [&page, option](const verdite::ModMenuRow &row) {
        return ext::mod_menu_option_id(page.mod, row) == option;
    });
    return found == page.rows.end() ? nullptr : &*found;
}

void add_mod_page_rows(SettingsPageView &made, const ModPage &page, const verdite::MenuSettingsView &view, Language language) {
    made.title = page.title;
    for (const verdite::MenuSettingsRowView &row_view : view.rows)
        if (const verdite::ModMenuRow *row = find_mod_row(page, row_view.option))
            made.rows.push_back({ext::mod_menu_label(*row, language), ext::mod_menu_value(page.mod, *row, language),
                                 row_view.enabled, row_view.selected});
}

const std::string &in(const Text &text, Language language) { return language == Language::Japanese ? text.ja : text.en; }

const Page *find_page(std::string_view id) {
    for (const Page &page : pages())
        if (id == page.id)
            return &page;
    return nullptr;
}

const Row *find_row(const Page &page, std::string_view option) {
    for (const Row &row : page.rows)
        if (option == row.option)
            return &row;
    return nullptr;
}

std::string value_text(const Row &row, const OptionValue &value, Language language) {
    for (const ValueText &known : row.values)
        if (known.value == value)
            return in(known.text, language);
    if (const bool *on = std::get_if<bool>(&value))
        return in(*on ? on_text : off_text, language);
    if (const double *number = std::get_if<double>(&value))
        return std::to_string(std::lround(*number));
    return {};
}

} // namespace

void settings_page_open() {
    keep_mod_pages_current();
    menu().open();
}
void settings_page_move_row(s32 delta) { menu().move_row(delta); }
void settings_page_move_page(s32 delta) { menu().move_page(delta); }
void settings_page_select_row(s32 row) {
    if (row >= 0)
        menu().select_row(static_cast<std::size_t>(row));
}
bool settings_page_step(s32 delta) { return menu().step(delta); }
bool settings_page_changed() { return menu().changed(); }
void settings_page_save() { menu().keep(); }
void settings_page_discard() { menu().put_back(); }

SettingsPageView settings_page_view(Language language) {
    const verdite::MenuSettingsView view = menu().view();
    SettingsPageView made;
    made.page_number = static_cast<s32>(view.page_index) + 1;
    made.page_count = static_cast<s32>(view.page_count);
    if (const ModPage *mod_page = find_mod_page(view.page)) {
        add_mod_page_rows(made, *mod_page, view, language);
        return made;
    }
    const Page *page = find_page(view.page);
    if (!page)
        return {};
    made.title = in(page->title, language);
    for (const verdite::MenuSettingsRowView &row_view : view.rows) {
        const Row *row = find_row(*page, row_view.option);
        if (!row)
            continue;
        made.rows.push_back({in(row->label, language), value_text(*row, row_view.value, language), row_view.enabled,
                             row_view.selected});
    }
    return made;
}

std::string settings_page_entry_text(Language language) { return in(entry_text, language); }
std::string settings_page_save_text(Language language) { return in(save_text, language); }
std::string settings_page_discard_text(Language language) { return in(discard_text, language); }

namespace ext {

bool settings_page_holding() { return menu().changed(); }

void settings_page_abandon() {
    if (menu().changed())
        menu().put_back();
}

} // namespace ext
} // namespace kf
