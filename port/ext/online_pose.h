#pragma once
// A player's pose as sent to the other players in the room, on the STATE channel 20 times a
// second: 17 bytes, a version byte, the floor, x, z and yaw as little-endian floats, the weapon,
// and the attack phase as a little-endian s16.
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace kf::ext {

struct OnlinePose {
    int floor = 0;   // 1..5
    float x = 0;     // in cells, as AutomapSnapshot
    float z = 0;
    float yaw = 0;   // radians, as AutomapSnapshot
    std::uint8_t weapon = 0xff;       // 0..15, or 0xff for none
    std::int16_t attack_phase = -1;   // 0..4095 through a swing, or -1
};

inline constexpr std::size_t online_pose_bytes = 17;

std::array<std::uint8_t, online_pose_bytes> online_pose_encode(const OnlinePose &pose);
// Nothing when the bytes are not a pose: the wrong size or version, a floor outside 1..5, a
// coordinate that is not finite or is off the map, or a weapon or attack phase out of range.
std::optional<OnlinePose> online_pose_decode(std::span<const std::uint8_t> bytes);

}
