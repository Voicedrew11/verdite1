#include <verdite/pickup_feed.h>

#include <doctest/doctest.h>

using namespace verdite;

namespace {

PickupLine line(std::uint32_t player, std::uint16_t item, std::uint32_t count = 1) {
    return {player, 0, item, count};
}

} // namespace

TEST_CASE("a line shows, then fades, then is forgotten") {
    PickupFeed feed({5, 4000, 600, 1500});
    feed.add(line(1, 7), 1000);
    REQUIRE(feed.shown(1000).size() == 1);
    CHECK(feed.shown(5000)[0].opacity == doctest::Approx(1.0f));
    CHECK(feed.shown(5300)[0].opacity == doctest::Approx(0.5f));
    CHECK(feed.shown(5600).empty());
    CHECK(feed.shown(1000).empty());
}

TEST_CASE("the same find within the merge time adds to its line") {
    PickupFeed feed({5, 4000, 600, 1500});
    feed.add(line(1, 7), 0);
    feed.add(line(2, 9), 100);
    feed.add(line(1, 7, 2), 1000);
    const auto lines = feed.shown(1000);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].line.item == 9);
    CHECK(lines[1].line.item == 7);
    CHECK(lines[1].line.count == 3);
    // Shown again from the merge: still there after the first add's time is up.
    CHECK(feed.shown(4900).size() == 1);
}

TEST_CASE("each merge restarts the merge time, and an old line is not merged into") {
    PickupFeed feed({5, 4000, 600, 1500});
    feed.add(line(1, 7), 0);
    feed.add(line(1, 7), 1400);
    feed.add(line(1, 7), 2800);
    REQUIRE(feed.shown(2800).size() == 1);
    CHECK(feed.shown(2800)[0].line.count == 3);
    feed.add(line(1, 7), 4400);
    CHECK(feed.shown(4400).size() == 2);
}

TEST_CASE("another player, action or item is a line of its own") {
    PickupFeed feed;
    feed.add(line(1, 7), 0);
    feed.add(line(2, 7), 0);
    feed.add(line(1, 8), 0);
    feed.add({1, 1, 7, 1}, 0);
    CHECK(feed.shown(0).size() == 4);
}

TEST_CASE("past the most lines the oldest goes first") {
    PickupFeed feed({3, 4000, 600, 1500});
    for (std::uint16_t item = 0; item < 5; ++item)
        feed.add(line(1, item), item);
    const auto lines = feed.shown(10);
    REQUIRE(lines.size() == 3);
    CHECK(lines[0].line.item == 2);
    CHECK(lines[2].line.item == 4);
}

TEST_CASE("counts stop at their most, and no fade or no lines is no trouble") {
    PickupFeed feed({0, 100, 0, 1500});
    feed.add(line(1, 7, 0xffffffffu), 0);
    feed.add(line(1, 7, 5), 0);
    REQUIRE(feed.shown(50).size() == 1);
    CHECK(feed.shown(50)[0].line.count == 0xffffffffu);
    CHECK(feed.shown(100).empty());
    feed.add(line(1, 7), 0);
    feed.clear();
    CHECK(feed.shown(0).empty());
}
