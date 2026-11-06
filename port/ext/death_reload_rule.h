#pragma once
// Which save a death reloads (kf/platform/death_reload.h): the rule, with no game state.
#include <kf/platform/saves.h>

#include <optional>

namespace kf::ext {

struct DeathReloadFacts {
    bool setting_on = false;               // the effective setting: off in Classic mode
    bool coop_world = false;               // a coop room has its own death
    std::optional<SaveSlot> last_save;     // made or loaded this session
};

// The save a death reloads, or none when the game's own restart runs. Only a slot or the quick
// save: the coop aside save holds a game set aside, not one in play.
std::optional<SaveSlot> death_reload_slot(const DeathReloadFacts &facts);

} // namespace kf::ext
