#include "online_stride.h"

#include <algorithm>
#include <cmath>

namespace kf::ext {
namespace {

constexpr float cell_units = 2000.0f;       // KF_MAP_TILE_SIZE
constexpr float full_stride_units = 150.0f; // a pose's move at which the stride is full (a run is 180)
constexpr float warp_units = 1000.0f;       // further than anyone walks in an update
constexpr float amount_easing = 0.5f;       // of the way to the new amount, each pose
constexpr int held_still_poses = 2;         // a stride kept through this many poses without a move

}

OnlineStride online_stride_step(const OnlineStride &stride, const OnlinePose &from, const OnlinePose &to) {
    const float dx = (to.x - from.x) * cell_units;
    const float dz = (to.z - from.z) * cell_units;
    const float distance = std::hypot(dx, dz);
    if (to.floor != from.floor || distance > warp_units)
        return {stride.phase, 0.0f, 0};
    if (distance == 0.0f && stride.still < held_still_poses)
        return {stride.phase, stride.amount, stride.still + 1};
    // The camera walks along (-sin h, cos h) for the automap's yaw h.
    const float forward = -dx * std::sin(to.yaw) + dz * std::cos(to.yaw);
    const float walked = forward < 0.0f ? -distance : distance;
    float phase = std::fmod(stride.phase + walked / online_stride_cycle, 1.0f);
    if (phase < 0.0f)
        phase += 1.0f;
    const float target = std::min(distance / full_stride_units, 1.0f);
    return {phase, stride.amount + (target - stride.amount) * amount_easing, distance == 0.0f ? stride.still + 1 : 0};
}

}
