#pragma once
// kf.automap and kf.online (docs/MODDING.md): the floor as the automap's snapshot holds it
// (kf/platform/automap.h), what the player has seen of it, and the other players in the online
// room, read-only, so a mod can draw a map.
#include <verdite/mods.h>

namespace kf::ext {

verdite::ModTable automap_table();
verdite::ModTable online_table();

} // namespace kf::ext
