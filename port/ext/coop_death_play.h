#pragma once
// This machine's part in death in a coop room (docs/COOP_PLAN.md, "Death is the party's";
// coop_death.h's rules). When its player's death fade ends the player goes down: out of the world,
// so nothing targets it, the collision grid does not count it, nothing hits it, it triggers nothing
// and a kill gives it nothing. Its view is a living player's eyes, on that player's floor, which
// it loads as an arriving player does, so it counts as on that floor; Attack or Confirm moves to
// the next. Once that player is out of a fight, or after 30 seconds at most, it comes back on a
// free cell beside them, revived as with a Grass Fruit. Its pose says whether it is down, and
// whether its player is in a fight. Outside a room nothing here acts.
#include "coop_death.h"

#include <cstdint>
#include <span>
#include <string>

namespace kf::ext {

// The coop world is in play, with this player `self`.
void coop_death_start(CoopPlayerId self);
// The world left play, the room ended, or the party went back to its last save.
void coop_death_stop();

// The player's death fade ended.
void coop_death_went_down();
// Once an update while down, in place of the player's own update: the view at the eyes followed,
// moved to the next player when `next` was pressed, on their floor; back beside them once they
// are out of a fight, or after 30 seconds at most.
void coop_death_down_update(bool next);
bool coop_death_down();
// A hit this player took or dealt.
void coop_death_fought();
// Once a world update, held or not: whether an enemy chases this player now.
void coop_death_world_update();
// For this player's pose.
bool coop_death_fighting();
// The peer whose eyes this machine sees through, 0 for none: its knight is not drawn here.
std::uint64_t coop_death_eyes_peer();
// The name of the player whose eyes this machine sees through, empty for none.
std::string coop_death_eyes_name();

// Host only: whether the party is down, so it goes back to its last save. A player in `not_back`
// has not yet come back to the save from the last wipe, and counts as up.
bool coop_death_party_down(std::span<const CoopPlayerId> not_back);

// For the agent: down, whose eyes, in a fight, as JSON fields.
std::string coop_death_json();

}
