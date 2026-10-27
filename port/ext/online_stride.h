#pragma once
// How far through a walk another player's legs are, from the poses they send: the cycle advances
// by the distance they move, so their feet keep pace with the ground (docs/PLAYER_MODEL.md).
#include "online_pose.h"

namespace kf::ext {

struct OnlineStride {
    float phase = 0;   // 0..1 through the cycle; the right foot leaves the ground at 0, the left at 0.5
    float amount = 0;  // 0 standing .. 1 a full stride
    int still = 0;     // poses in a row that did not move
};

// Distance walked in one cycle, in the game's units: the knight's planted foot sweeps half of it.
inline constexpr float online_stride_cycle = 1700.0f;

// The stride once a player has moved from one pose to the next. A step back plays the cycle
// backwards; the amount follows their speed, held through a pose or two that repeat the last (poses
// go out on the wall clock, which can fall between two of the game's updates); a change of floor or
// a jump further than anyone walks in an update (a warp) leaves them standing.
OnlineStride online_stride_step(const OnlineStride &stride, const OnlinePose &from, const OnlinePose &to);

}
