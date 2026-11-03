// The SETTINGS page in the game's own menu: which settings it shows, and their
// labels and values as the menu font can spell them (kf/platform/settings_page.h).
// The Japanese text uses only the kana the font has: no small kana, no を.
#include "internal.hpp"
#include "mod_menu.h"
#include <kf/game/menu_text.h>
#include <kf/platform/coop.h>
#include <kf/platform/settings_page.h>
#include <verdite/menu_settings.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <utility>

namespace kf {
namespace {

using verdite::OptionValue;

// What the page's places hold (menu_settings_panel in the game's menu_settings.cpp): rows 22 apart
// from Y 41, a row box of 120 pixels of text, and an option box 110 wide with its 8-pixel ends.
constexpr std::size_t settings_page_max_rows = 8;
constexpr int settings_page_label_px = 120;
constexpr int settings_page_value_px = 94;

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
const Text custom_text{"CUSTOM", "カスタム"};
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

std::vector<ValueText> lines(std::initializer_list<int> factors, std::optional<Text> window) {
    std::vector<ValueText> values;
    for (const int factor : factors) {
        const std::string digits = std::to_string(factor * 240);
        values.push_back({static_cast<double>(factor), {digits + "P", digits}});
    }
    if (window)
        values.front().text = *window;
    return values;
}

// ENEMY TURN's values: the menu font has no '.', so the overlay's quarter steps read as CUSTOM here.
std::vector<ValueText> enemy_turn_values() {
    std::vector<ValueText> values = {{ext::enemy_turn_automatic, {"AUTO", "じどう"}}, {1.0, {"ORIGINAL", "オリジナル"}}};
    for (const int whole : {2, 3, 4})
        values.push_back({static_cast<double>(whole), {std::to_string(whole) + "X", std::to_string(whole) + "ばい"}});
    for (int quarters = 5; quarters < 16; ++quarters)
        if (quarters % 4 != 0)
            values.push_back({quarters / 4.0, custom_text});
    return values;
}

// STICK SENS's values, as percent for the same reason.
std::vector<ValueText> stick_sens_values() {
    std::vector<ValueText> values = {{ext::stick_sens_custom, custom_text}};
    for (const double stop : ext::stick_sens_stops) {
        const std::string percent = std::to_string(std::lround(stop * 100.0));
        values.push_back({stop, {percent, percent}});
    }
    return values;
}

std::vector<OptionValue> stick_sens_page_stops() {
    return {ext::stick_sens_stops.begin(), ext::stick_sens_stops.end()};
}

const std::vector<Page> &pages() {
    static const std::vector<Page> table = [] {
        std::vector<Page> made;
        made.push_back({"display", {"DISPLAY", "がめん"}, {
            {"display.fullscreen", {"FULLSCREEN", "フルスクリーン"}, {}, {}},
            {"display.resolution", {"RESOLUTION", "かいぞうど"},
             lines({0, 1, 2, 3, 4, 5, 6, 7, 8}, Text{"WINDOW", "ウインドウ"}), {}},
            {"display.aspect", {"ASPECT", "がめんのひりつ"},
             {{std::string("4:3"), {"4/3", "4/3"}}, {std::string("16:9"), {"16/9", "16/9"}},
              {std::string("16:10"), {"16/10", "16/10"}}, {std::string("21:9"), {"21/9", "21/9"}},
              {std::string("window"), {"WINDOW", "ウインドウ"}}}, {}},
            {"display.hud_at_edges", {"HUD AT EDGES", "ゲージのいち"}, {}, {}},
            {"display.frame_rate", {"FRAME RATE", "フレームレート"},
             {{20.0, {"ORIGINAL", "オリジナル"}}, {0.0, {"MONITOR", "モニター"}}, {60.0, {"60", "60"}},
              {120.0, {"120", "120"}}, {144.0, {"144", "144"}}, {165.0, {"165", "165"}}, {240.0, {"240", "240"}},
              {static_cast<double>(ext::frame_rate_unlimited), {"UNLIMITED", "むせいげん"}}}, {}},
            {"display.antialiasing", {"ANTIALIASING", "アンチエイリアス"},
             {{0.0, off_text}, {1.0, {"FXAA", "かんい"}}, {2.0, {"4X SSAA", "4ばい"}}, {3.0, {"9X SSAA", "9ばい"}}}, {}},
            {"overlay.run_setup", {"RUN SETUP", "はじめのせつてい"}, {}, {}},
        }});
        made.push_back({"graphics", {"GRAPHICS", "がしつ"}, {
            {"display.wiggle", {"WIGGLE", "ポリゴンのゆれ"}, {}, {}},
            {"display.true_color", {"TRUE COLOUR", "なめらかないろ"}, {}, {}},
            {"display.texture_filter", {"FILTER", "もよう"},
             {{0.0, {"NEAREST", "オリジナル"}}, {1.0, {"BILINEAR", "なめらか"}}, {2.0, {"3 POINT", "3てん"}},
              {3.0, {"SHARP", "するどい"}}}, {}},
            {"display.aniso", {"ANISO", "ななめのもよう"},
             {{0.0, off_text}, {1.0, {"TRILINEAR", "なめらか"}}, {2.0, {"2X", "2ばい"}}, {4.0, {"4X", "4ばい"}},
              {8.0, {"8X", "8ばい"}}, {16.0, {"16X", "16ばい"}}}, {}},
            {"display.pixel_lighting", {"PER PIXEL LIGHT", "ピクセルのひかり"}, {}, {}},
            {"display.ao", {"AO", "すみのかげ"},
             {{0.0, off_text}, {1.0, {"LOW", "ひくい"}}, {2.0, {"MEDIUM", "ふつう"}}, {3.0, {"HIGH", "たかい"}}}, {}},
        }});
        made.push_back({"world", {"WORLD", "せかい"}, {
            {"display.fog", {"FOG", "きり"},
             {{0.0, {"CORNERS", "かど"}}, {1.0, {"DEPTH", "ふかさ"}}, {2.0, {"DISTANCE", "とおさ"}}}, {}},
            {"display.detailed_map", {"DETAILED MAP", "とおくのかべ"}, {}, {}},
            {"display.dynamic_light", {"DYNAMIC LIGHT", "うごくひかり"},
             {{0.0, off_text}, {1.0, on_text}, {2.0, {"SHADOWS", "かげあり"}}}, {}},
            {"display.flame_glow", {"FLAME GLOW", "ほのおのかがやき"}, {}, {}},
        }});
        made.push_back({"gameplay", {"GAMEPLAY", "ゲーム"}, {
            {"gameplay.classic_mode", {"CLASSIC MODE", "オリジナルのまま"}, {}, {}},
            {"gameplay.tick_rate", {"TICK RATE", "せかいのはやさ"},
             {{30.0, {"30", "30"}}, {20.0, {"20", "20"}}, {15.0, {"15", "15"}}, {12.0, {"12", "12"}}}, {}},
            {"gameplay.enemy_turn_speed", {"ENEMY TURN", "てきのふりむき"}, enemy_turn_values(),
             {ext::enemy_turn_automatic, 1.0, 2.0, 3.0, 4.0}},
            {"gameplay.auto_use_keys", {"AUTO KEYS", "かぎのじどうしよう"}, {}, {}},
            {"overlay.item_descriptions", {"ITEM TEXT", "アイテムのせつめい"}, {}, {}},
            {"gameplay.bug_fixes", {"BUG FIXES", "バグのしゆうせい"}, {}, {}},
        }});
        made.push_back({"controls", {"CONTROLS", "そうさ"}, {
            {"analog.style", {"CONTROLS", "そうさ"},
             {{0.0, {"DPAD", "じゆうじ"}}, {1.0, {"3D", "ひとつ"}}, {2.0, {"2D", "ふたつ"}}, {3.0, {"2D ACCEL", "ふたつかそく"}}}, {}},
            {"controls.pad_layout", {"PAD BUTTONS", "ボタンはいち"},
             {{std::string("west"), {"WEST", "かいがい"}}, {std::string("jp"), {"JAPANESE", "にほん"}}}, {}},
            {"analog.invert_pitch", {"INVERT CAMERA", "カメラのはんてん"}, {}, {}},
            {"analog.stick_sens", {"STICK SENS", "レバーのはやさ"}, stick_sens_values(), stick_sens_page_stops()},
        }});
        made.push_back({"sound", {"SOUND", "おと"}, {
            {"audio.volume", {"VOLUME", "おとのおおきさ"}, {},
             {0.0, 10.0, 20.0, 30.0, 40.0, 50.0, 60.0, 70.0, 80.0, 90.0, 100.0}},
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
    float opened_turn_sensitivity = 0, opened_pitch_sensitivity = 0;
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

// A custom stick speed is no value of its row, so the table cannot set it back: the pair is kept here.
void remember_stick_speeds() {
    const AnalogOptions &analog = ext::settings().analog;
    menu_state.opened_turn_sensitivity = analog.turn_sensitivity;
    menu_state.opened_pitch_sensitivity = analog.pitch_sensitivity;
}

void put_back_visit() {
    menu().put_back();
    AnalogOptions &analog = ext::settings().analog;
    if (analog.turn_sensitivity == menu_state.opened_turn_sensitivity &&
        analog.pitch_sensitivity == menu_state.opened_pitch_sensitivity)
        return;
    analog.turn_sensitivity = menu_state.opened_turn_sensitivity;
    analog.pitch_sensitivity = menu_state.opened_pitch_sensitivity;
    ext::apply_control_settings();
    ext::settings_mark_dirty();
}

// Pressing an action puts the visit back first (verdite/menu_settings.h); this does it with the pair.
bool on_action(const verdite::MenuSettingsView &view) {
    const auto selected = std::ranges::find_if(view.rows, &verdite::MenuSettingsRowView::selected);
    if (selected == view.rows.end())
        return false;
    const verdite::OptionRow *option = ext::options().find(selected->option);
    return option && option->kind == verdite::OptionKind::Action;
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
    remember_stick_speeds();
    menu().open();
}
void settings_page_move_row(s32 delta) { menu().move_row(delta); }
void settings_page_move_page(s32 delta) { menu().move_page(delta); }
void settings_page_select_row(s32 row) {
    if (row >= 0)
        menu().select_row(static_cast<std::size_t>(row));
}
bool settings_page_step(s32 delta) {
    if (delta > 0 && on_action(menu().view()))
        put_back_visit();
    return menu().step(delta);
}
bool settings_page_changed() { return menu().changed(); }
void settings_page_save() { menu().keep(); }
void settings_page_discard() { put_back_visit(); }

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

void settings_page_check() {
    int rows = 0, problems = 0;
    const auto report = [&problems](const std::string &what, const std::string &why) {
        ++problems;
        std::printf("[settings page] %s: %s\n", what.c_str(), why.c_str());
    };
    const auto check_text = [&report](const std::string &text, Language language, int limit_px) {
        const std::vector<MenuTextGlyph> glyphs = menu_spell(text, language);
        const auto drawable = static_cast<std::size_t>(std::ranges::count_if(text, [](char c) {
            const auto byte = static_cast<unsigned char>(c);
            return c != ' ' && (byte & 0xC0) != 0x80;   // one count per character, spaces aside
        }));
        if (glyphs.size() != drawable)
            report(text, "the menu font cannot draw all of it");
        else if (menu_text_width(glyphs) > limit_px)
            report(text, "wider than its box");
    };
    for (const Page &page : pages()) {
        if (page.rows.size() > settings_page_max_rows)
            report(page.id, "more rows than the screen fits");
        for (const Row &row : page.rows) {
            ++rows;
            for (const Language language : {Language::English, Language::Japanese}) {
                check_text(in(row.label, language), language, settings_page_label_px);
                for (const ValueText &value : row.values)
                    check_text(in(value.text, language), language, settings_page_value_px);
            }
        }
    }
    std::printf("[settings page] check: %d rows, %d problem(s)\n", rows, problems);
}

bool settings_page_holding() { return menu().changed(); }

void settings_page_abandon() {
    if (menu().changed())
        put_back_visit();
}

} // namespace ext
} // namespace kf
