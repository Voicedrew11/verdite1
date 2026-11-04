#pragma once
// Messages on the ground, the parts with no ImGui and no network (docs/MESSAGES_PLAN.md): which
// shown message the action button is for, the box of the message open for reading, and a message's
// place in a record. A cell is AutomapSnapshot's: cell (x, z) spans [x, x + 1). Yaw is in radians, 0 facing
// +z (north) and increasing turns towards -x, so the way the player looks is (-sin yaw, cos yaw)
// in (x, z).
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

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

// How far the action button reaches: the cell ahead is out when a wall stands before it.
enum class MessageReach : std::uint8_t { HereAndAhead, HereOnly };
// The cell of the messages the action button is for: the player's, else the one ahead. Nothing when
// neither has one.
[[nodiscard]] std::optional<MessageCell> message_cell_for_action(std::span<const MessageCell> cells, float x, float z, float yaw, MessageReach reach);

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
// A choice in the box. Leave is always first, and where the cursor starts, so that two presses of the
// action button read a message without rating it.
enum class MessageChoice : std::uint8_t { Leave, Good, Next };
enum class MessageKey : std::uint8_t { Previous, Following, Confirm, Back };

// The box of the messages in one cell, open for reading. It stays open until the player chooses
// Leave, presses Back, or is more than a cell from it (Chebyshev distance in cells). Rating leaves
// it open on the message, so the player sees the rating taken.
class MessageBox {
public:
    // ids: the messages in cell, the first shown first. None opens nothing.
    void open(std::span<const std::uint64_t> ids, MessageCell cell);
    void close();
    [[nodiscard]] bool is_open() const;
    // The message shown now; nothing when closed.
    [[nodiscard]] std::optional<std::uint64_t> current() const;
    [[nodiscard]] std::size_t page() const;         // into the cell's messages, from 0
    [[nodiscard]] std::size_t page_count() const;
    // The choices for a message of this rating, and the one the cursor is on.
    [[nodiscard]] std::vector<MessageChoice> choices(MessageRating rating) const;
    [[nodiscard]] std::size_t cursor(MessageRating rating) const;
    // The player is at (x, z) now.
    void follow(float x, float z);
    // A key, with the rating of the message shown. The id to rate when the key rated it.
    [[nodiscard]] std::optional<std::uint64_t> press(MessageKey key, MessageRating rating);

private:
    std::vector<std::uint64_t> ids_;
    MessageCell cell_{};
    std::size_t page_ = 0;
    std::size_t cursor_ = 0;
};

// How many good ratings a player's messages gained between two lists of them; a message new in the
// later list, and one that lost count, gains none.
struct MessageCount {
    std::uint64_t id = 0;
    std::uint32_t good = 0;
};
[[nodiscard]] std::uint32_t message_good_gained(std::span<const MessageCount> before, std::span<const MessageCount> after);

} // namespace kf::ext
