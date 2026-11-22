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
