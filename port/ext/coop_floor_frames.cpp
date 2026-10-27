#include "coop_floor_frames.h"

#include "coop_bytes.h"
#include "coop_party.h"

#include <algorithm>
#include <iterator>

namespace kf::ext {
namespace {

constexpr std::size_t header_bytes = 8;   // tag, floor, update, part, parts
constexpr std::uint32_t catch_up_after = 1;
constexpr std::uint32_t stall_updates = 32;
constexpr std::size_t frames_most = 64;

} // namespace

std::vector<std::vector<std::uint8_t>> coop_frame_encode(int floor, std::uint32_t update,
    std::span<const std::uint8_t> frame) {
    constexpr std::size_t per_packet = coop_frame_packet_most - header_bytes;
    const std::size_t parts = std::max<std::size_t>(1, (frame.size() + per_packet - 1) / per_packet);
    std::vector<std::vector<std::uint8_t>> packets;
    for (std::size_t part = 0; part < parts; ++part) {
        CoopByteWriter out;
        out.u8(coop_frame_tag);
        out.u8(static_cast<std::uint8_t>(floor));
        out.u32(update);
        out.u8(static_cast<std::uint8_t>(part));
        out.u8(static_cast<std::uint8_t>(parts));
        const std::size_t first = part * per_packet;
        out.raw(frame.subspan(first, std::min(per_packet, frame.size() - first)));
        packets.push_back(std::move(out.bytes()));
    }
    return packets;
}

std::optional<CoopFramePart> coop_frame_decode(std::span<const std::uint8_t> bytes) {
    if (bytes.size() > coop_frame_packet_most)
        return std::nullopt;
    CoopByteReader in(bytes);
    const std::uint8_t tag = in.u8();
    CoopFramePart part;
    part.floor = in.u8();
    part.update = in.u32();
    part.part = in.u8();
    part.parts = in.u8();
    if (!in.ok() || tag != coop_frame_tag || part.floor < 1 || part.floor > coop_floor_count || part.parts == 0 || part.part >= part.parts)
        return std::nullopt;
    const auto frame = bytes.subspan(header_bytes);
    part.bytes.assign(frame.begin(), frame.end());
    return part;
}

CoopFramePlayback::CoopFramePlayback(std::uint32_t delay_updates) : delay_updates_(delay_updates) {}

void CoopFramePlayback::receive(const CoopFramePart &part) {
    if (playhead_ && part.update < *playhead_)
        return;
    Frame &frame = frames_[part.update];
    if (frame.received.empty())
        frame.parts = part.parts;
    if (part.parts != frame.parts)
        return;
    frame.received.try_emplace(part.part, part.bytes);
    if (frames_.size() > frames_most)
        frames_.erase(frames_.begin());
}

std::optional<std::vector<std::uint8_t>> CoopFramePlayback::step() {
    if (frames_.empty())
        return std::nullopt;
    const std::uint32_t newest = frames_.rbegin()->first;
    if (!playhead_)
        playhead_ = frames_.begin()->first;
    std::uint32_t &playhead = *playhead_;
    const std::uint32_t behind = newest > playhead ? newest - playhead : 0;
    if (behind > delay_updates_ + stall_updates) {
        playhead = newest - delay_updates_;
        waiting_ = false;
    } else if (behind == 0) {
        waiting_ = true;
    } else if (!waiting_ || behind > delay_updates_) {
        waiting_ = false;
        playhead += behind > delay_updates_ + catch_up_after ? 2 : 1;
    }
    // The newest whole frame at or before the playhead; older ones are done with.
    auto shown = frames_.end();
    for (auto at = frames_.begin(); at != frames_.end() && at->first <= playhead; ++at)
        if (at->second.whole())
            shown = at;
    if (shown == frames_.end())
        return std::nullopt;
    frames_.erase(frames_.begin(), shown);
    std::vector<std::uint8_t> frame;
    for (const auto &[index, bytes] : shown->second.received)
        frame.insert(frame.end(), bytes.begin(), bytes.end());
    return frame;
}

}
