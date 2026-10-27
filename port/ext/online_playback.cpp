#include "online_playback.h"

#include <cmath>
#include <iterator>
#include <numbers>

namespace kf::ext {
namespace {

constexpr float cell_units = 2000.0f;          // KF_MAP_TILE_SIZE
constexpr float warp_units = 1000.0f;          // further than anyone walks in an update
constexpr std::int16_t attack_phase_step = 300; // KF_WEAPON_ATTACK_PHASE_STEP, every update of a swing
constexpr std::int16_t attack_phase_end = 4096;
constexpr std::uint32_t catch_up_after = 1;    // updates past the delay before playing two an update
constexpr std::uint32_t stall_updates = 32;    // updates past the delay before jumping
constexpr std::size_t heard_most = 128;

float lerp(float from, float to, float along) {
    return from + (to - from) * along;
}

float lerp_angle(float from, float to, float along) {
    constexpr float turn = 2.0f * std::numbers::pi_v<float>;
    return from + std::remainder(to - from, turn) * along;
}

// A swing advances a fixed step an update, so any pose inside it places every update of it.
std::int16_t attack_phase_between(const OnlinePose &before, const OnlinePose &after, std::uint32_t update) {
    if (after.attack_phase >= 0) {
        const std::int64_t phase = after.attack_phase - std::int64_t{attack_phase_step} * (after.update - update);
        if (phase >= 0)
            return static_cast<std::int16_t>(phase);
    }
    if (before.attack_phase >= 0) {
        const std::int64_t phase = before.attack_phase + std::int64_t{attack_phase_step} * (update - before.update);
        if (phase < attack_phase_end)
            return static_cast<std::int16_t>(phase);
    }
    return -1;
}

OnlinePose between(const OnlinePose &before, const OnlinePose &after, std::uint32_t update) {
    const auto span = static_cast<float>(after.update - before.update);
    const float distance = std::hypot(after.x - before.x, after.z - before.z) * cell_units;
    OnlinePose pose = before;
    pose.update = update;
    if (after.floor != before.floor || distance > warp_units * span)
        return pose;
    const float along = static_cast<float>(update - before.update) / span;
    pose.x = lerp(before.x, after.x, along);
    pose.z = lerp(before.z, after.z, along);
    pose.yaw = lerp_angle(before.yaw, after.yaw, along);
    pose.pitch = static_cast<std::int16_t>(std::lround(lerp(before.pitch, after.pitch, along)));
    pose.foot_height = static_cast<std::int32_t>(
        std::lround(lerp(static_cast<float>(before.foot_height), static_cast<float>(after.foot_height), along)));
    pose.attack_phase = attack_phase_between(before, after, update);
    return pose;
}

}

OnlinePlayback::OnlinePlayback(std::uint32_t delay_updates) : delay_updates_(delay_updates) {}

void OnlinePlayback::receive(const OnlinePose &pose) {
    if (playhead_ && pose.update < *playhead_)
        return;
    heard_.try_emplace(pose.update, pose);
    if (heard_.size() > heard_most)
        heard_.erase(heard_.begin());
}

std::optional<OnlinePose> OnlinePlayback::step() {
    if (heard_.empty())
        return std::nullopt;
    const std::uint32_t newest = heard_.rbegin()->first;
    if (!playhead_)
        playhead_ = heard_.begin()->first;
    std::uint32_t &playhead = *playhead_;
    const std::uint32_t behind = newest - playhead;
    if (behind > delay_updates_ + stall_updates) {
        playhead = newest - delay_updates_;
        waiting_ = false;
    } else if (behind == 0) {
        waiting_ = true;
    } else if (!waiting_ || behind > delay_updates_) {
        waiting_ = false;
        playhead += behind > delay_updates_ + catch_up_after ? 2 : 1;
    }
    // Keep the last pose at or before the playhead, to fill the gap after it.
    if (const auto after = heard_.upper_bound(playhead); after != heard_.begin())
        heard_.erase(heard_.begin(), std::prev(after));
    return pose_at(playhead);
}

OnlinePose OnlinePlayback::pose_at(std::uint32_t update) const {
    const auto after = heard_.lower_bound(update);
    if (after == heard_.end())
        return heard_.rbegin()->second;
    if (after->first == update || after == heard_.begin())
        return after->second;
    return between(std::prev(after)->second, after->second, update);
}

}
