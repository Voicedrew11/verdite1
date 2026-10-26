#pragma once
// A player's pose as sent to the other players in the room, on the STATE channel 20 times a
// second: 14 bytes, a version byte, the floor, then x, z and yaw as little-endian floats.
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
};

inline constexpr std::size_t online_pose_bytes = 14;

std::array<std::uint8_t, online_pose_bytes> online_pose_encode(const OnlinePose &pose);
// Nothing when the bytes are not a pose: the wrong size or version, a floor outside 1..5, or a
// coordinate that is not finite or is off the map.
std::optional<OnlinePose> online_pose_decode(std::span<const std::uint8_t> bytes);

}
