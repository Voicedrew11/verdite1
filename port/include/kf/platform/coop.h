#ifndef KF_PLATFORM_COOP_H
#define KF_PLATFORM_COOP_H
// PC extension: the coop world (docs/COOP_PLAN.md). What the game asks of it while it plays one,
// as the host in a room, as a guest in one, or as the host alone between sessions; outside it each
// call answers as the game alone would. Kept by port/ext/coop.cpp.
#include <kf/lib/types.h>
#include <kf/platform/language.h>

#include <string>
#include <vector>

namespace kf {

// Whether the game in play is the coop world rather than a single-player save.
bool coop_world_in_play();

// How many of an item in the pack the others in the room wear: the equipment list leaves it out
// once that is all of them, and the drop and sell lists count it as taken.
int coop_worn_by_others(u8 item);

// The highest floor any player has reached, which NPCs' dialogue stages and the floor-2 departure
// read in coop; `own` outside it. A stage is never set lower than it was in coop.
u8 coop_highest_floor(u8 own);

// Whether this machine runs the loaded floor's actors (actor_pool_update): always alone, and in a
// room only on the floor's owner. Elsewhere the owner's actors are drawn as it sends them, and a
// hit on one does nothing until hits cross machines (Order 6).
bool coop_runs_floor();

// map_unload_floor, before the floor goes: the floor's owner sends it live to its heir, and its
// record to the host.
void coop_floor_unloading();

// A save point used in coop: the whole party and the world are saved to the host's coop save, as
// its last save; no card slot is picked. False when not in coop, and the game saves as it does.
bool coop_save_point();

// For the feed: what the next changes to the pack and purse are, said by the shop's buy and sell
// before they make them. Any other change is found (more) or used (fewer).
enum class PackCause : u8 { Found, Bought, Sold, Used };
void coop_pack_cause(PackCause cause);

// The pickup feed's lines for the game to draw at the side of the picture in its menu font,
// oldest first: the text before the item's name, the item (0xff for none, as for gold), the text
// after it, and how far faded (1 shown .. 0 gone). The text is what menu_spell spells.
struct CoopFeedRow {
    std::string before;
    u8 item = 0xff;
    std::string after;
    float opacity = 1.0f;
};
std::vector<CoopFeedRow> coop_feed_view(Language language);

// Once per world update, after the player's, held or not: sends what this player's game changed
// in the pack and purse.
void coop_update();

// Between two world updates (host_between_updates), where no menu or script holds the game:
// joining and leaving the coop world, the host's decisions put into the game, and this player's
// character sent.
void coop_between_updates();

}

#endif // KF_PLATFORM_COOP_H
