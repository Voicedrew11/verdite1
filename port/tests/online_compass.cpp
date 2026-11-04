#include <kf/platform/online_compass.h>

#include <doctest/doctest.h>

#include <vector>

using namespace kf;

namespace {

constexpr OnlineCompassRect dial = {269, 18, 302, 50};   // the game's sprite

OnlinePlayer player_at(float x, float z, u8 colour = 0) {
    OnlinePlayer player{};
    player.x = x;
    player.z = z;
    player.colour = colour;
    return player;
}

}

TEST_CASE("a player's way picks the nearest point: north is +z, east is +x") {
    CHECK(online_compass_point(0, 5) == OnlineCompassPoint::North);
    CHECK(online_compass_point(5, 1) == OnlineCompassPoint::East);
    CHECK(online_compass_point(1, -5) == OnlineCompassPoint::South);
    CHECK(online_compass_point(-5, 1) == OnlineCompassPoint::West);
    CHECK(online_compass_point(3, 3) == OnlineCompassPoint::North);
    CHECK(online_compass_point(-3, -3) == OnlineCompassPoint::South);
    CHECK_FALSE(online_compass_point(0, 0));
}

TEST_CASE("every dot is 4 pixels square and clear of the dial, on its own side") {
    const std::array points = {OnlineCompassPoint::North, OnlineCompassPoint::East, OnlineCompassPoint::South, OnlineCompassPoint::West};
    for (const OnlineCompassPoint point : points) {
        const OnlineCompassRect dot = online_compass_dot_at(dial, point, 0, 1);
        CHECK(dot.right - dot.left == 4);
        CHECK(dot.bottom - dot.top == 4);
        const bool outside = dot.bottom <= dial.top || dot.top >= dial.bottom || dot.right <= dial.left || dot.left >= dial.right;
        CHECK(outside);
    }
    CHECK(online_compass_dot_at(dial, OnlineCompassPoint::North, 0, 1).bottom < dial.top);
    CHECK(online_compass_dot_at(dial, OnlineCompassPoint::South, 0, 1).top > dial.bottom);
    CHECK(online_compass_dot_at(dial, OnlineCompassPoint::West, 0, 1).right < dial.left);
    CHECK(online_compass_dot_at(dial, OnlineCompassPoint::East, 0, 1).left > dial.right);
}

TEST_CASE("a dot sits at the middle of its side, on the dial's letter") {
    const OnlineCompassRect north = online_compass_dot_at(dial, OnlineCompassPoint::North, 0, 1);
    CHECK(north.left == 284);
    const OnlineCompassRect west = online_compass_dot_at(dial, OnlineCompassPoint::West, 0, 1);
    CHECK(west.top == 32);
}

TEST_CASE("players at one point stand in a row without touching, and all stay on the picture") {
    const std::vector<OnlinePlayer> players = {player_at(50, 60, 1), player_at(50, 70, 2), player_at(51, 80, 3)};
    const auto dots = online_compass_dots(dial, 50, 50, players);
    REQUIRE(dots.size() == 3);
    CHECK(dots[0].colour == 1);
    CHECK(dots[2].colour == 3);
    CHECK(dots[1].rect.left == dots[0].rect.right + online_compass_dot_gap_px);
    CHECK(dots[2].rect.left == dots[1].rect.right + online_compass_dot_gap_px);
    CHECK(dots[0].rect.top == dots[2].rect.top);
    const OnlineCompassRect far_east = online_compass_dot_at(dial, OnlineCompassPoint::East, 4, 5);
    CHECK(far_east.right <= 320);
}

TEST_CASE("no dot for the player whose eyes these are, or for one exactly here") {
    OnlinePlayer watched = player_at(60, 50);
    watched.eyes = true;
    const std::vector<OnlinePlayer> players = {watched, player_at(50, 50)};
    CHECK(online_compass_dots(dial, 50, 50, players).empty());
}
