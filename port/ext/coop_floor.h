#pragma once
// A floor's live state, for the handover between its owners (docs/COOP_PLAN.md, "The handover is
// live"): everything on the loaded floor that runs on its own (the actors and their definitions,
// effects in flight, objects and doors, NPCs, the scripts' timers, and the collision grid's
// occupants but this player), so the machine that takes the floor carries on the same fight. The
// floor's lasting records are the world's, not in it. The bytes are this build's layout: a
// machine of another layout refuses them and runs the floor from its record.
#include <cstdint>
#include <span>
#include <vector>

namespace kf::ext {

// Between two world updates, on the floor's owner.
std::vector<std::uint8_t> coop_floor_capture();

// Between two world updates, on a machine that has loaded the same floor from its record: puts the
// live floor in place of the record's. False, and nothing changed, when the bytes are not a
// snapshot of this floor in this layout, or a slot holds a table index or cell out of range.
bool coop_floor_restore(std::span<const std::uint8_t> bytes);

}
