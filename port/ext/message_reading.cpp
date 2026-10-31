#include "message_reading.h"

#include <algorithm>
#include <cmath>

namespace kf::ext {
namespace {

// Beyond this a coordinate is off any map; its cell is one no message has.
constexpr float off_map = 1000.0f;
constexpr int off_map_cell = -1000;
constexpr double turn_radians = 6.283185307179586;
constexpr int facings = 256;

int cell_coordinate(float value) {
    if (!(value > -off_map))
        return off_map_cell;
    if (value > off_map)
        return -off_map_cell;
    return static_cast<int>(std::floor(value));
}

bool on_map(MessageCell cell) {
    return cell.x >= 0 && cell.x <= message_cell_most && cell.z >= 0 && cell.z <= message_cell_most;
}

std::optional<std::size_t> index_of(std::span<const MessageCell> cells, MessageCell wanted) {
    const auto found = std::ranges::find(cells, wanted);
    if (found == cells.end())
        return std::nullopt;
    return static_cast<std::size_t>(found - cells.begin());
}

// The yaw as 256ths of a turn, from 0 to 255; a yaw not finite faces 0.
std::uint8_t facing_of(float yaw) {
    if (!std::isfinite(yaw))
        return 0;
    const double turns = yaw / turn_radians;
    const double fraction = turns - std::floor(turns);
    return static_cast<std::uint8_t>(std::lround(fraction * facings) % facings);
}

} // namespace

MessageCell message_cell_at(float x, float z) {
    return {cell_coordinate(x), cell_coordinate(z)};
}

MessageCell message_cell_ahead(float x, float z, float yaw) {
    return message_cell_at(x - std::sin(yaw), z + std::cos(yaw));
}

std::optional<std::size_t> message_for_action(std::span<const MessageCell> cells, float x, float z, float yaw) {
    if (const auto here = index_of(cells, message_cell_at(x, z)))
        return here;
    return index_of(cells, message_cell_ahead(x, z, yaw));
}

std::optional<std::uint32_t> message_place_encode(MessageCell cell, float yaw) {
    if (!on_map(cell))
        return std::nullopt;
    const auto x = static_cast<std::uint32_t>(cell.x);
    const auto z = static_cast<std::uint32_t>(cell.z);
    return x | (z << 8) | (static_cast<std::uint32_t>(facing_of(yaw)) << 16);
}

std::optional<MessagePlace> message_place_decode(std::uint32_t place) {
    const MessageCell cell{static_cast<int>(place & 0xff), static_cast<int>((place >> 8) & 0xff)};
    const std::uint32_t reserved = place >> 24;
    if (!on_map(cell) || reserved != 0)
        return std::nullopt;
    return MessagePlace{cell, static_cast<std::uint8_t>((place >> 16) & 0xff)};
}

void OpenMessage::open(std::uint64_t id, MessageCell cell) {
    open_ = Open{id, cell};
}

void OpenMessage::close() {
    open_.reset();
}

std::optional<OpenMessage::Open> OpenMessage::current() const {
    return open_;
}

void OpenMessage::follow(float x, float z) {
    if (!open_)
        return;
    const MessageCell at = message_cell_at(x, z);
    const int distance = std::max(std::abs(at.x - open_->cell.x), std::abs(at.z - open_->cell.z));
    if (distance > 1)
        close();
}

MessagePress OpenMessage::press(MessageRating rating) {
    const bool rate = open_ && rating == MessageRating::Possible;
    close();
    return rate ? MessagePress::Rate : MessagePress::Close;
}

} // namespace kf::ext
