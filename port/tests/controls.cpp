#include <kf/platform/controls.h>

#include <doctest/doctest.h>

#include <string>

using namespace kf;

TEST_CASE("every action's name reads back as that action") {
    for (std::size_t i = 0; i < static_cast<std::size_t>(Action::count); ++i) {
        const auto action = static_cast<Action>(i);
        const auto name = action_name(action);
        CAPTURE(std::string(name));
        CHECK_FALSE(name.empty());
        CHECK(action_from_name(name) == action);
    }
    CHECK(action_name(Action::attack) == "attack");
    CHECK(action_name(Action::full_map) == "full_map");
    CHECK(action_name(Action::count).empty());
    CHECK_FALSE(action_from_name("nope").has_value());
    CHECK_FALSE(action_from_name("Attack").has_value());
    CHECK_FALSE(action_from_name("").has_value());
}

namespace {
constexpr Control key_w{InputDevice::keyboard, 'w'};
constexpr Control key_i{InputDevice::keyboard, 'i'};
constexpr Control mouse_left{InputDevice::mouse, 1};

// W walks forward and the left mouse button attacks, with W held and the frame's edges taken.
InputState walking() {
    InputState input{};
    const std::array<InputBinding, 2> start{{{key_w, Action::forward, false}, {mouse_left, Action::attack, false}}};
    REQUIRE(input_set_bindings(&input, start));
    input_button(&input, key_w, true);
    const auto frame = input_take(&input);
    REQUIRE(input_pressed(&frame, Action::forward));
    return input;
}
} // namespace

TEST_CASE("input_set_bindings replaces the table") {
    auto input = walking();
    const std::array<InputBinding, 1> next{{{key_i, Action::magic, false}}};
    REQUIRE(input_set_bindings(&input, next));
    REQUIRE(input.binding_count == 1);
    CHECK(input.bindings[0].control == key_i);
    CHECK(input.bindings[0].action == Action::magic);
    input_take(&input);
    input_button(&input, key_i, true);
    auto frame = input_take(&input);
    CHECK(input_pressed(&frame, Action::magic));
    input_button(&input, mouse_left, true);
    frame = input_take(&input);
    CHECK_FALSE(input_pressed(&frame, Action::attack));
}

TEST_CASE("the same bindings again keep held controls held, with no edges") {
    auto input = walking();
    const std::array<InputBinding, 2> same{{{key_w, Action::forward, false}, {mouse_left, Action::attack, false}}};
    REQUIRE(input_set_bindings(&input, same));
    const auto frame = input_take(&input);
    CHECK((frame.held & action_bit(Action::forward)) != 0);
    CHECK(frame.pressed == 0);
    CHECK(frame.released == 0);
}

TEST_CASE("a held control moved to another action releases the old and does not press the new") {
    auto input = walking();
    const std::array<InputBinding, 2> moved{{{key_w, Action::backward, false}, {mouse_left, Action::attack, false}}};
    REQUIRE(input_set_bindings(&input, moved));
    auto frame = input_take(&input);
    CHECK((frame.released & action_bit(Action::forward)) != 0);
    CHECK(frame.held == 0);
    CHECK(frame.pressed == 0);
    // Letting go is a no-op; the next press acts.
    input_button(&input, key_w, false);
    frame = input_take(&input);
    CHECK(frame.released == 0);
    input_button(&input, key_w, true);
    frame = input_take(&input);
    CHECK(input_pressed(&frame, Action::backward));
}

TEST_CASE("a held control removed releases its action") {
    auto input = walking();
    const std::array<InputBinding, 1> removed{{{mouse_left, Action::attack, false}}};
    REQUIRE(input_set_bindings(&input, removed));
    const auto frame = input_take(&input);
    CHECK((frame.released & action_bit(Action::forward)) != 0);
    CHECK(frame.held == 0);
}

TEST_CASE("input_set_bindings refuses a bad table and changes nothing") {
    auto input = walking();
    const auto before_count = input.binding_count;
    const auto check_unchanged = [&] {
        CHECK(input.binding_count == before_count);
        CHECK(input.bindings[0].control == key_w);
        CHECK(input.bindings[0].down);
        const auto frame = input_take(&input);
        CHECK(frame.held == action_bit(Action::forward));
        CHECK(frame.pressed == 0);
        CHECK(frame.released == 0);
    };

    SUBCASE("more than the capacity") {
        std::array<InputBinding, input_binding_capacity + 1> many{};
        for (std::size_t i = 0; i < many.size(); ++i)
            many[i] = {{InputDevice::keyboard, static_cast<u32>(1000 + i)}, Action::forward, false};
        CHECK_FALSE(input_set_bindings(&input, many));
        check_unchanged();
    }
    SUBCASE("an action past count") {
        const std::array<InputBinding, 1> bad{{{key_i, Action::count, false}}};
        CHECK_FALSE(input_set_bindings(&input, bad));
        check_unchanged();
    }
    SUBCASE("a control listed twice") {
        const std::array<InputBinding, 2> twice{{{key_i, Action::magic, false}, {key_i, Action::attack, false}}};
        CHECK_FALSE(input_set_bindings(&input, twice));
        check_unchanged();
    }
}

TEST_CASE("a full table fits") {
    InputState input{};
    std::array<InputBinding, input_binding_capacity> full{};
    for (std::size_t i = 0; i < full.size(); ++i)
        full[i] = {{InputDevice::keyboard, static_cast<u32>(1000 + i)}, Action::forward, false};
    CHECK(input_set_bindings(&input, full));
    CHECK(input.binding_count == input_binding_capacity);
}
