#include "../ext/coop_floor_frames.h"

#include <doctest/doctest.h>

using namespace kf::ext;

namespace {

constexpr std::size_t actor_bytes = 30;

// A frame of `count` actors whose bytes all say `update`.
std::vector<std::uint8_t> actors(std::uint32_t update, std::size_t count) {
    return std::vector<std::uint8_t>(count * actor_bytes, static_cast<std::uint8_t>(update));
}

void send(CoopFramePlayback &playback, std::uint32_t update, std::size_t count = 2) {
    for (const auto &packet : coop_frame_encode(2, update, actors(update, count))) {
        const auto part = coop_frame_decode(packet);
        REQUIRE(part.has_value());
        playback.receive(part.value_or(CoopFramePart{}));
    }
}

// The update a played frame was sent in, from its bytes.
int played(CoopFramePlayback &playback) {
    const auto frame = playback.step();
    REQUIRE(frame.has_value());
    const auto actors = frame.value_or(std::vector<std::uint8_t>{});
    REQUIRE_FALSE(actors.empty());
    return actors.front();
}

} // namespace

TEST_CASE("a frame too big for one packet goes in parts, none over the most, joined as it was") {
    const auto packets = coop_frame_encode(3, 7, actors(7, 100));
    CHECK(packets.size() == 3);
    std::size_t carried = 0;
    for (const auto &packet : packets) {
        CHECK(packet.size() <= coop_frame_packet_most);
        const auto decoded = coop_frame_decode(packet);
        REQUIRE(decoded.has_value());
        const CoopFramePart part = decoded.value_or(CoopFramePart{});
        CHECK(part.floor == 3);
        CHECK(part.update == 7);
        CHECK(part.parts == 3);
        carried += part.bytes.size();
    }
    CHECK(carried == 100 * actor_bytes);
    CHECK(coop_frame_encode(1, 1, {}).size() == 1);
}

TEST_CASE("a packet of another tag, floor or part is refused") {
    auto packet = coop_frame_encode(1, 1, actors(1, 1)).front();
    auto tag = packet;
    tag[0] = 4;   // a pose's version
    CHECK_FALSE(coop_frame_decode(tag));
    auto floor = packet;
    floor[1] = 6;
    CHECK_FALSE(coop_frame_decode(floor));
    auto part = packet;
    part[6] = 1;   // part 1 of 1
    CHECK_FALSE(coop_frame_decode(part));
    CHECK_FALSE(coop_frame_decode(std::span(packet).first(7)));
}

TEST_CASE("frames play a few updates behind, one an update, holding the last") {
    CoopFramePlayback playback(2);
    CHECK_FALSE(playback.step().has_value());
    send(playback, 10);
    CHECK(played(playback) == 10);
    send(playback, 11);
    CHECK(played(playback) == 10);   // waiting for the delay to build
    send(playback, 12);
    CHECK(played(playback) == 10);
    send(playback, 13);
    CHECK(played(playback) == 11);
    send(playback, 14);
    CHECK(played(playback) == 12);
    CHECK(played(playback) == 13);
    CHECK(played(playback) == 14);
    CHECK(played(playback) == 14);
}

TEST_CASE("a frame missing a part is not drawn; the last whole one is") {
    CoopFramePlayback playback(0);
    send(playback, 1, 100);
    CHECK(playback.step().value_or(std::vector<std::uint8_t>{}).size() == 100 * actor_bytes);
    const auto packets = coop_frame_encode(2, 2, actors(2, 100));
    for (std::size_t i = 0; i + 1 < packets.size(); ++i)
        playback.receive(coop_frame_decode(packets[i]).value_or(CoopFramePart{}));
    CHECK(played(playback) == 1);
    playback.receive(coop_frame_decode(packets.back()).value_or(CoopFramePart{}));
    CHECK(played(playback) == 2);
}

TEST_CASE("far behind, playback jumps to the newest less the delay") {
    CoopFramePlayback playback(2);
    send(playback, 1);
    CHECK(played(playback) == 1);
    send(playback, 100);
    send(playback, 98);
    CHECK(played(playback) == 98);
}
