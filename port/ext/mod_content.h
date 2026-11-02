#pragma once
// kf.content (docs/CONTENT_LAYER_PLAN.md): the items and floors mods add beside the disc's. Rows are
// keyed by a name the mod gives, so a save keeps meaning the same thing from run to run. Game thread only.
#include "item_menu.h"

#include <verdite/mods.h>

#include <kf/platform/content.h>
#include <kf/platform/saves.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace kf::ext {

// The functions of kf.content that need no floor in play.
std::vector<verdite::ModFunction> mod_content_functions();

// A mod's Lua state is discarded: its items lose their rows, and their ids and stock stay. Its floors
// stay held, so a player standing on one can still reload it.
void mod_content_forget(std::string_view id);

// A mod item's row, or null: none for that id, or the mods are paused.
const ExtraItem *mod_content_item(std::uint8_t id);
// The bytes of a mod's floor's file, for a path as the game names it ("KF/BA/MIXA.DAT"); null for any other.
std::shared_ptr<const std::vector<std::uint8_t>> mod_content_floor_file(const char *path);
// The floor's entrance, if its files are held; they stay after its mod goes.
const ModFloorEntry *mod_content_floor_held(std::uint8_t floor);
// Whether mods may send the player to this floor now: its mod is running.
bool mod_content_floor_offered(std::uint8_t floor);

// The stock and floors beside the card, written and read with a save slot. A read clears both first, so a
// save with no side file holds none of a mod's items.
void mod_content_save_write(SaveSlot slot);
void mod_content_save_read(SaveSlot slot);

} // namespace kf::ext
