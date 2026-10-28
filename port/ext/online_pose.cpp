#include "online_pose.h"

#include <kf/platform/automap.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace kf::ext {
namespace {

static_assert(std::endian::native == std::endian::little, "poses are sent little-endian as they are");

constexpr std::uint8_t pose_version = 6;
constexpr std::uint8_t flag_down = 1;
constexpr std::uint8_t flag_fighting = 2;
constexpr int floor_count = 5;
constexpr std::uint8_t weapon_count = 16;
constexpr std::uint8_t no_weapon = 0xff;
constexpr std::int16_t attack_phase_end = 4096;
constexpr std::int16_t pitch_most = 1024; // straight up or down
constexpr std::int32_t height_most = 1 << 20; // far below any floor, as deep as a fall goes

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

bool known_weapon(std::uint8_t weapon) {
    return weapon < weapon_count || weapon == no_weapon;
}

bool known_attack_phase(std::int16_t phase) {
    return phase == -1 || (phase >= 0 && phase < attack_phase_end);
}

bool known_pitch(std::int16_t pitch) {
    return pitch >= -pitch_most && pitch <= pitch_most;
}

bool known_height(std::int32_t height) {
    return height >= -height_most && height <= height_most;
}

}

std::vector<std::uint8_t> online_pose_encode(const OnlinePose &pose) {
    std::vector<std::uint8_t> bytes(online_pose_bytes);
    bytes[0] = pose_version;
    bytes[1] = static_cast<std::uint8_t>(pose.floor);
    put_float(&bytes[2], pose.x);
    put_float(&bytes[6], pose.z);
    put_float(&bytes[10], pose.yaw);
    bytes[14] = pose.weapon;
    std::memcpy(&bytes[15], &pose.attack_phase, sizeof pose.attack_phase);
    std::memcpy(&bytes[17], &pose.pitch, sizeof pose.pitch);
    std::memcpy(&bytes[19], &pose.update, sizeof pose.update);
    std::memcpy(&bytes[23], &pose.foot_height, sizeof pose.foot_height);
    bytes[27] = static_cast<std::uint8_t>((pose.down ? flag_down : 0) | (pose.fighting ? flag_fighting : 0));
    std::memcpy(&bytes[28], pose.sounds.data(), pose.sounds.size());
    const std::size_t effects = std::min(pose.effects.size(), online_pose_effects_most);
    bytes.insert(bytes.end(), pose.effects.begin(), pose.effects.begin() + static_cast<std::ptrdiff_t>(effects));
    return bytes;
}

std::optional<OnlinePose> online_pose_decode(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < online_pose_bytes || bytes.size() > online_pose_most || bytes[0] != pose_version)
        return std::nullopt;
    OnlinePose pose;
    pose.floor = bytes[1];
    pose.x = get_float(&bytes[2]);
    pose.z = get_float(&bytes[6]);
    pose.yaw = get_float(&bytes[10]);
    pose.weapon = bytes[14];
    std::memcpy(&pose.attack_phase, &bytes[15], sizeof pose.attack_phase);
    std::memcpy(&pose.pitch, &bytes[17], sizeof pose.pitch);
    std::memcpy(&pose.update, &bytes[19], sizeof pose.update);
    std::memcpy(&pose.foot_height, &bytes[23], sizeof pose.foot_height);
    const std::uint8_t flags = bytes[27];
    pose.down = (flags & flag_down) != 0;
    pose.fighting = (flags & flag_fighting) != 0;
    std::memcpy(pose.sounds.data(), &bytes[28], pose.sounds.size());
    pose.effects.assign(bytes.begin() + online_pose_bytes, bytes.end());
    if ((flags & ~(flag_down | flag_fighting)) != 0)
        return std::nullopt;
    if (pose.floor < 1 || pose.floor > floor_count || !on_map(pose.x, automap_columns) ||
        !on_map(pose.z, automap_rows) || !std::isfinite(pose.yaw) || !known_weapon(pose.weapon) ||
        !known_attack_phase(pose.attack_phase) || !known_pitch(pose.pitch) || !known_height(pose.foot_height))
        return std::nullopt;
    return pose;
}

std::vector<std::size_t> online_pose_sounds_between(const OnlinePose &was, const OnlinePose &now) {
    std::vector<std::size_t> sounds;
    for (std::size_t sound = 0; sound < online_sound_kinds; ++sound)
        if (was.sounds[sound] != now.sounds[sound])
            sounds.push_back(sound);
    return sounds;
}

}
