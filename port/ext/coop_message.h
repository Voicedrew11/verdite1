#pragma once
// What the coop session says on the CONTROL channel (reliable, ordered): a version byte, a kind
// byte, then the kind's fields, little-endian. The host is the room's oldest peer; everything about
// the party goes through it (docs/COOP_PLAN.md, "The lasting world is the host's save").
#include "coop_floor_owners.h"
#include "coop_party.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace kf::ext {

// Guest to host, on opening: who this is.
struct CoopHello {
    CoopPlayerId id = 0;
    std::string name;
};

// Host to a guest, the answer: their character (empty for one new to the world, who starts a new
// game's), the party's pack and purse, the world, and where the host stands, to arrive beside.
struct CoopWelcome {
    std::vector<std::uint8_t> state;
    CoopWorn worn{};
    CoopItems pack{};
    std::uint32_t gold = 0;
    int highest_floor = 1;
    std::vector<std::uint8_t> world;
    int floor = 1;
    std::uint8_t cell_x = 0, cell_z = 0;
};

// Guest to host, when the character changes (at most a few times a second): numbered, and kept
// once the host acknowledges that number.
struct CoopCharacterUpdate {
    std::uint32_t number = 0;
    std::vector<std::uint8_t> state;
    int highest_floor = 1;
};
struct CoopAcknowledged {
    std::uint32_t number = 0;
};

// Guest to host: a change the guest's game made to the pack or purse, and what caused it, for the
// feed. Its number orders it among that guest's changes.
enum class CoopCause : std::uint8_t { Found, Bought, Sold, Used };
struct CoopPackRequest {
    std::uint32_t number = 0;
    CoopPackChange change;
    CoopCause cause = CoopCause::Found;
};
// Guest to host: what they now want to wear, numbered; the answer is in the next CoopPack.
struct CoopWearRequest {
    std::uint32_t number = 0;
    CoopWorn worn = coop_nothing_worn;
};

// Host to everyone, after any change: the pack and purse, the world's highest floor, what each
// player in the room wears, and for each guest the last of their pack changes and wear requests
// it holds.
struct CoopPack {
    CoopItems pack{};
    std::uint32_t gold = 0;
    int highest_floor = 1;
    struct Player { CoopPlayerId id; CoopWorn worn; std::uint32_t applied_through; std::uint32_t worn_through; };
    std::vector<Player> players;
};
// Host to everyone: a line for the feed.
struct CoopFeedLine {
    CoopPlayerId by = 0;
    CoopCause cause = CoopCause::Found;
    CoopPackChange change;
};

// Guest to host: this player used a save point, so the whole party and world are saved.
struct CoopSaveRequest {};

// Host to everyone, when anyone comes or goes: each player in the room and their peer, so a
// snapshot can go straight from one player to another.
struct CoopRoster {
    struct Player { CoopPlayerId id; std::uint64_t peer; };
    std::vector<Player> players;
};
// A player to everyone, when it comes onto a floor: where it is now, and whom its machine handed
// the floor it left to (0: no one). The host's table takes it; the others learn where it is.
struct CoopFloorAt {
    int floor = 1;
    CoopPlayerId left_floor_with = 0;
};
// Host to everyone: the table's orders (CoopFloorOwners), each acted on by the players it names.
struct CoopFloorOrders {
    std::vector<CoopFloorOrder> orders;
};
// A floor's owner to the host, as its lasting state changes and when it leaves; and the host to a
// player it orders to run a floor from the record, just before the order. The bytes are
// coop_floor_record's.
struct CoopFloorRecord {
    int floor = 1;
    std::vector<std::uint8_t> record;
};
// One player to another, straight: a floor's record and its live state (coop_floor_capture), for
// the one it goes to to run (a take, or a handover to the heir) or to watch (a show).
enum class CoopSnapshotFor : std::uint8_t { Take, Handover, Show };
struct CoopFloorSnapshot {
    int floor = 1;
    CoopSnapshotFor purpose = CoopSnapshotFor::Show;
    std::vector<std::uint8_t> record;
    std::vector<std::uint8_t> live;
};
// The taker of a floor to the host: it runs the floor from the snapshot now.
struct CoopFloorTaken {
    int floor = 1;
};
// A player on a floor to everyone: what it changed in the floor's lasting state (coop_floor_writes).
struct CoopFloorWrites {
    int floor = 1;
    std::vector<std::uint8_t> writes;
};

inline constexpr std::size_t coop_actor_slots = 128;   // KF_ACTOR_CAPACITY

// A player on a floor another machine runs, to its owner: a hit as it landed on this player's
// screen, with actor_apply_damage's arguments. The owner refuses one whose slot now holds another
// definition.
struct CoopActorHit {
    int floor = 1;
    std::uint8_t slot = 0;
    std::uint8_t definition = 0;
    std::uint16_t base_power = 0;
    std::array<std::uint16_t, 5> damage{};   // cutting, striking, piercing, magic, fire
    std::uint16_t scale = 0;
    bool credited = false;                   // a player's own hit, which gives experience on a kill
};
// The same player to the owner: it aimed a spell at an actor (player_target), which may dodge.
struct CoopActorAim {
    int floor = 1;
    std::uint8_t slot = 0;
    std::uint8_t definition = 0;
};
// A floor's owner to everyone: a player's hit killed an actor, and every player on the floor gets
// its experience.
struct CoopKill {
    int floor = 1;
    std::uint16_t experience = 0;
};

using CoopMessage = std::variant<CoopHello, CoopWelcome, CoopCharacterUpdate, CoopAcknowledged, CoopPackRequest,
    CoopWearRequest, CoopPack, CoopFeedLine, CoopSaveRequest, CoopRoster, CoopFloorAt, CoopFloorOrders,
    CoopFloorRecord, CoopFloorSnapshot, CoopFloorTaken, CoopFloorWrites, CoopActorHit, CoopActorAim, CoopKill>;

inline constexpr std::size_t coop_message_most = 65536;   // VO_CONTROL_BYTES

// At most coop_message_most; a welcome with a world too big for it is nothing.
std::optional<std::vector<std::uint8_t>> coop_message_encode(const CoopMessage &message);
// Nothing when the bytes are not a message: the wrong version or kind, a length past the end or
// bytes left over, an item, stack, floor or cell out of range, a name outside the font.
std::optional<CoopMessage> coop_message_decode(std::span<const std::uint8_t> bytes);

}
