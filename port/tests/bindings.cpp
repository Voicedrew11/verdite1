#include <kf/platform/bindings.h>

#include <SDL3/SDL_keycode.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <tuple>
#include <vector>

using namespace kf;

namespace {
Control named(BindingGroup group, std::string_view name) {
    const auto control = control_from_name(group, name);
    CAPTURE(std::string(name));
    REQUIRE(control.has_value());
    return control.value_or(Control{});
}
Control key(std::string_view name) {
    return named(BindingGroup::keys, name);
}
Control pad(std::string_view name) {
    return named(BindingGroup::pad, name);
}
ActionControls list(BindingGroup group, std::string_view text) {
    const auto controls = controls_from_text(group, text);
    CAPTURE(std::string(text));
    REQUIRE(controls.has_value());
    return controls.value_or(ActionControls{});
}
std::size_t at(Action action) {
    return static_cast<std::size_t>(action);
}

using Bound = std::tuple<InputDevice, u32, Action>;
struct NamedBinding {
    BindingGroup group;
    std::string_view name;
    Action action;
};
// Today's bind_inputs by name: every key, mouse button, pad button, stick direction and trigger.
constexpr std::array<NamedBinding, 39> shared_snapshot{{
    {BindingGroup::keys, "W", Action::forward},
    {BindingGroup::keys, "Up", Action::forward},
    {BindingGroup::keys, "S", Action::backward},
    {BindingGroup::keys, "Down", Action::backward},
    {BindingGroup::keys, "A", Action::strafe_left},
    {BindingGroup::keys, "D", Action::strafe_right},
    {BindingGroup::keys, "Left", Action::turn_left},
    {BindingGroup::keys, "Right", Action::turn_right},
    {BindingGroup::keys, "PageUp", Action::look_up},
    {BindingGroup::keys, "PageDown", Action::look_down},
    {BindingGroup::keys, "F", Action::interact},
    {BindingGroup::keys, "Return", Action::confirm},
    {BindingGroup::keys, "Space", Action::attack},
    {BindingGroup::keys, "Q", Action::magic},
    {BindingGroup::keys, "Tab", Action::inventory},
    {BindingGroup::keys, "Backspace", Action::back},
    {BindingGroup::keys, "Escape", Action::pause_or_back},
    {BindingGroup::keys, "P", Action::pause},
    {BindingGroup::keys, "LeftShift", Action::run},
    {BindingGroup::keys, "RightShift", Action::run},
    {BindingGroup::keys, "F2", Action::quick_save},
    {BindingGroup::keys, "F4", Action::quick_load},
    {BindingGroup::keys, "R", Action::compare_language},
    {BindingGroup::keys, "ACBack", Action::pause_or_back},
    {BindingGroup::keys, "M", Action::full_map},
    {BindingGroup::keys, "MouseLeft", Action::attack},
    {BindingGroup::keys, "MouseRight", Action::magic},
    {BindingGroup::pad, "DpadUp", Action::forward},
    {BindingGroup::pad, "DpadDown", Action::backward},
    {BindingGroup::pad, "DpadLeft", Action::turn_left},
    {BindingGroup::pad, "DpadRight", Action::turn_right},
    {BindingGroup::pad, "Start", Action::inventory},
    {BindingGroup::pad, "Select", Action::pause},
    {BindingGroup::pad, "L1", Action::strafe_left},
    {BindingGroup::pad, "R1", Action::strafe_right},
    {BindingGroup::pad, "L3", Action::run},
    {BindingGroup::pad, "R3", Action::compare_language},
    {BindingGroup::pad, "L2", Action::look_up},
    {BindingGroup::pad, "R2", Action::look_down},
}};
constexpr std::array<NamedBinding, 4> stick_snapshot{{
    {BindingGroup::pad, "LeftStickLeft", Action::turn_left},
    {BindingGroup::pad, "LeftStickRight", Action::turn_right},
    {BindingGroup::pad, "LeftStickUp", Action::forward},
    {BindingGroup::pad, "LeftStickDown", Action::backward},
}};
constexpr std::array<NamedBinding, 4> west_snapshot{{
    {BindingGroup::pad, "Square", Action::attack},
    {BindingGroup::pad, "Triangle", Action::magic},
    {BindingGroup::pad, "Cross", Action::interact},
    {BindingGroup::pad, "Circle", Action::back},
}};
constexpr std::array<NamedBinding, 4> japanese_snapshot{{
    {BindingGroup::pad, "Triangle", Action::attack},
    {BindingGroup::pad, "Square", Action::magic},
    {BindingGroup::pad, "Circle", Action::interact},
    {BindingGroup::pad, "Cross", Action::back},
}};

std::vector<Bound> expected_bindings(std::span<const NamedBinding> faces) {
    std::vector<Bound> bound;
    using Table = std::span<const NamedBinding>;
    for (const auto table : {Table(shared_snapshot), Table(stick_snapshot), faces})
        for (const auto &entry : table) {
            const auto control = named(entry.group, entry.name);
            bound.emplace_back(control.device, control.code, entry.action);
        }
    std::sort(bound.begin(), bound.end());
    return bound;
}
std::vector<Bound> default_set(PadLayout layout) {
    std::vector<Bound> bound;
    for (const auto group : {BindingGroup::keys, BindingGroup::pad}) {
        const auto defaults = default_bindings(group, layout);
        for (std::size_t a = 0; a < action_count; ++a)
            for (const auto control : defaults[a].controls())
                bound.emplace_back(control.device, control.code, static_cast<Action>(a));
    }
    for (const auto &binding : fixed_bindings())
        bound.emplace_back(binding.control.device, binding.control.code, binding.action);
    std::sort(bound.begin(), bound.end());
    return bound;
}
} // namespace

