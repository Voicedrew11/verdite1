#pragma once
// The coop world's party (docs/COOP_PLAN.md, "Items" and "The state map"): one pack and one purse,
// every character that has joined, what each wears, and the world's highest floor. The host keeps
// the one true copy; each guest keeps a mirror of the pack, ahead of it by its own changes not yet
// applied. Rules and data only: no game state, no network. A character's and the world's bytes are
// the game's (save_system.cpp encodes them) and are carried here unread.
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kf::ext {

inline constexpr std::size_t coop_item_count = 80;       // KF_ITEM_COUNT
inline constexpr int coop_stack_most = 99;                // KF_ITEM_STACK_CAPACITY
inline constexpr std::uint8_t coop_no_item = 0xff;        // KF_OBJECT_NONE
// Weapon, head, body, shield, arms, legs, accessory: the game's equipment slots.
inline constexpr std::size_t coop_worn_slots = 7;

using CoopPlayerId = std::uint64_t;   // an install's, the same in every session; 0 is none
using CoopItems = std::array<std::uint8_t, coop_item_count>;
using CoopWorn = std::array<std::uint8_t, coop_worn_slots>;   // item ids, coop_no_item for empty

struct CoopCharacter {
    CoopPlayerId id = 0;
    std::string name;                  // capitals and digits: what menu_spell draws
    std::vector<std::uint8_t> state;   // the character as the game saves it, gold aside
    CoopWorn wore{};                   // what they wore when they last left; put back on if still free
    int highest_floor = 1;
};

// A change to the pack or purse: an item and a count (negative takes), or gold.
struct CoopPackChange {
    std::uint8_t item = coop_no_item;   // coop_no_item: the change is gold
    std::int32_t count = 0;
};

// The host's copy.
class CoopParty {
public:
    // A player comes into the room. On their first join their character is fresh_state and the
    // game's starting kit (a Short Sword, a Medicinal Herb, 150 gold) goes into the pack and purse;
    // after that it is the character kept here, wearing what of `wore` no one else wears.
    const CoopCharacter &join(CoopPlayerId id, std::string_view name, std::span<const std::uint8_t> fresh_state);
    // They leave: what they wore stays in the pack, free for the others, and is remembered.
    void leave(CoopPlayerId id);

    // As much of the change as the pack allows: an item is never taken below what the others wear
    // or added past a full stack, gold never below 0. Returns the count applied.
    std::int32_t apply(CoopPlayerId by, CoopPackChange change);
    // Puts on what of `worn` is free for them; anything not free leaves its slot empty. Returns
    // what they now wear.
    CoopWorn wear(CoopPlayerId id, const CoopWorn &worn);
    // A character's latest state, as their machine sends it; the highest floor rises with it.
    void store(CoopPlayerId id, std::span<const std::uint8_t> state, int highest_floor);

    // How many of an item are in the pack that `id` may wear, drop or sell: the count less what
    // the others in the room wear.
    int free_for(CoopPlayerId id, std::uint8_t item) const;
    const CoopItems &pack() const;
    std::uint32_t gold() const;
    // The highest floor any character has reached: NPCs' stages and the floor-2 departure read it.
    int highest_floor() const;
    std::span<const CoopCharacter> characters() const;
    std::vector<CoopWorn> worn_in_room() const;

private:
    // (the characters, who is in the room and what each wears, the pack and purse: with the .cpp)
};

// A guest's mirror: the host's pack with this player's changes the host has not yet applied on
// top, so the game reads what it just did while the host decides.
class CoopPackMirror {
public:
    // What the game changed since the last call, numbered; the caller sends them to the host.
    struct Sent { std::uint32_t number; CoopPackChange change; };
    std::vector<Sent> take_changes(const CoopItems &game_pack, std::uint32_t game_gold);
    // The host's pack and purse, with every change of ours through `applied_through` in it.
    void receive(const CoopItems &pack, std::uint32_t gold, std::uint32_t applied_through);
    // What the game's pack and purse are set to now.
    const CoopItems &pack() const;
    std::uint32_t gold() const;
};

// The coop world as the host's file keeps it: the live state, written as play goes on, and the
// last save, the whole party and world as when anyone last saved at a save point.
struct CoopWorldState {
    CoopParty party;
    std::vector<std::uint8_t> world;   // the game's floors and story flags, and the two shops' banks
};
struct CoopWorldFile {
    CoopWorldState live;
    std::optional<CoopWorldState> last_save;
};
std::vector<std::uint8_t> coop_world_encode(const CoopWorldFile &file);
// Nothing when the bytes are not a coop world: the wrong magic or version, a bad checksum, a count
// or length that runs past the end, a stack over 99, a name outside the font, two characters with
// one id, or a floor outside 1..5.
std::optional<CoopWorldFile> coop_world_decode(std::span<const std::uint8_t> bytes);

}
