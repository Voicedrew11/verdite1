#pragma once
// What the coop session says on the CONTROL channel (reliable, ordered): a version byte, a kind
// byte, then the kind's fields, little-endian. The host is the room's oldest peer; everything about
// the party goes through it (docs/COOP_PLAN.md, "The lasting world is the host's save").
#include "coop_party.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace kf::ext {

// Guest to host, on opening: who this is, and a fresh character in case they are new to the world.
struct CoopHello {
    CoopPlayerId id = 0;
    std::string name;
    std::vector<std::uint8_t> fresh_state;
};

// Host to a guest, the answer: their character, the party's pack and purse, the world, and where
// the host stands, to arrive beside.
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
// Guest to host: what they now want to wear; the answer is in the next CoopPack.
struct CoopWearRequest {
    CoopWorn worn{};
};

// Host to everyone, after any change: the pack and purse, what each player in the room wears, and
// for each guest the last of their changes it holds.
struct CoopPack {
    CoopItems pack{};
    std::uint32_t gold = 0;
    struct Player { CoopPlayerId id; CoopWorn worn; std::uint32_t applied_through; };
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

using CoopMessage = std::variant<CoopHello, CoopWelcome, CoopCharacterUpdate, CoopAcknowledged, CoopPackRequest,
    CoopWearRequest, CoopPack, CoopFeedLine, CoopSaveRequest>;

inline constexpr std::size_t coop_message_most = 65536;   // VO_CONTROL_BYTES

// At most coop_message_most; a welcome with a world too big for it is nothing.
std::optional<std::vector<std::uint8_t>> coop_message_encode(const CoopMessage &message);
// Nothing when the bytes are not a message: the wrong version or kind, a length past the end or
// bytes left over, an item, stack, floor or cell out of range, a name outside the font.
std::optional<CoopMessage> coop_message_decode(std::span<const std::uint8_t> bytes);

}
