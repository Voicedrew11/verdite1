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
    float top_speed = 1.0f;
    float game_speed = 1.0f;
    bool paused = false;
    bool music_while_paused = false;   // the music plays on while this mod pauses the game
    bool takes_mouse = false;          // the cursor shows and the game gets no mouse input
};

struct ModPlayCombined {
    PlayOverrides overrides;
    float game_speed = 1.0f;
    bool paused = false;
    // Paused only by mods that keep the music: one that pauses without it silences the game.
    bool music_while_paused = false;
    bool takes_mouse = false;
};

[[nodiscard]] ModPlayCombined combine_mod_play(std::span<const ModPlay> mods);

} // namespace kf::ext
