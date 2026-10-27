#ifndef KF_PLATFORM_COOP_H
#define KF_PLATFORM_COOP_H
// PC extension: the coop world (docs/COOP_PLAN.md). What the game asks of it while it plays one,
// as the host in a room, as a guest in one, or as the host alone between sessions; outside it each
// call answers as the game alone would. Kept by port/ext/coop.cpp.
#include <kf/lib/types.h>

namespace kf {

// Whether the game in play is the coop world rather than a single-player save.
bool coop_world_in_play();

// How many of an item in the pack the others in the room wear: the equipment list leaves it out
// once that is all of them, and the drop and sell lists count it as taken.
int coop_worn_by_others(u8 item);

// The highest floor any player has reached, which NPCs' dialogue stages and the floor-2 departure
// read in coop; `own` outside it. A stage is never set lower than it was in coop.
u8 coop_highest_floor(u8 own);

// Whether map_world_state_persist may write the floor's records into the lasting world: always
// alone, and in a room only on the floor's owner (the host, until floors have owners). A machine
// that may not still keeps its own copy for the session.
bool coop_floor_records_kept();

// A save point used in coop: the whole party and the world are saved to the host's coop save, as
// its last save; no card slot is picked. False when not in coop, and the game saves as it does.
bool coop_save_point();

// Once per world update, after the player's: sends what changed in this player's character, pack
// and purse, and puts in the game what the host has decided since.
void coop_update();

}

#endif // KF_PLATFORM_COOP_H