TEST_CASE("the defaults and fixed controls are today's 47 bindings") {
    const auto west = default_set(PadLayout::West);
    CHECK(west.size() == 47);
    CHECK(west == expected_bindings(west_snapshot));
    const auto japanese = default_set(PadLayout::Japanese);
    CHECK(japanese.size() == 47);
    CHECK(japanese == expected_bindings(japanese_snapshot));
}

TEST_CASE("every default and fixed control's name reads back as that control") {
    std::vector<Control> controls;
    for (const auto layout : {PadLayout::West, PadLayout::Japanese})
        for (const auto group : {BindingGroup::keys, BindingGroup::pad})
            for (const auto &action : default_bindings(group, layout))
                for (const auto control : action.controls())
                    controls.push_back(control);
    for (const auto &binding : fixed_bindings())
        controls.push_back(binding.control);
    for (const auto control : controls) {
        const auto name = control_name(control);
        CAPTURE(name);
        CHECK_FALSE(name.empty());
        CHECK(control_from_name(binding_group(control), name) == control);
    }
}

TEST_CASE("names are read ignoring case and spaces, and written one way") {
    CHECK(control_name(key("space")) == "Space");
    CHECK(control_name(key("Left Shift")) == "LeftShift");
    CHECK(control_name(key("leftshift")) == "LeftShift");
    CHECK(control_name(key("q")) == "Q");
    CHECK(control_name(pad("dpad up")) == "DpadUp");
    CHECK(key("q").code == 'q');
    // 1073742424 is a scancode past SDL's table, so it has no other name.
    for (const auto name :
         {"Comma", "KeypadComma", "Keycode1", "Keycode1073742424", "1", "-", "MouseX2"})
        CHECK(control_name(key(name)) == name);
}

TEST_CASE("every key's written name reads back as that key") {
    std::vector<u32> codes{0x41, 0x5A, 0xE9};
    for (u32 scancode = 1; scancode < SDL_SCANCODE_COUNT; ++scancode)
        codes.push_back(SDL_SCANCODE_TO_KEYCODE(scancode));
    for (const auto code : codes) {
        const Control control{InputDevice::keyboard, code};
        const auto name = control_name(control);
        CAPTURE(name);
        CHECK(control_from_name(BindingGroup::keys, name) == control);
    }
    // SDL calls Return2 "Return", and an upper-case letter's character reads as the lower-case key.
    CHECK(control_name({InputDevice::keyboard, SDLK_RETURN2}) == "Keycode1073741982");
    CHECK(control_name({InputDevice::keyboard, 0x41}) == "Keycode65");
    CHECK(control_name({InputDevice::keyboard, SDLK_RETURN}) == "Return");
}

