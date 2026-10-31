#pragma once
// Messages on the ground, the parts with no ImGui and no network (docs/MESSAGES_PLAN.md): which
// shown message the action button is for, the message open for reading, and a message's place in
// a record. A cell is AutomapSnapshot's: cell (x, z) spans [x, x + 1). Yaw is in radians, 0 facing
// +z (north) and increasing turns towards -x, so the way the player looks is (-sin yaw, cos yaw)
// in (x, z).
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace kf::ext {

struct MessageCell {
    int x = 0, z = 0;
    bool operator==(const MessageCell &) const = default;
};
// A message's place is 0..100 in each cell coordinate; a cell past it is invalid.
inline constexpr int message_cell_most = 100;

// The cell a point lies in: the floor of each coordinate. A point that is not finite, or far off
// the map, lies in no cell a message can have.
MessageCell message_cell_at(float x, float z);
// The cell one cell ahead of (x, z) along yaw, as the point there lies in.
MessageCell message_cell_ahead(float x, float z, float yaw);

// The index in cells (in shown order) of the message the action button is for: the first in the
// player's cell, else the first in the cell ahead. Nothing when neither has one.
[[nodiscard]] std::optional<std::size_t> message_for_action(std::span<const MessageCell> cells, float x, float z, float yaw);

// A record's place, as the record holds it: byte 0 the cell x, byte 1 the cell z, byte 2 the facing
// in 256ths of a turn, byte 3 zero; little-endian in the uint32.
struct MessagePlace {
    MessageCell cell;
    std::uint8_t facing = 0;
    bool operator==(const MessagePlace &) const = default;
};
// The place of a message at cell, facing yaw; nothing when the cell is off the map.
[[nodiscard]] std::optional<std::uint32_t> message_place_encode(MessageCell cell, float yaw);
// The place a record holds; nothing when a cell is off the map or byte 3 is not zero.
[[nodiscard]] std::optional<MessagePlace> message_place_decode(std::uint32_t place);

// Possible: not the player's own, and not rated yet by this player.
enum class MessageRating : std::uint8_t { Possible, Impossible };
// What a press of the action button does to an open message: rate it, or only close it.
enum class MessagePress : std::uint8_t { Close, Rate };

// The message open for reading. It stays open until the player walks more than a cell from it
// (Chebyshev distance in cells) or presses the action button.
class OpenMessage {
public:
    struct Open {
        std::uint64_t id = 0;
        MessageCell cell{};
        bool operator==(const Open &) const = default;
    };
    void open(std::uint64_t id, MessageCell cell);
    void close();
    [[nodiscard]] std::optional<Open> current() const;
    // The player is at (x, z) now.
    void follow(float x, float z);
    // The action button pressed while open: Rate when the message can still be rated, and it closes
    // either way.
    [[nodiscard]] MessagePress press(MessageRating rating);

private:
    std::optional<Open> open_;
};

} // namespace kf::ext
