#pragma once
// Hits across machines in a coop room (docs/COOP_PLAN.md, "Hits are decided by whoever feels
// them"). A player's hit on an actor another machine runs, and its aim, go to that machine, which
// applies them; a kill by any player's hit gives every player on the floor its experience. The
// floor's owner is coop_floor_play.h's. Outside a room nothing here acts.
#include "coop_message.h"

#include <cstdint>

namespace kf::ext {

// Whether a message is one of these (CoopActorHit to CoopKill).
bool coop_hits_message(const CoopMessage &message);
// One of them from a player in the room, kept for the next world update.
void coop_hits_receive(CoopPlayerId from, const CoopMessage &message);
// Once a world update, held or not, before the actors': what came is applied, and this player's
// aim at an actor another machine runs is sent to it.
void coop_hits_world_update();
// The world left play, or the room ended.
void coop_hits_stop();

}
