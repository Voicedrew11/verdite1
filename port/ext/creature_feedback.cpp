#include <kf/platform/creature_feedback.h>
#include <kf/platform/input.h>

#include "creature_feedback_rule.h"

#include <vector>

namespace kf {
namespace {

std::vector<ext::CreatureTrack> tracks;

} // namespace

CreatureLook creature_look(std::size_t slot, const CreatureSighting &seen) {
    if (slot >= tracks.size())
        tracks.resize(slot + 1);
    const ext::CreatureStep step = ext::creature_step(tracks[slot], seen, world_update_vblanks,
                                                      {creature_hit_flash, creature_death_fade});
    tracks[slot] = step.track;
    return step.look;
}

void creature_feedback_forget() {
    tracks.clear();
}

} // namespace kf
