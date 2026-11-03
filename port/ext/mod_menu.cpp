#include "mod_menu.h"

#include "internal.hpp"

#include <kf/platform/coop.h>
#include <verdite/menu_settings.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <utility>
#include <vector>

namespace kf::ext {
namespace {

using verdite::ModMenuKind;
using verdite::ModMenuRow;
using verdite::OptionKind;
using verdite::OptionValue;

OptionKind option_kind(ModMenuKind kind) {
    switch (kind) {
    case ModMenuKind::Toggle: return OptionKind::Toggle;
    case ModMenuKind::Number: return OptionKind::Number;
    case ModMenuKind::Choice: return OptionKind::Choice;
    }
    return OptionKind::Toggle;
}

// The value in the mod's config, or the row's default when that has none of the row's type.
OptionValue value_now(const std::string &mod, const ModMenuRow &row) {
    const auto value = mods_config_value(mod, row.key);
    return value && value->index() == row.fallback.index() ? *value : row.fallback;
}

verdite::OptionState row_state() {
    verdite::OptionState state;
    if (coop_world_in_play()) {
        state.enabled = false;
        state.note = "Mods are off in coop.";
    }
    return state;
}

verdite::OptionRow option_row(const std::string &mod, const ModMenuRow &row) {
    verdite::OptionRow made;
    made.id = mod_menu_option_id(mod, row);
    made.kind = option_kind(row.kind);
    made.label = row.label;
    for (const verdite::ModMenuChoice &choice : row.choices)
        made.choices.push_back({choice.value, choice.label});
    made.min = row.min;
    made.max = row.max;
    made.step = row.step;
    made.format = "%g";
    made.get = [mod, row] { return value_now(mod, row); };
    made.set = [mod, key = row.key](const OptionValue &value) { mods_set_config_value(mod, key, value); };
    made.state = row_state;
    return made;
}

// What the table's rows were built from.
std::string menus_signature() {
    std::string signature;
    for (const verdite::ModInfo &mod : mods_list())
        if (!mod.menu.empty()) {
            signature += mod.id + '\n' + mod.name + '\n';
            for (const ModMenuRow &row : mod.menu)
                signature += row.key + '\t' + row.label + '\t' + std::to_string(row.choices.size()) + '\t' +
                             std::to_string(row.min) + '\t' + std::to_string(row.max) + '\t' + std::to_string(row.step) + '\n';
        }
    return signature;
}

} // namespace

verdite::OptionPage mod_menu_options_page() {
    verdite::OptionPage page{"Mods", {}};
    for (const verdite::ModInfo &mod : mods_list()) {
        if (mod.menu.empty())
            continue;
        verdite::OptionSection section{mod.name, {}};
        for (const ModMenuRow &row : mod.menu)
            section.rows.push_back(option_row(mod.id, row));
        page.sections.push_back(std::move(section));
    }
    return page;
}

void mod_menu_refresh_options() {
    static std::string shown;
    std::string now = menus_signature();
    if (now == shown)
        return;
    shown = std::move(now);
    options_rebuild();
}

std::string mod_menu_option_id(std::string_view mod, const ModMenuRow &row) {
    return "mods." + std::string(mod) + "." + row.key;
}

std::string mod_menu_capitals(std::string_view text) {
    std::string capitals(text);
    std::ranges::transform(capitals, capitals.begin(), [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; });
    return capitals;
}

std::string mod_menu_label(const ModMenuRow &row, Language language) {
    return language == Language::Japanese && !row.japanese.empty() ? row.japanese : mod_menu_capitals(row.label);
}

std::string mod_menu_value(std::string_view mod, const ModMenuRow &row, Language language) {
    const OptionValue value = value_now(std::string(mod), row);
    if (const bool *on = std::get_if<bool>(&value); on && row.kind == ModMenuKind::Toggle) {
        if (language == Language::Japanese)
            return *on ? "オン" : "オフ";
        return *on ? "ON" : "OFF";
    }
    if (row.kind == ModMenuKind::Choice) {
        const auto found = std::ranges::find(row.choices, value, &verdite::ModMenuChoice::value);
        if (found == row.choices.end())
            return {};
        return language == Language::Japanese && !found->japanese.empty() ? found->japanese : mod_menu_capitals(found->label);
    }
    std::array<char, 32> text{};
    std::snprintf(text.data(), text.size(), "%g", std::get<double>(value));
    return text.data();
}

bool mod_menu_rows_enabled() {
    return row_state().enabled;
}

void mod_menu_step(std::string_view mod, const ModMenuRow &row, int delta) {
    verdite::MenuSettings page(options(), {{"mod", {{mod_menu_option_id(mod, row), {}}}}}, platform_features());
    page.open();
    page.step(delta);
    page.keep();
}

} // namespace kf::ext
