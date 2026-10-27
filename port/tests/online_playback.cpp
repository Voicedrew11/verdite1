#include "../ext/online_playback.h"

#include <doctest/doctest.h>

#include <cmath>
#include <numbers>

using namespace kf::ext;

namespace {

// A pose on floor 1 at update n, walking along +z a tenth of a cell an update.
OnlinePose at(std::uint32_t update) {
    OnlinePose pose{1, 10.0f, 10.0f + 0.1f * static_cast<float>(update % 100), 0.0f};
    pose.update = update;
    return pose;
}

OnlinePose next(OnlinePlayback &playback) {
    const auto pose = playback.step();
    REQUIRE(pose.has_value());
    return pose.value_or(OnlinePose{});
}

std::uint32_t played(OnlinePlayback &playback) {
    return next(playback).update;
}

}

TEST_CASE("nothing is played before the first pose") {
    OnlinePlayback playback(2);
    CHECK_FALSE(playback.step().has_value());
}

TEST_CASE("playback waits for the delay, then plays one update an update") {
    OnlinePlayback playback(2);
    playback.receive(at(100));
    CHECK(played(playback) == 100);
    playback.receive(at(101));
    CHECK(played(playback) == 100);
    playback.receive(at(102));
    CHECK(played(playback) == 100);
    for (std::uint32_t update = 103; update < 110; ++update) {
        playback.receive(at(update));
        CHECK(played(playback) == update - 2);
    }
}

TEST_CASE("a lost pose is filled in between its neighbours") {
    OnlinePlayback playback(2);
    for (std::uint32_t update = 0; update < 6; ++update)
        if (update != 3)
            playback.receive(at(update));
    CHECK(played(playback) == 2);
    const OnlinePose pose = next(playback);
    CHECK(pose.update == 3);
    CHECK(pose.z == doctest::Approx(10.3f));
}

TEST_CASE("a pose that comes late but before it is played is used") {
    OnlinePlayback playback(2);
    playback.receive(at(0));
    playback.step();
    OnlinePose late = at(1);
    late.x = 12.0f;
    playback.receive(at(2));
    playback.receive(late);
    playback.step();
    playback.receive(at(3));
    const OnlinePose pose = next(playback);
    CHECK(pose.update == 1);
    CHECK(pose.x == 12.0f);
}

TEST_CASE("running out holds the last pose, and the delay builds again") {
    OnlinePlayback playback(2);
    std::uint32_t update = 0;
    for (; update < 5; ++update) {
        playback.receive(at(update));
        playback.step();
    }
    // Nothing comes for a while: it plays up to the newest and holds it.
    CHECK(played(playback) == 3);
    for (int held = 0; held < 3; ++held)
        CHECK(played(playback) == 4);
    // Poses come again: it waits until it is the delay behind before going on.
    playback.receive(at(update++));
    CHECK(played(playback) == 4);
    playback.receive(at(update++));
    CHECK(played(playback) == 4);
    playback.receive(at(update++));
    CHECK(played(playback) == 5);
}

TEST_CASE("behind the delay, it plays two an update until it is back") {
    OnlinePlayback playback(2);
    playback.receive(at(0));
    playback.step();
    std::uint32_t next = 1;
    for (; next <= 10; ++next)
        playback.receive(at(next));
    for (const std::uint32_t expected : {2u, 4u, 6u, 8u, 10u, 12u, 14u, 16u, 17u, 18u}) {
        playback.receive(at(next++));
        CHECK(played(playback) == expected);
    }
}

TEST_CASE("far behind, it jumps to the delay") {
    OnlinePlayback playback(2);
    playback.receive(at(0));
    playback.step();
    playback.receive(at(500));
    CHECK(played(playback) == 498);
}

TEST_CASE("a pose already played is dropped") {
    OnlinePlayback playback(2);
    for (std::uint32_t update = 0; update < 8; ++update) {
        playback.receive(at(update));
        playback.step();
    }
    OnlinePose stale = at(3);
    stale.x = 50.0f;
    playback.receive(stale);
    playback.receive(at(8));
    const OnlinePose pose = next(playback);
    CHECK(pose.x == 10.0f);
}

TEST_CASE("a swing is placed from any pose inside it") {
    OnlinePlayback playback(3);
    OnlinePose inside = at(4);
    inside.attack_phase = 900;
    playback.receive(at(0));
    playback.receive(inside);
    CHECK(next(playback).attack_phase == 0); // update 1, where the swing began
    CHECK(next(playback).attack_phase == 300);
    CHECK(next(playback).attack_phase == 600);
    CHECK(next(playback).attack_phase == 900);
}

TEST_CASE("a swing's end is placed from its last pose") {
    OnlinePlayback playback(3);
    OnlinePose inside = at(0);
    inside.attack_phase = 3600;
    playback.receive(inside);
    playback.receive(at(4));
    CHECK(next(playback).attack_phase == 3900); // update 1
    CHECK(next(playback).attack_phase == -1);   // past the end
}

TEST_CASE("the facing turns the short way between two poses") {
    OnlinePlayback playback(1);
    const float pi = std::numbers::pi_v<float>;
    OnlinePose before = at(0);
    before.yaw = pi - 0.1f;
    OnlinePose after = at(2);
    after.yaw = -pi + 0.1f;
    playback.receive(before);
    playback.receive(after);
    const OnlinePose pose = next(playback);
    CHECK(std::remainder(pose.yaw - pi, 2.0f * pi) == doctest::Approx(0.0f).epsilon(0.001));
}

TEST_CASE("a warp or a change of floor is not filled in") {
    OnlinePlayback playback(1);
    OnlinePose after = at(2);
    after.x = 60.0f;
    playback.receive(at(0));
    playback.receive(after);
    const OnlinePose pose = next(playback);
    CHECK(pose.x == 10.0f);
}

TEST_CASE("the height is filled in between two poses") {
    OnlinePlayback playback(1);
    OnlinePose before = at(0);
    before.foot_height = -1000;
    OnlinePose after = at(2);
    after.foot_height = -2000;
    playback.receive(before);
    playback.receive(after);
    CHECK(next(playback).foot_height == -1500);
}
