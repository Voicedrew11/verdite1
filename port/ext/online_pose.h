#pragma once
// A player's pose as sent to the other players in the room, on the STATE channel once per game
// update: 28 bytes, a version byte, the floor, x, z and yaw as little-endian floats, the weapon,
// the attack phase and pitch as little-endian s16s, then the update it was sent in (u32), the
// height of the feet (s32), little-endian, and a byte of flags: down (1), in a fight (2).
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
    std::int16_t pitch = 0;           // the camera's, in 1/4096 turns, positive looking down
    std::uint32_t update = 0;         // the sender's count of game updates
    std::int32_t foot_height = 0;     // the feet's y in the game's units, up is negative
    bool down = false;                // out of the world until they come back (docs/COOP_PLAN.md, "Death")
    bool fighting = false;            // hit or hitting lately, or chased: no one comes back beside them yet
};

inline constexpr std::size_t online_pose_bytes = 28;

std::array<std::uint8_t, online_pose_bytes> online_pose_encode(const OnlinePose &pose);
// Nothing when the bytes are not a pose: the wrong size or version, a floor outside 1..5, a
// coordinate that is not finite or is off the map, or a weapon, attack phase, pitch, height or
// flag out of range.
std::optional<OnlinePose> online_pose_decode(std::span<const std::uint8_t> bytes);

}
