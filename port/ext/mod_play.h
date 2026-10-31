#pragma once
// Each mod's own kf.play and kf.game values, and how they combine into what the game sees: a
// switch is on when any mod turns it on, speeds multiply and are held to 0.25..8. A mod's values
// are dropped when it unloads or fails, so a broken mod cannot leave noclip on.
#include <kf/platform/play_overrides.h>

#include <span>

namespace kf::ext {

constexpr float min_mod_speed = 0.25f;
constexpr float max_mod_speed = 8.0f;

// One mod's values, as it last set them.
struct ModPlay {
    bool noclip = false;
    bool invincible = false;
    bool peaceful = false;
    float move_speed = 1.0f;
    float turn_speed = 1.0f;
    float game_speed = 1.0f;
    bool paused = false;
};

struct ModPlayCombined {
    PlayOverrides overrides;
    float game_speed = 1.0f;
    bool paused = false;
};

[[nodiscard]] ModPlayCombined combine_mod_play(std::span<const ModPlay> mods);

} // namespace kf::ext
