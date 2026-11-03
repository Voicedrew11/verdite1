#pragma once
// A mod's own menu (mod.json's "menu", verdite/mods.h) as rows of the settings: the option table holds
// them as mods.<id>.<key>, so F1's settings, the title's MODS page, the pause menu's settings and the
// agent channel all reach the same values, in the mod's config.
#include <kf/platform/language.h>
#include <verdite/mods.h>
#include <verdite/options.h>

#include <string>
#include <string_view>

namespace kf::ext {

// The Mods page of the option table: a section per mod that has a menu. Its rows are fixed while
// mods are off in coop.
verdite::OptionPage mod_menu_options_page();
// Rebuilds the option table when the mods' menus have changed. Holds no view of it: call it only where
// no one holds one, between frames.
void mod_menu_refresh_options();

std::string mod_menu_option_id(std::string_view mod, const verdite::ModMenuRow &row);
// A row's label and its value now, in the menu font's spelling.
std::string mod_menu_label(const verdite::ModMenuRow &row, Language language);
std::string mod_menu_value(std::string_view mod, const verdite::ModMenuRow &row, Language language);
// Capitals, as the menu font has no lower case.
std::string mod_menu_capitals(std::string_view text);
// Whether the rows can be changed now (not in coop), and a step of one (-1 or 1), kept at once.
bool mod_menu_rows_enabled();
void mod_menu_step(std::string_view mod, const verdite::ModMenuRow &row, int delta);

} // namespace kf::ext
