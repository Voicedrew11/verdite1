#ifndef KF_PLATFORM_COOP_H
#define KF_PLATFORM_COOP_H
// PC extension: the coop world (docs/COOP_PLAN.md). What the game asks of it while it plays one,
// as the host in a room, as a guest in one, or as the host alone between sessions; outside it each
// call answers as the game alone would. Kept by port/ext/coop.cpp.
#include <kf/lib/types.h>
#include <kf/platform/language.h>

#include <array>
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
// hit on one goes to the owner (coop_hit_actor_elsewhere).
bool coop_runs_floor();

// actor_apply_damage on a floor another player's machine runs, once the hit has trained this
// player: the hit as it landed here goes to that machine, which applies it.
struct CoopActorHitArgs {
    u16 actor;
    u8 definition;
    u16 base_power;
    std::array<u16, 5> damage;   // cutting, striking, piercing, magic, fire
    u16 scale;
    bool credited;               // a player's own hit, which gives experience on a kill
};
void coop_hit_actor_elsewhere(const CoopActorHitArgs &hit);

// actor_apply_damage on the floor's owner, once a player's hit has killed an actor and given this
// player its experience: every other player on the floor gets it too.
void coop_actor_killed(u16 experience);

// actor_try_attack_player on the floor's owner when the actor goes after another player (by online
// id, as actor_target_players has it): that player's machine runs the same test against its own
// position and the actor as it draws it, and feels the swing only if it lands there.
struct CoopSwingArgs {
    u16 actor;
    u8 definition;
    u16 minimum_distance;
    u16 maximum_distance;
    s16 angle_offset;
    s16 angle_tolerance;
    u16 status;                  // KfPlayerStatusFlags, rolled here
};
void coop_swing_at_other(u32 player, const CoopSwingArgs &swing);

// A projectile, an actor's jump or a blast that reached another player on this floor (by online
// id), with player_apply_damage's arguments: that player's machine applies it.
struct CoopPlayerHitArgs {
    u16 cutting;
    u16 striking;
    u16 piercing;
    u16 status;                  // KfPlayerStatusFlags
    u16 magic;
    u16 fire;
    u16 scale_q12;
    u16 multiplier_tenths;
};
void coop_hit_other_player(u32 player, const CoopPlayerHitArgs &hit);

// Whether this player's menus back out by themselves: in coop a menu does not pause the world, so a
// player can die in one, and the death waits for the menu to close.
bool coop_backs_out_of_menu();

// Death in coop (docs/COOP_PLAN.md, "Death is the party's"). player_death_update, as the death fade
// ends: in coop the player goes down, in place of the game's revival or new game, and true is
// returned. False outside coop, and the game revives or restarts as it does.
bool coop_player_died();
// Whether this player is down: out of the world, so nothing targets, meets or hurts it
// (player_distance_to_point finds it nowhere, player_apply_damage does nothing) and it triggers
// nothing where its view is (floor scripts, warps).
bool coop_player_down();
// player_update while down, in place of the player's own update: the view at a living player's
// eyes, the next one when `next` was pressed, on their floor; back beside them, faded in, once
// they are out of a fight or after 30 seconds at most.
void coop_down_update(bool next);
// A hit this player took (player_apply_damage) or dealt (actor_apply_damage with its credit): it
// is in a fight for a few seconds.
void coop_player_fought();
// What the game draws while down, in what menu_spell spells: whose eyes this player sees
// through. Empty when not down.
std::string coop_watching_text(Language language);

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
