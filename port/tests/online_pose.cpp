#include "../ext/online_pose.h"

#include <doctest/doctest.h>

#include <limits>

using namespace kf::ext;

TEST_CASE("a pose sent is the pose received") {
    const OnlinePose sent{3, 41.5f, 12.25f, -1.5f, 2, 3071, -800, 123456, -5120};
    const auto bytes = online_pose_encode(sent);
    const auto received = online_pose_decode(bytes);
    REQUIRE(received.has_value());
    const OnlinePose pose = received.value_or(OnlinePose{});
    CHECK(pose.floor == 3);
    CHECK(pose.x == 41.5f);
    CHECK(pose.z == 12.25f);
    CHECK(pose.yaw == -1.5f);
    CHECK(pose.weapon == 2);
    CHECK(pose.attack_phase == 3071);
    CHECK(pose.pitch == -800);
    CHECK(pose.update == 123456);
    CHECK(pose.foot_height == -5120);
}

TEST_CASE("a player with no weapon, not attacking, is a pose") {
    const auto received = online_pose_decode(online_pose_encode({1, 10.0f, 10.0f, 0.0f, 0xff, -1}));
    REQUIRE(received.has_value());
    CHECK(received.value_or(OnlinePose{}).weapon == 0xff);
    CHECK(received.value_or(OnlinePose{}).attack_phase == -1);
}

TEST_CASE("a weapon or attack phase out of range is refused") {
    CHECK_FALSE(online_pose_decode(online_pose_encode({1, 10.0f, 10.0f, 0.0f, 16, -1})));
    CHECK_FALSE(online_pose_decode(online_pose_encode({1, 10.0f, 10.0f, 0.0f, 2, 4096})));
    CHECK_FALSE(online_pose_decode(online_pose_encode({1, 10.0f, 10.0f, 0.0f, 2, -2})));
}

TEST_CASE("a pitch past straight up or down is refused") {
    CHECK(online_pose_decode(online_pose_encode({1, 10.0f, 10.0f, 0.0f, 2, -1, 1024})));
    CHECK(online_pose_decode(online_pose_encode({1, 10.0f, 10.0f, 0.0f, 2, -1, -1024})));
    CHECK_FALSE(online_pose_decode(online_pose_encode({1, 10.0f, 10.0f, 0.0f, 2, -1, 1025})));
    CHECK_FALSE(online_pose_decode(online_pose_encode({1, 10.0f, 10.0f, 0.0f, 2, -1, -1025})));
}

TEST_CASE("a height past any fall is refused") {
    CHECK(online_pose_decode(online_pose_encode({1, 10.0f, 10.0f, 0.0f, 2, -1, 0, 0, 1 << 20})));
    CHECK_FALSE(online_pose_decode(online_pose_encode({1, 10.0f, 10.0f, 0.0f, 2, -1, 0, 0, (1 << 20) + 1})));
    CHECK_FALSE(online_pose_decode(online_pose_encode({1, 10.0f, 10.0f, 0.0f, 2, -1, 0, 0, -(1 << 20) - 1})));
}

TEST_CASE("bytes that are not a pose are refused") {
    auto bytes = online_pose_encode({1, 10.0f, 10.0f, 0.0f});
    CHECK_FALSE(online_pose_decode(std::span(bytes).first(online_pose_bytes - 1)));
    auto wrong_version = bytes;
    wrong_version[0] = 3;
    CHECK_FALSE(online_pose_decode(wrong_version));
    CHECK_FALSE(online_pose_decode(online_pose_encode({0, 10.0f, 10.0f, 0.0f})));
    CHECK_FALSE(online_pose_decode(online_pose_encode({6, 10.0f, 10.0f, 0.0f})));
}

TEST_CASE("a pose off the map or not finite is refused") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    CHECK_FALSE(online_pose_decode(online_pose_encode({1, -0.5f, 10.0f, 0.0f})));
    CHECK_FALSE(online_pose_decode(online_pose_encode({1, 10.0f, 100.5f, 0.0f})));
    CHECK_FALSE(online_pose_decode(online_pose_encode({1, nan, 10.0f, 0.0f})));
    CHECK_FALSE(online_pose_decode(online_pose_encode({1, 10.0f, 10.0f, infinity})));
}

TEST_CASE("a pose says whether its player is down or in a fight") {
    for (const bool down : {false, true})
        for (const bool fighting : {false, true}) {
            OnlinePose pose{1, 10.0f, 20.0f, 0.5f, 3, -1, 0, 7, -400};
            pose.down = down;
            pose.fighting = fighting;
            const auto received = online_pose_decode(online_pose_encode(pose));
            REQUIRE(received.has_value());
            CHECK(received.value_or(OnlinePose{}).down == down);
            CHECK(received.value_or(OnlinePose{}).fighting == fighting);
        }
    auto unknown = online_pose_encode({1, 10.0f, 20.0f, 0.5f, 3, -1, 0, 7, -400});
    unknown.back() = 4;
    CHECK_FALSE(online_pose_decode(unknown));
}
