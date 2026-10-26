#include "online_pose.h"

#include <kf/platform/automap.h>

#include <bit>
#include <cmath>
#include <cstring>

namespace kf::ext {
namespace {

static_assert(std::endian::native == std::endian::little, "poses are sent little-endian as they are");

constexpr std::uint8_t pose_version = 1;
constexpr int floor_count = 5;

void put_float(std::uint8_t *at, float value) {
    std::memcpy(at, &value, sizeof value);
}

float get_float(const std::uint8_t *at) {
    float value = 0;
    std::memcpy(&value, at, sizeof value);
    return value;
}

bool on_map(float value, int cells) {
    return std::isfinite(value) && value >= 0.0f && value <= static_cast<float>(cells);
}

}

std::array<std::uint8_t, online_pose_bytes> online_pose_encode(const OnlinePose &pose) {
    std::array<std::uint8_t, online_pose_bytes> bytes{};
    bytes[0] = pose_version;
    bytes[1] = static_cast<std::uint8_t>(pose.floor);
    put_float(&bytes[2], pose.x);
    put_float(&bytes[6], pose.z);
    put_float(&bytes[10], pose.yaw);
    return bytes;
}

std::optional<OnlinePose> online_pose_decode(std::span<const std::uint8_t> bytes) {
    if (bytes.size() != online_pose_bytes || bytes[0] != pose_version)
        return std::nullopt;
    OnlinePose pose;
    pose.floor = bytes[1];
    pose.x = get_float(&bytes[2]);
    pose.z = get_float(&bytes[6]);
    pose.yaw = get_float(&bytes[10]);
    if (pose.floor < 1 || pose.floor > floor_count || !on_map(pose.x, automap_columns) ||
        !on_map(pose.z, automap_rows) || !std::isfinite(pose.yaw))
        return std::nullopt;
    return pose;
}

}