TEST_CASE("a name that is not a control of the group is refused") {
    for (const auto name : {"", " ", "Nope", "Keycode", "Keycode12x", "Keycode0",
                            "Keycode4294967296", "Keycode01234567890", "Keycode32", "Cross",
                            "Mouse6", "SpaceSpace"}) {
        CAPTURE(std::string(name));
        CHECK_FALSE(control_from_name(BindingGroup::keys, name).has_value());
    }
    CHECK_FALSE(control_from_name(BindingGroup::pad, "Space").has_value());
    CHECK_FALSE(control_from_name(BindingGroup::pad, "Guide").has_value());
    CHECK_FALSE(control_from_name(BindingGroup::keys, std::string(33, 'a')).has_value());
}

TEST_CASE("the fixed controls, the overlay's keys and another group's controls are not bindable") {
    CHECK_FALSE(control_bindable(BindingGroup::keys, key("F1")));
    CHECK_FALSE(control_bindable(BindingGroup::keys, key("F11")));
    CHECK_FALSE(control_bindable(BindingGroup::keys, key("Escape")));
    CHECK_FALSE(control_bindable(BindingGroup::keys, key("ACBack")));
    CHECK_FALSE(control_bindable(BindingGroup::pad, pad("LeftStickUp")));
    CHECK_FALSE(control_bindable(BindingGroup::keys, pad("Cross")));
    CHECK_FALSE(control_bindable(BindingGroup::pad, key("Space")));
    CHECK_FALSE(control_bindable(BindingGroup::keys, Control{InputDevice::mouse, 6}));
    CHECK(control_bindable(BindingGroup::keys, key("Space")));
    CHECK(control_bindable(BindingGroup::keys, key("MouseX1")));
    CHECK(control_bindable(BindingGroup::keys, key("F2")));
    CHECK(control_bindable(BindingGroup::pad, pad("R2")));
}

TEST_CASE("a list reads from and writes to its text") {
    CHECK(list(BindingGroup::keys, "none").count == 0);
    CHECK(list(BindingGroup::keys, "NONE").count == 0);
    CHECK(list(BindingGroup::keys, "Space,Space") == list(BindingGroup::keys, "Space"));
    const auto two = list(BindingGroup::keys, "space,Mouse Left");
    CHECK(controls_text(two.controls()) == "Space,MouseLeft");
    CHECK(controls_text({}) == "none");
    CHECK(list(BindingGroup::keys, "A,B,C,D").count == 4);
    for (const auto text : {"", ",", "Space,", ",Space", "Space,,Q", "A,B,C,D,E", "Escape", "F1",
                            "Cross", "nothing"}) {
        CAPTURE(std::string(text));
        CHECK_FALSE(controls_from_text(BindingGroup::keys, text).has_value());
    }
    std::string long_text = "Space";
    while (long_text.size() <= 300)
        long_text += ",Space";
    CHECK_FALSE(controls_from_text(BindingGroup::keys, long_text).has_value());
    CHECK_FALSE(controls_from_text(BindingGroup::pad, "LeftStickUp").has_value());
}

TEST_CASE("a letter binds by its US place, any other key as the layout makes it") {
    CHECK(bound_key(4, 0x12345) == 0x61);
    CHECK(bound_key(29, 0x12345) == 0x7A);
    CHECK(bound_key(30, 0x31) == 0x31);
    CHECK(bound_key(30, 0x26) == 0x26);
}

