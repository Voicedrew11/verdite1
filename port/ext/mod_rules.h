#pragma once
// kf.rules (docs/MODDING.md): each running mod's values for the game's rule tables, laid over the
// originals the game loaded. mods.cpp binds the table and says which mods run; the game reloads a
// table through mods_rules_reapply (kf/platform/mod_events.h).
#include <verdite/mods.h>

#include <span>
#include <string>
#include <string_view>

namespace kf::ext {

// The kf.rules table, for api_tables.
verdite::ModTable rules_table();

// The mod's values come off (it unloaded, reloaded or failed).
void rules_forget(std::string_view id);
// Lays the values of these mods, in load order, over the originals; any other mod's values are
// dropped.
void rules_lay(std::span<const std::string> running_in_load_order);
// Lays the originals alone (the mods are paused); each mod keeps its values for when they resume.
void rules_lay_originals();

} // namespace kf::ext
