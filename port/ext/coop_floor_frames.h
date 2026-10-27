#pragma once
// A floor's awake actors as its owner sends them to the others on the floor (docs/COOP_PLAN.md,
// "Floor ownership"): once a game update on the STATE channel, numbered by the owner's update and
// played back a few updates behind, as the knights are, so every update of the owner's is drawn.
// A frame too big for one packet goes in parts, and is drawn only once every part has come. The
// actors' bytes are coop_floor_actors', carried here unread.
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace kf::ext {

inline constexpr std::uint8_t coop_frame_tag = 0xf1;   // a STATE packet's first byte; a pose's is its version
inline constexpr std::size_t coop_frame_packet_most = 1100;   // VO_STATE_BYTES

struct CoopFramePart {
    int floor = 1;
    std::uint32_t update = 0;
    std::uint8_t part = 0;
    std::uint8_t parts = 1;
    std::vector<std::uint8_t> actors;
};

// The packets of one frame: `actors` split between actor records of `actor_bytes`, at most
// coop_frame_packet_most each. One packet for no actors.
std::vector<std::vector<std::uint8_t>> coop_frame_encode(int floor, std::uint32_t update,
    std::span<const std::uint8_t> actors, std::size_t actor_bytes);
// Nothing when the bytes are not a frame's packet: the wrong tag or size, a floor outside 1..5, a
// part past the parts.
std::optional<CoopFramePart> coop_frame_decode(std::span<const std::uint8_t> bytes);

class CoopFramePlayback {
public:
    explicit CoopFramePlayback(std::uint32_t delay_updates);

    // A packet as it arrives, in any order; one of a frame already played is dropped.
    void receive(const CoopFramePart &part);
    // One of this machine's updates: the actors of the newest whole frame at or before the
    // playhead, nothing before the first. Holds the last when none has come, catches up a step at
    // a time when behind, and jumps when far behind.
    std::optional<std::vector<std::uint8_t>> step();

private:
    struct Frame {
        std::uint8_t parts = 1;
        std::map<std::uint8_t, std::vector<std::uint8_t>> received;
        bool whole() const { return received.size() == parts; }
    };
    std::uint32_t delay_updates_;
    std::map<std::uint32_t, Frame> frames_;
    std::optional<std::uint32_t> playhead_;
    bool waiting_ = true;   // for the delay to build, at the start or after running out
};

}