TEST_CASE("bindings_resolve lays the changes over the defaults") {
    const auto defaults = default_bindings(BindingGroup::keys, PadLayout::West);
    const auto keys = [](std::string_view text) { return list(BindingGroup::keys, text); };

    SUBCASE("no changes give the defaults") {
        CHECK(bindings_resolve(defaults, GroupChanges{}) == defaults);
    }
    SUBCASE("a changed list replaces its default") {
        GroupChanges changes{};
        changes[at(Action::attack)] = keys("K");
        const auto resolved = bindings_resolve(defaults, changes);
        CHECK(resolved[at(Action::attack)] == keys("K"));
        CHECK(resolved[at(Action::magic)] == defaults[at(Action::magic)]);
    }
    SUBCASE("a changed list takes from an unchanged action's defaults") {
        GroupChanges changes{};
        changes[at(Action::attack)] = keys("Q");
        const auto resolved = bindings_resolve(defaults, changes);
        CHECK(resolved[at(Action::attack)] == keys("Q"));
        CHECK(resolved[at(Action::magic)] == keys("MouseRight"));
    }
    SUBCASE("of two changed lists sharing a control, the first keeps it") {
        GroupChanges changes{};
        changes[at(Action::attack)] = keys("K");
        changes[at(Action::magic)] = keys("K,J");
        const auto resolved = bindings_resolve(defaults, changes);
        CHECK(resolved[at(Action::attack)] == keys("K"));
        CHECK(resolved[at(Action::magic)] == keys("J"));
    }
}

TEST_CASE("switching PAD BUTTONS moves the unchanged faces and keeps the changed lists") {
    BindingChanges changes{};
    changes.pad[at(Action::attack)] = list(BindingGroup::pad, "Square,R2");
    const auto west = bindings_resolve(changes, PadLayout::West);
    const auto japanese = bindings_resolve(changes, PadLayout::Japanese);
    for (const auto &resolved : {west, japanese}) {
        CHECK(resolved.pad[at(Action::attack)] == list(BindingGroup::pad, "Square,R2"));
        CHECK(resolved.pad[at(Action::look_down)].count == 0);
        CHECK(resolved.keys == default_bindings(BindingGroup::keys, PadLayout::West));
    }
    CHECK(west.pad[at(Action::magic)] == list(BindingGroup::pad, "Triangle"));
    CHECK(west.pad[at(Action::interact)] == list(BindingGroup::pad, "Cross"));
    CHECK(japanese.pad[at(Action::magic)].count == 0);
    CHECK(japanese.pad[at(Action::interact)] == list(BindingGroup::pad, "Circle"));
    CHECK(japanese.pad[at(Action::back)] == list(BindingGroup::pad, "Cross"));
}

TEST_CASE("bindings_set gives an action exactly its controls") {
    const auto defaults = default_bindings(BindingGroup::keys, PadLayout::West);
    const auto keys = [](std::string_view text) { return list(BindingGroup::keys, text); };

    SUBCASE("it takes from other changed lists") {
        auto changes = bindings_set(defaults, {}, Action::attack, keys("K,J"));
        changes = bindings_set(defaults, changes, Action::magic, keys("K"));
        CHECK(changes[at(Action::attack)] == keys("J"));
        CHECK(changes[at(Action::magic)] == keys("K"));
    }
    SUBCASE("a list set back to its default stops being a change") {
        auto changes = bindings_set(defaults, {}, Action::attack, keys("K"));
        changes = bindings_set(defaults, changes, Action::attack, keys("Space,MouseLeft"));
        CHECK_FALSE(changes[at(Action::attack)].has_value());
    }
    SUBCASE("resetting the taker gives the control back to its unchanged owner") {
        auto changes = bindings_set(defaults, {}, Action::attack, keys("Q"));
        CHECK(bindings_resolve(defaults, changes)[at(Action::magic)] == keys("MouseRight"));
        changes = bindings_set(defaults, changes, Action::attack, defaults[at(Action::attack)]);
        CHECK(changes == GroupChanges{});
        CHECK(bindings_resolve(defaults, changes)[at(Action::magic)] == keys("Q,MouseRight"));
    }
    SUBCASE("an empty list is kept as a change") {
        const auto changes = bindings_set(defaults, {}, Action::attack, keys("none"));
        REQUIRE(changes[at(Action::attack)].has_value());
        CHECK(changes[at(Action::attack)].value_or(keys("Space")).count == 0);
        CHECK(bindings_resolve(defaults, changes)[at(Action::attack)].count == 0);
    }
}

