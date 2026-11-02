#pragma once
// kf.scene (docs/MODDING.md): a mod's lines and markers in the world, drawn with the game's faces
// from the picture after the next update. Each mod's shapes are kept until the next update.
#include <verdite/mods.h>

#include <string_view>
#include <vector>

namespace kf::ext {

std::vector<verdite::ModFunction> mod_scene_functions();

// Drops one mod's shapes (unloaded, held, or not running), or every mod's (mods off, or the next update).
void mod_scene_clear_mod(std::string_view id);
void mod_scene_clear_all();

} // namespace kf::ext
