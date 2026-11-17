#pragma once
// The rule behind kf/platform/creature_feedback.h for one actor slot, with no game state.
#include <kf/platform/creature_feedback.h>

namespace kf::ext {

// The red at the blow, then gone over the flash; the body held a moment once its death ends, then
// faded. 60 Hz vblanks, the clock the game's updates are counted in (world_update_vblanks).
inline constexpr int hit_flash_vblanks = 18;
inline constexpr int body_hold_vblanks = 18;
inline constexpr int body_fade_vblanks = 54;

struct CreatureFeedbackOptions {
    bool hit_flash = true;
    bool death_fade = true;
};

// What is remembered of a slot between updates.
struct CreatureTrack {
    bool known = false;          // seen awake, with this occupant
    u8 definition = 0;
    u16 health = 0;
    int flash_left_vblanks = 0;
    bool body = false;           // its death was seen while it was awake
    int dead_vblanks = 0;        // since then
};

struct CreatureStep {
    CreatureTrack track;
    CreatureLook look;
};

// The slot after an update elapsed_vblanks long that found it as seen.
CreatureStep creature_step(const CreatureTrack &before, const CreatureSighting &seen, int elapsed_vblanks,
                           const CreatureFeedbackOptions &options);

} // namespace kf::ext
