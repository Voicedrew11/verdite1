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

std::optional<MessageCell> message_cell_for_action(std::span<const MessageCell> cells, float x, float z, float yaw, MessageReach reach) {
    const MessageCell here = message_cell_at(x, z);
    if (std::ranges::find(cells, here) != cells.end())
        return here;
    if (reach == MessageReach::HereOnly)
        return std::nullopt;
    const MessageCell ahead = message_cell_ahead(x, z, yaw);
    if (std::ranges::find(cells, ahead) != cells.end())
        return ahead;
    return std::nullopt;
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

void MessageBox::open(std::span<const std::uint64_t> ids, MessageCell cell) {
    ids_.assign(ids.begin(), ids.end());
    cell_ = cell;
    page_ = 0;
    cursor_ = 0;
}

void MessageBox::close() {
    ids_.clear();
}

bool MessageBox::is_open() const {
    return !ids_.empty();
}

std::optional<std::uint64_t> MessageBox::current() const {
    if (ids_.empty())
        return std::nullopt;
    return ids_[page_];
}

std::size_t MessageBox::page() const {
    return page_;
}

std::size_t MessageBox::page_count() const {
    return ids_.size();
}

std::vector<MessageChoice> MessageBox::choices(MessageRating rating) const {
    std::vector<MessageChoice> rows{MessageChoice::Leave};
    if (rating == MessageRating::Possible)
        rows.push_back(MessageChoice::Good);
    if (ids_.size() > 1)
        rows.push_back(MessageChoice::Next);
    return rows;
}

std::size_t MessageBox::cursor(MessageRating rating) const {
    return std::min(cursor_, choices(rating).size() - 1);
}

void MessageBox::follow(float x, float z) {
    if (ids_.empty())
        return;
    const MessageCell at = message_cell_at(x, z);
    const int distance = std::max(std::abs(at.x - cell_.x), std::abs(at.z - cell_.z));
    if (distance > 1)
        close();
}

std::optional<std::uint64_t> MessageBox::press(MessageKey key, MessageRating rating) {
    if (ids_.empty())
        return std::nullopt;
    const std::vector<MessageChoice> rows = choices(rating);
    const std::size_t at = cursor(rating);
    switch (key) {
    case MessageKey::Previous:
        cursor_ = (at + rows.size() - 1) % rows.size();
        return std::nullopt;
    case MessageKey::Following:
        cursor_ = (at + 1) % rows.size();
        return std::nullopt;
    case MessageKey::Back:
        close();
        return std::nullopt;
    case MessageKey::Confirm:
        break;
    }
    cursor_ = 0;
    switch (rows[at]) {
    case MessageChoice::Leave:
        close();
        return std::nullopt;
    case MessageChoice::Good:
        return ids_[page_];
    case MessageChoice::Next:
        page_ = (page_ + 1) % ids_.size();
        return std::nullopt;
    }
    return std::nullopt;
}

std::uint32_t message_good_gained(std::span<const MessageCount> before, std::span<const MessageCount> after) {
    std::uint32_t gained = 0;
    for (const MessageCount &now : after) {
        const auto was = std::ranges::find_if(before, [&now](const MessageCount &count) { return count.id == now.id; });
        if (was != before.end() && now.good > was->good)
            gained += now.good - was->good;
    }
    return gained;
}

} // namespace kf::ext