TEST_CASE("bindings_tidy keeps the first holder and drops lists equal to their defaults") {
    const auto defaults = default_bindings(BindingGroup::keys, PadLayout::West);
    const auto keys = [](std::string_view text) { return list(BindingGroup::keys, text); };
    GroupChanges changes{};
    changes[at(Action::attack)] = keys("K,Q");
    changes[at(Action::magic)] = keys("Q,J");
    changes[at(Action::confirm)] = keys("Return");
    changes[at(Action::back)] = keys("K");
    const auto tidy = bindings_tidy(defaults, changes);
    CHECK(tidy[at(Action::attack)] == keys("K,Q"));
    CHECK(tidy[at(Action::magic)] == keys("J"));
    CHECK_FALSE(tidy[at(Action::confirm)].has_value());
    REQUIRE(tidy[at(Action::back)].has_value());
    CHECK(tidy[at(Action::back)].value_or(keys("Backspace")).count == 0);
    CHECK(bindings_resolve(defaults, tidy) == bindings_resolve(defaults, changes));
    CHECK(bindings_tidy(defaults, tidy) == tidy);
}

TEST_CASE("a pad change equal to a layout's face default survives switching layouts") {
    const auto west = default_bindings(BindingGroup::pad, PadLayout::West);
    const auto japanese = default_bindings(BindingGroup::pad, PadLayout::Japanese);
    const auto pad_list = [](std::string_view text) { return list(BindingGroup::pad, text); };
    // On West, Attack takes Triangle from Magic.
    auto changes = bindings_set(west, {}, Action::attack, pad_list("Triangle"));
    // On Japanese, Triangle is Attack's default: neither a reload nor another edit drops it.
    changes = bindings_tidy(japanese, changes);
    changes = bindings_set(japanese, changes, Action::quick_save, pad_list("R3"));
    CHECK(changes[at(Action::attack)] == pad_list("Triangle"));
    const auto back_on_west = bindings_resolve(west, changes);
    CHECK(back_on_west[at(Action::attack)] == pad_list("Triangle"));
    CHECK(back_on_west[at(Action::magic)].count == 0);
    // A list equal to a default no layout moves is still no change.
    changes = bindings_set(west, changes, Action::run, west[at(Action::run)]);
    CHECK_FALSE(changes[at(Action::run)].has_value());
}

TEST_CASE("bindings_missing_way finds a way through the menus with no usable control") {
    for (const auto layout : {PadLayout::West, PadLayout::Japanese})
        for (const auto group : {BindingGroup::keys, BindingGroup::pad})
            CHECK_FALSE(bindings_missing_way(default_bindings(group, layout), group).has_value());
    auto keys = default_bindings(BindingGroup::keys, PadLayout::West);
    keys[at(Action::confirm)] = list(BindingGroup::keys, "R");
    keys[at(Action::interact)] = list(BindingGroup::keys, "F");
    CHECK(bindings_missing_way(keys, BindingGroup::keys) == BindingWay::confirm);
    keys[at(Action::interact)] = list(BindingGroup::keys, "F,MouseMiddle");
    CHECK(bindings_missing_way(keys, BindingGroup::keys) == BindingWay::confirm);
    keys[at(Action::interact)] = list(BindingGroup::keys, "F,E,Up");
    CHECK_FALSE(bindings_missing_way(keys, BindingGroup::keys).has_value());
    // Back opens the menu in play too.
    keys[at(Action::inventory)] = list(BindingGroup::keys, "none");
    CHECK_FALSE(bindings_missing_way(keys, BindingGroup::keys).has_value());
    keys[at(Action::back)] = list(BindingGroup::keys, "MouseX1");
    CHECK(bindings_missing_way(keys, BindingGroup::keys) == BindingWay::menu);

    auto pad_bindings = default_bindings(BindingGroup::pad, PadLayout::West);
    pad_bindings[at(Action::inventory)] = list(BindingGroup::pad, "none");
    CHECK_FALSE(bindings_missing_way(pad_bindings, BindingGroup::pad).has_value());
    pad_bindings[at(Action::back)] = list(BindingGroup::pad, "none");
    pad_bindings[at(Action::pause_or_back)] = list(BindingGroup::pad, "Select");
    CHECK(bindings_missing_way(pad_bindings, BindingGroup::pad) == BindingWay::menu);
}
