#include "../ext/online_stride.h"

#include <doctest/doctest.h>

#include <numbers>

using namespace kf::ext;

namespace {

// A pose on floor 1 facing along +z (yaw 0), moved by dz cells.
OnlinePose at(float x, float z, float yaw = 0.0f, int floor = 1) {
    return {floor, x, z, yaw, 0xff, -1};
}

constexpr float run_cells = 180.0f / 2000.0f;   // a run: 180 units an update

}

TEST_CASE("walking forward advances the cycle by the distance walked") {
    const auto stride = online_stride_step({}, at(10.0f, 10.0f), at(10.0f, 10.0f + run_cells));
    CHECK(stride.phase == doctest::Approx(180.0f / online_stride_cycle));
    CHECK(stride.amount == doctest::Approx(0.5f));
}

TEST_CASE("forward follows the facing") {
    // Facing a quarter turn, the camera walks along -x.
    const float quarter = std::numbers::pi_v<float> / 2;
    const auto forward = online_stride_step({}, at(10.0f, 10.0f, quarter), at(10.0f - run_cells, 10.0f, quarter));
    CHECK(forward.phase == doctest::Approx(180.0f / online_stride_cycle));
    const auto back = online_stride_step({}, at(10.0f, 10.0f, quarter), at(10.0f + run_cells, 10.0f, quarter));
    CHECK(back.phase == doctest::Approx(1.0f - 180.0f / online_stride_cycle));
}

TEST_CASE("the cycle wraps") {
    const auto stride = online_stride_step({0.95f, 1.0f}, at(10.0f, 10.0f), at(10.0f, 10.0f + run_cells));
    CHECK(stride.phase == doctest::Approx(0.95f + 180.0f / online_stride_cycle - 1.0f));
}

TEST_CASE("a player who stops eases to standing, keeping their phase") {
    OnlineStride stride{0.3f, 1.0f};
    for (int update = 0; update < 12; ++update)
        stride = online_stride_step(stride, at(10.0f, 10.0f), at(10.0f, 10.0f));
    CHECK(stride.phase == doctest::Approx(0.3f));
    CHECK(stride.amount < 0.001f);
}

TEST_CASE("a pose that repeats the last keeps the stride") {
    const OnlineStride walking{0.4f, 1.0f};
    auto stride = online_stride_step(walking, at(10.0f, 10.0f), at(10.0f, 10.0f));
    stride = online_stride_step(stride, at(10.0f, 10.0f), at(10.0f, 10.0f));
    CHECK(stride.amount == 1.0f);
    stride = online_stride_step(stride, at(10.0f, 10.0f), at(10.0f, 10.0f + run_cells));
    CHECK(stride.amount == 1.0f);
    CHECK(stride.still == 0);
}

TEST_CASE("a warp or a change of floor leaves the player standing") {
    const OnlineStride walking{0.4f, 1.0f, 0};
    const auto warped = online_stride_step(walking, at(10.0f, 10.0f), at(40.0f, 10.0f));
    CHECK(warped.amount == 0.0f);
    CHECK(warped.phase == doctest::Approx(0.4f));
    const auto stairs = online_stride_step(walking, at(10.0f, 10.0f, 0.0f, 1), at(10.0f, 10.0f + run_cells, 0.0f, 2));
    CHECK(stairs.amount == 0.0f);
}
