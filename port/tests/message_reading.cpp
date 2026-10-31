#include "../ext/message_reading.h"

#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <cstdint>
#include <numbers>
#include <vector>

using namespace kf::ext;

namespace {
constexpr float quarter_turn = std::numbers::pi_v<float> / 2.0f;
} // namespace

TEST_CASE("a point lies in the cell its coordinates' floors name") {
    CHECK(message_cell_at(3.7f, 0.0f) == MessageCell{3, 0});
    CHECK(message_cell_at(9.999f, 10.0f) == MessageCell{9, 10});
    CHECK(message_cell_at(-0.5f, 2.5f) == MessageCell{-1, 2});
}

TEST_CASE("a point not finite or far off the map lies in no message cell") {
    const float nan = std::nanf("");
    const float infinity = std::numeric_limits<float>::infinity();
    CHECK(message_cell_at(nan, 1.0f).x < 0);
    CHECK(message_cell_at(infinity, 1.0f).x > message_cell_most);
    CHECK(message_cell_at(1.0f, -infinity).z < 0);
    CHECK(message_cell_at(1e9f, 1.0f).x > message_cell_most);
}

TEST_CASE("the cell ahead is one along the way the player looks") {
    // yaw 0 faces +z (north); the turn towards -x is a positive yaw
    CHECK(message_cell_ahead(5.5f, 5.5f, 0.0f) == MessageCell{5, 6});
    CHECK(message_cell_ahead(5.5f, 5.5f, quarter_turn) == MessageCell{4, 5});
    CHECK(message_cell_ahead(5.5f, 5.5f, -quarter_turn) == MessageCell{6, 5});
    CHECK(message_cell_ahead(5.5f, 5.5f, 2.0f * quarter_turn) == MessageCell{5, 4});
}

TEST_CASE("the action button is for the message in the player's cell first") {
    const std::vector<MessageCell> cells = {{5, 6}, {5, 5}};   // shown order
    CHECK(message_for_action(cells, 5.5f, 5.5f, 0.0f) == 1u);   // own cell, though one is ahead
}

TEST_CASE("the first message in shown order in a cell is the one the button is for") {
    const std::vector<MessageCell> cells = {{2, 2}, {5, 5}, {5, 5}};
    CHECK(message_for_action(cells, 5.5f, 5.5f, 0.0f) == 1u);
}

TEST_CASE("with none in the player's cell, the one ahead; with none at all, none") {
    const std::vector<MessageCell> ahead = {{7, 7}, {5, 6}};
    CHECK(message_for_action(ahead, 5.5f, 5.5f, 0.0f) == 1u);
    const std::vector<MessageCell> behind = {{5, 4}};
    CHECK_FALSE(message_for_action(behind, 5.5f, 5.5f, 0.0f).has_value());
    const std::vector<MessageCell> none;
    CHECK_FALSE(message_for_action(none, 5.5f, 5.5f, 0.0f).has_value());
}

TEST_CASE("a message two cells ahead is not for the action button") {
    const std::vector<MessageCell> cells = {{5, 7}};
    CHECK_FALSE(message_for_action(cells, 5.5f, 5.5f, 0.0f).has_value());
}

TEST_CASE("a place is cell x, cell z, facing in 256ths of a turn, and a zero byte") {
    const std::uint32_t place = 3u | (7u << 8) | (64u << 16);
    CHECK(message_place_encode({3, 7}, quarter_turn) == place);
    CHECK(message_place_decode(place) == MessagePlace{{3, 7}, 64});
}

TEST_CASE("a negative yaw faces the same way as its turn around") {
    const std::uint32_t place = message_place_encode({0, 0}, -quarter_turn).value_or(0);
    CHECK(message_place_decode(place) == MessagePlace{{0, 0}, 192});
}

TEST_CASE("a place off the map is refused in both directions") {
    CHECK_FALSE(message_place_encode({101, 0}, 0.0f).has_value());
    CHECK_FALSE(message_place_encode({0, -1}, 0.0f).has_value());
    CHECK(message_place_encode({message_cell_most, message_cell_most}, 0.0f).has_value());
    CHECK_FALSE(message_place_decode(101u).has_value());
    CHECK_FALSE(message_place_decode(3u | (101u << 8)).has_value());
    CHECK(message_place_decode(100u | (100u << 8)).has_value());
}

TEST_CASE("a place whose zero byte is set is refused") {
    CHECK_FALSE(message_place_decode(3u | (7u << 8) | (1u << 24)).has_value());
}

TEST_CASE("a message stays open while the player is within a cell of it, and closes past that") {
    OpenMessage reading;
    reading.open(42, {5, 5});
    reading.follow(6.5f, 6.5f);   // one cell away diagonally
    REQUIRE(reading.current());
    CHECK(reading.current() == OpenMessage::Open{42, {5, 5}});
    reading.follow(7.5f, 5.5f);   // two cells away
    CHECK_FALSE(reading.current().has_value());
}

TEST_CASE("a press of the button rates a message that can be rated, then closes it") {
    OpenMessage reading;
    reading.open(1, {5, 5});
    CHECK(reading.press(MessageRating::Possible) == MessagePress::Rate);
    CHECK_FALSE(reading.current().has_value());
}

TEST_CASE("a press closes a message that cannot be rated, without rating it") {
    OpenMessage reading;
    reading.open(1, {5, 5});
    CHECK(reading.press(MessageRating::Impossible) == MessagePress::Close);
    CHECK_FALSE(reading.current().has_value());
}

TEST_CASE("a press with nothing open does nothing") {
    OpenMessage reading;
    CHECK(reading.press(MessageRating::Possible) == MessagePress::Close);
    CHECK_FALSE(reading.current().has_value());
}
