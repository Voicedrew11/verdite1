#pragma once
// Death in a coop room (docs/COOP_PLAN.md, "Death is the party's"): the rules, with no game state
// and no network. A player whose HP reaches 0 goes down: out of the world, seeing through a living
// player's eyes, the next at a button press, until that player is out of a fight; then it comes
// back on a free cell beside them. When every player in the room is down, the host sends the party
// back to its last save.
#include "coop_party.h"

#include <cstdint>
#include <functional>
#include <span>

namespace kf::ext {

// A player in the room as its last pose has it, in the roster's order (the host first).
struct CoopPartyMember {
    CoopPlayerId id = 0;
    int floor = 0;           // 0 before its first pose
    float x = 0, z = 0;      // in cells
    bool down = false;
    bool fighting = false;
};

// Whom a down player on `floor` at (`x`, `z`) sees through, of the players up with a pose (a floor):
// `current` while it is one; else the nearest on that floor; else the first in the room's order; 0
// when there is none.
CoopPlayerId coop_eyes_choose(std::span<const CoopPartyMember> room, CoopPlayerId current, int floor, float x, float z);
// The next player up with a pose after `current` in the room's order, round to the first; the first
// when `current` is not in the room; 0 when there is none.
CoopPlayerId coop_eyes_next(std::span<const CoopPartyMember> room, CoopPlayerId current);

// Whether the party is down: this player and every other in the room. A host alone is a party of
// one, so its own death is the party's.
bool coop_party_down(bool self_down, std::span<const CoopPartyMember> others);

struct CoopCell {
    int x = 0, z = 0;
    bool operator==(const CoopCell &) const = default;
};
// Where a down player comes back beside a player standing on `cell`: the first of the four cells
// beside it that `free` says can be stood on now, then the four corners, then their own cell, since
// players pass through each other.
CoopCell coop_cell_beside(CoopCell cell, const std::function<bool(CoopCell)> &free);

// Whether this player is in a fight, which keeps a down player from coming back beside it: a hit
// taken or dealt in the last `calm_updates` world updates, or an enemy chasing it now.
class CoopFight {
public:
    explicit CoopFight(std::uint32_t calm_updates);

    void hit();
    // Once a world update, with whether an enemy chases this player now.
    void update(bool chased);
    bool fighting() const;

private:
    std::uint32_t calm_updates_;
    std::uint32_t since_hit_;
    bool chased_ = false;
};

}
