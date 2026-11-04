#include "../ext/message_reading.h"

#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <cstdint>
#include <numbers>
#include <span>
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

TEST_CASE("the action button is for the messages in the player's cell first") {
    const std::vector<MessageCell> cells = {{5, 6}, {5, 5}};
    CHECK(message_cell_for_action(cells, 5.5f, 5.5f, 0.0f, MessageReach::HereAndAhead) == MessageCell{5, 5});
}

TEST_CASE("with none in the player's cell, the one ahead; with none at all, none") {
    const std::vector<MessageCell> ahead = {{7, 7}, {5, 6}};
    CHECK(message_cell_for_action(ahead, 5.5f, 5.5f, 0.0f, MessageReach::HereAndAhead) == MessageCell{5, 6});
    const std::vector<MessageCell> behind = {{5, 4}};
    CHECK_FALSE(message_cell_for_action(behind, 5.5f, 5.5f, 0.0f, MessageReach::HereAndAhead).has_value());
    const std::vector<MessageCell> none;
    CHECK_FALSE(message_cell_for_action(none, 5.5f, 5.5f, 0.0f, MessageReach::HereAndAhead).has_value());
}

TEST_CASE("a message two cells ahead is not for the action button") {
    const std::vector<MessageCell> cells = {{5, 7}};
    CHECK_FALSE(message_cell_for_action(cells, 5.5f, 5.5f, 0.0f, MessageReach::HereAndAhead).has_value());
}

TEST_CASE("a wall before the cell ahead leaves only the player's own") {
    const std::vector<MessageCell> ahead = {{5, 6}};
    CHECK_FALSE(message_cell_for_action(ahead, 5.5f, 5.5f, 0.0f, MessageReach::HereOnly).has_value());
    const std::vector<MessageCell> here = {{5, 5}};
    CHECK(message_cell_for_action(here, 5.5f, 5.5f, 0.0f, MessageReach::HereOnly) == MessageCell{5, 5});
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

namespace {
const std::vector<std::uint64_t> one_message = {42};
const std::vector<std::uint64_t> two_messages = {42, 43};

MessageBox open_box(std::span<const std::uint64_t> ids) {
    MessageBox box;
    box.open(ids, {5, 5});
    return box;
}
} // namespace

TEST_CASE("a box stays open within a cell of the message and closes past it") {
    MessageBox box = open_box(one_message);
    box.follow(6.5f, 6.5f);   // one cell away diagonally
    CHECK(box.current() == 42u);
    box.follow(7.5f, 5.5f);   // two cells away
    CHECK_FALSE(box.is_open());
    CHECK_FALSE(box.current().has_value());
}

TEST_CASE("the cursor starts on Leave, so pressing the button twice rates nothing") {
    MessageBox box = open_box(one_message);
    CHECK(box.choices(MessageRating::Possible) == std::vector{MessageChoice::Leave, MessageChoice::Good});
    CHECK(box.cursor(MessageRating::Possible) == 0);
    CHECK_FALSE(box.press(MessageKey::Confirm, MessageRating::Possible).has_value());
    CHECK_FALSE(box.is_open());
}

TEST_CASE("choosing Good rates the message and leaves it open on Leave") {
    MessageBox box = open_box(one_message);
    CHECK_FALSE(box.press(MessageKey::Following, MessageRating::Possible).has_value());
    CHECK(box.cursor(MessageRating::Possible) == 1);
    CHECK(box.press(MessageKey::Confirm, MessageRating::Possible) == 42u);
    CHECK(box.is_open());
    CHECK(box.cursor(MessageRating::Impossible) == 0);
    CHECK(box.choices(MessageRating::Impossible) == std::vector{MessageChoice::Leave});
}

TEST_CASE("a message that cannot be rated offers no Good, and Back closes without rating") {
    MessageBox box = open_box(one_message);
    CHECK(box.choices(MessageRating::Impossible) == std::vector{MessageChoice::Leave});
    (void)box.press(MessageKey::Following, MessageRating::Impossible);
    CHECK(box.cursor(MessageRating::Impossible) == 0);
    MessageBox other = open_box(one_message);
    (void)other.press(MessageKey::Following, MessageRating::Possible);
    CHECK_FALSE(other.press(MessageKey::Back, MessageRating::Possible).has_value());
    CHECK_FALSE(other.is_open());
}

TEST_CASE("the cursor wraps both ways") {
    MessageBox box = open_box(one_message);
    (void)box.press(MessageKey::Previous, MessageRating::Possible);
    CHECK(box.cursor(MessageRating::Possible) == 1);
    (void)box.press(MessageKey::Following, MessageRating::Possible);
    CHECK(box.cursor(MessageRating::Possible) == 0);
}

TEST_CASE("Next turns through the cell's messages and comes back round") {
    MessageBox box = open_box(two_messages);
    CHECK(box.page_count() == 2);
    const auto rows = box.choices(MessageRating::Impossible);
    CHECK(rows == std::vector{MessageChoice::Leave, MessageChoice::Next});
    (void)box.press(MessageKey::Following, MessageRating::Impossible);
    CHECK_FALSE(box.press(MessageKey::Confirm, MessageRating::Impossible).has_value());
    CHECK(box.current() == 43u);
    CHECK(box.page() == 1);
    CHECK(box.cursor(MessageRating::Impossible) == 0);
    (void)box.press(MessageKey::Following, MessageRating::Impossible);
    (void)box.press(MessageKey::Confirm, MessageRating::Impossible);
    CHECK(box.current() == 42u);
}

TEST_CASE("a cursor past a row that went away stays on the last row") {
    MessageBox box = open_box(two_messages);
    (void)box.press(MessageKey::Previous, MessageRating::Possible);   // on Next, the third row
    CHECK(box.cursor(MessageRating::Possible) == 2);
    CHECK(box.cursor(MessageRating::Impossible) == 1);
}

TEST_CASE("a closed box takes no key, and opening on no messages opens nothing") {
    MessageBox box;
    CHECK_FALSE(box.press(MessageKey::Confirm, MessageRating::Possible).has_value());
    box.open({}, {5, 5});
    CHECK_FALSE(box.is_open());
}

TEST_CASE("only counts that rose on messages already known are gained") {
    const std::vector<MessageCount> before = {{1, 2}, {2, 5}};
    const std::vector<MessageCount> after = {{1, 4}, {2, 3}, {3, 9}};
    CHECK(message_good_gained(before, after) == 2);
    CHECK(message_good_gained({}, after) == 0);
}
