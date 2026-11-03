#ifndef KF_PLATFORM_MOD_EVENTS_H
#define KF_PLATFORM_MOD_EVENTS_H
// The events the game raises for the mods (docs/MODDING.md, "Events"). Each runs inline, where the
// thing happens, on the game's thread. The functions that return a value return what the mods left,
// or their input when no mod handles the event (or no mod host runs, as in headless tests).
#include <kf/lib/types.h>
#include <kf/platform/language.h>
#include <kf/platform/saves.h>

#include <string>
#include <vector>

namespace kf {

s32 mods_player_damaged(s32 loss_hp);

enum class HpDrain : u8 { Poison, Equipment };
// HP lost outside a blow; returns the loss the mods left (0 for none).
s32 mods_player_drained(HpDrain cause, s32 loss_hp);

// A blow on an actor. by_player: the player made it (not a hazard or another actor).
struct ActorHit {
    u16 actor_index;
    u16 definition_id;
    s32 damage_hp;
    bool by_player;
};
s32 mods_actor_damaged(const ActorHit &hit);
// experience is the reward the kill gives the player; the result replaces it when the player made
// the kill, and is ignored otherwise.
s16 mods_actor_killed(const ActorHit &hit, s16 experience);

// What a dying actor drops: the gold, and the item that falls (-1: none).
struct ActorDrop {
    u16 actor_index;
    u16 definition_id;
    u16 gold;
    s16 item;
};
ActorDrop mods_actor_drops(const ActorDrop &drop);

void mods_level_up(u8 level);
// false: a mod cancelled the pickup, and the item stays where it lies.
bool mods_item_picked_up(u16 item);
// A mod's item chosen in the Use list; true when a handler says it was used, and the menus close.
bool mods_item_used(u16 item);
void mods_floor_loaded(u8 floor, u8 map_variant);
void mods_dialogue(u8 floor, u8 character, u8 stage, u8 page);
// false: a mod cancelled the shop, which does not open.
bool mods_shop_opened(u8 bank);
// A revival keeps the game and its save data; a restart is a new game (mods_new_game).
void mods_player_died();
// A new game starts with empty save data (no event).
void mods_new_game();
// Stores the mods' save data beside the slot, then raises on_save.
void mods_game_saved(SaveSlot slot);
// Reads the mods' save data beside the slot (none or unreadable: empty), then raises on_load_game.
void mods_game_loaded(SaveSlot slot);
// The lines to show before loading the slot when it was made with other mods than those running now,
// in the menu font's spelling; empty when they agree, or the save does not say.
std::vector<std::string> mods_save_warning(SaveSlot slot, Language language);
// F4: true when the quick save may load. When its mods differ the first press says so and answers
// false, and a second press soon after answers true.
bool mods_quick_load_confirmed();

// The tables kf.rules changes (docs/MODDING.md, "kf.rules").
enum class RulesTable : u8 { ActorDefinitions, LevelGrowth, ShopPrices, WeaponRecords, ArmourRecords };
// The game reloaded a table the rules change: its values are the originals, and the running mods'
// rules are laid over them again.
void mods_rules_reapply(RulesTable table);

} // namespace kf

#endif // KF_PLATFORM_MOD_EVENTS_H
