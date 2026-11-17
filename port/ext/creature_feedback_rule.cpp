#include "creature_feedback_rule.h"

#include <algorithm>

namespace kf::ext {
namespace {

constexpr s32 red_all_q12 = 4096;
constexpr int keep_all = 255;

int keep_of(const CreatureTrack &track, const CreatureFeedbackOptions &options) {
    if (!options.death_fade || !track.body || track.dead_vblanks <= body_hold_vblanks)
        return keep_all;
    const int fading = track.dead_vblanks - body_hold_vblanks;
    return std::max(0, keep_all - keep_all * fading / body_fade_vblanks);
}

CreatureLook look_of(const CreatureTrack &track, const CreatureFeedbackOptions &options) {
    const int keep = keep_of(track, options);
    if (keep == 0)
        return {};
    CreatureLook look;
    look.drawn = true;
    look.keep = static_cast<u8>(keep);
    if (options.hit_flash)
        look.red_q12 = red_all_q12 * track.flash_left_vblanks / hit_flash_vblanks;
    return look;
}

// Its slot no longer drawn by the game: only a body it was seen to leave, still fading.
CreatureStep step_asleep(const CreatureTrack &before, const CreatureSighting &seen, int elapsed_vblanks,
                         const CreatureFeedbackOptions &options) {
    if (!options.death_fade || !before.body || !seen.dead || seen.definition != before.definition)
        return {};
    CreatureTrack track = before;
    track.flash_left_vblanks = std::max(0, track.flash_left_vblanks - elapsed_vblanks);
    track.dead_vblanks += elapsed_vblanks;
    const CreatureLook look = look_of(track, options);
    if (!look.drawn)
        return {};
    return {track, look};
}

} // namespace

CreatureStep creature_step(const CreatureTrack &before, const CreatureSighting &seen, int elapsed_vblanks,
                           const CreatureFeedbackOptions &options) {
    elapsed_vblanks = std::max(elapsed_vblanks, 0);
    if (!seen.awake)
        return step_asleep(before, seen, elapsed_vblanks, options);

    CreatureTrack track = before;
    const bool new_occupant = !before.known || before.definition != seen.definition || (before.body && !seen.dead);
    if (new_occupant) {
        track = {};
        track.known = true;
        track.definition = seen.definition;
    } else {
        track.flash_left_vblanks = std::max(0, track.flash_left_vblanks - elapsed_vblanks);
        if (seen.health < track.health)
            track.flash_left_vblanks = hit_flash_vblanks;
    }
    track.health = seen.health;
    if (seen.dead && track.body)
        track.dead_vblanks += elapsed_vblanks;
    track.body = track.body || seen.dead;
    return {track, look_of(track, options)};
}

} // namespace kf::ext
