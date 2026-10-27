#pragma once
// A floor's live state, for the handover between its owners (docs/COOP_PLAN.md, "The handover is
// live"): everything on the loaded floor that runs on its own (the actors and their definitions,
// effects in flight, objects and doors, NPCs, the scripts' timers, and the collision grid's
// occupants but this player), so the machine that takes the floor carries on the same fight. The
// floor's lasting records are the world's, not in it. The bytes are this build's layout: a
// machine of another layout refuses them and runs the floor from its record.
//
// Also what passes between the players on one floor while one of them runs it: the floor's
// record, the changes a player makes to its lasting state (writes), and the owner's awake actors,
// which the others draw as puppets. Every function here is for between two world updates unless
// it says otherwise.
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace kf::ext {

// On the floor's owner.
std::vector<std::uint8_t> coop_floor_capture();

// On a machine that has loaded the same floor from its record: puts the live floor in place of
// the record's. False, and nothing changed, when the bytes are not a snapshot of this floor in
// this layout, or a slot holds a table index or cell out of range.
bool coop_floor_restore(std::span<const std::uint8_t> bytes);

// A floor's lasting record and its story flags (floor 1's, 3's or 5's), as this machine's world
// keeps them: what map_world_state_persist keeps of it, and what a machine loads it from. The
// loaded floor is persisted first.
std::vector<std::uint8_t> coop_floor_record(int floor);
// The floor a record is of, 1..5; 0 for bytes that are not one.
int coop_floor_record_floor(std::span<const std::uint8_t> record);
// Puts a record into this machine's world for a floor that is not loaded, as the host keeps an
// owner's. Floor 1's two-step triggers keep this machine's stage unless the record's is complete.
// False, and nothing changed, for bytes that are not a record or a record of the loaded floor.
bool coop_floor_keep_record(std::span<const std::uint8_t> record);
// Runs the loaded floor again from `record`, then puts `live` over it when it is not empty, keeping
// this machine's player and random stream: loaded again unless the record is what the floor holds
// now and, without `live`, no actor on it was drawn from another machine's. False, with the floor as it was, when
// either is refused.
bool coop_floor_reload(std::span<const std::uint8_t> record, std::span<const std::uint8_t> live);

// What this machine changed in the loaded floor's lasting state since the last call, as writes
// for the others on the floor; empty for nothing. A write is an object whose id or link changed
// (a pickup taken, a chest emptied, a key's link cleared) or whose action began (a door, a chest's
// lid), a drop that appeared or went, an event whose state or dialogue stage changed, or the
// floor's story flags: floor 1's two-step triggers only once complete, and not floor 3's map
// pieces, which each machine counts as its own objects reveal them. The motion an object begins
// is not sent: every machine runs its own objects. On a new floor the first call starts afresh
// and says nothing.
std::vector<std::uint8_t> coop_floor_writes();
// Puts another player's writes into the loaded floor, with what loading the floor would apply for
// a story flag (a passage opened, the boss's encounter); not sent again by coop_floor_writes.
// False, and nothing changed, for bytes that are not writes for this layout.
bool coop_floor_apply_writes(std::span<const std::uint8_t> writes);

// The floor was unloaded: the writes start afresh on the next floor.
void coop_floor_forget();

// What the others on the floor draw of it, on its owner, once a world update: its awake actors, and
// the effects in flight that its actors launched (the others run their own traps and spells).
std::vector<std::uint8_t> coop_floor_frame();
// Once a world update, on a machine that does not run the floor: the owner's awake actors in place
// of this machine's, which do not run, and every other actor asleep, the collision grid counting
// them where they stand; the owner's effects drawn beside this machine's own; and the sounds the
// owner's actors and effects made since the last frame. False, and nothing changed, for bytes that
// are not a frame or an effect this machine cannot draw.
bool coop_floor_show_frame(std::span<const std::uint8_t> frame);
// The owner's effects drawn no more: this machine left watching, or the floor.
void coop_floor_hide_effects();

}
