#include <verdite/options.h>

#include <doctest/doctest.h>

#include <cmath>
#include <utility>

using namespace verdite;

namespace {

constexpr auto window = static_cast<PlatformFeatures>(PlatformFeature::Window);
constexpr auto touch = static_cast<PlatformFeatures>(PlatformFeature::Touch);

// A settings struct as a game keeps one, and its table.
struct Game {
    bool fullscreen = false;
    bool lights = true;
    double strength = 100;
    std::string aspect = "4:3";
    int resets = 0;
};

OptionRow row(std::string id, OptionKind kind) {
    OptionRow made;
    made.id = std::move(id);
    made.kind = kind;
    made.label = made.id;
    return made;
}

OptionState state(bool shown, bool enabled, std::string note = {}) {
    OptionState made;
    made.shown = shown;
    made.enabled = enabled;
    made.note = std::move(note);
    return made;
}

OptionTable table_for(Game &game) {
    OptionRow fullscreen = row("display.fullscreen", OptionKind::Toggle);
    fullscreen.needs = window;
    fullscreen.get = [&] { return OptionValue(game.fullscreen); };
    fullscreen.set = [&](const OptionValue &v) { game.fullscreen = std::get<bool>(v); };
    OptionRow aspect = row("display.aspect", OptionKind::Choice);
    aspect.choices = {{std::string("4:3"), "4:3"}, {std::string("16:9"), "16:9"}};
    aspect.get = [&] { return OptionValue(game.aspect); };
    aspect.set = [&](const OptionValue &v) { game.aspect = std::get<std::string>(v); };
    OptionRow lights = row("display.lights", OptionKind::Toggle);
    lights.get = [&] { return OptionValue(game.lights); };
    lights.set = [&](const OptionValue &v) { game.lights = std::get<bool>(v); };
    OptionRow strength = row("display.strength", OptionKind::Number);
    strength.min = 0;
    strength.max = 200;
    strength.step = 5;
    strength.get = [&] { return OptionValue(game.strength); };
    strength.set = [&](const OptionValue &v) { game.strength = std::get<double>(v); };
    strength.state = [&] { return state(game.lights, true); };
    OptionRow reset = row("display.reset", OptionKind::Action);
    reset.run = [&] { ++game.resets; };
    reset.state = [&] { return state(true, !game.fullscreen, "Not in fullscreen"); };
    OptionRow pad = row("touch.size", OptionKind::Number);
    pad.needs = touch;
    pad.get = [] { return OptionValue(1.0); };
    pad.set = [](const OptionValue &) {};
    return OptionTable({
        OptionPage{"Display", {OptionSection{"", {fullscreen, aspect}}, OptionSection{"Lighting", {lights, strength, reset}}}},
        OptionPage{"Touch", {OptionSection{"", {pad}}}},
    });
}

} // namespace

TEST_CASE("a view leaves out what the platform lacks, then empty pages") {
    Game game;
    const OptionTable table = table_for(game);
    const auto phone = table.view(touch);
    REQUIRE(phone.size() == 2);
    CHECK(phone[0].sections[0].rows.size() == 1);   // no fullscreen without a window
    CHECK(phone[0].sections[0].rows[0].row->id == "display.aspect");
    const auto desktop = table.view(window);
    REQUIRE(desktop.size() == 1);   // the touch page is empty there
    CHECK(desktop[0].title == "Display");
    CHECK(desktop[0].sections[0].rows.size() == 2);
}

TEST_CASE("a view carries each row's value and state now") {
    Game game;
    const OptionTable table = table_for(game);
    auto rows = table.view(window)[0].sections[1].rows;
    REQUIRE(rows.size() == 3);
    CHECK(std::get<bool>(rows[0].value));
    CHECK(std::get<double>(rows[1].value) == 100);
    CHECK(rows[2].state.enabled);
    game.lights = false;
    game.fullscreen = true;
    rows = table.view(window)[0].sections[1].rows;
    REQUIRE(rows.size() == 2);   // strength hidden with the lights off
    CHECK(rows[1].row->id == "display.reset");
    CHECK_FALSE(rows[1].state.enabled);
    CHECK(rows[1].state.note == "Not in fullscreen");
}

TEST_CASE("a number is clamped and snapped to its step") {
    Game game;
    OptionTable table = table_for(game);
    CHECK(table.set("display.strength", 42.0));
    CHECK(game.strength == 40);
    CHECK(table.set("display.strength", 999.0));
    CHECK(game.strength == 200);
    CHECK_FALSE(table.set("display.strength", std::nan("")));
    CHECK(game.strength == 200);
}

TEST_CASE("a choice takes only one of its values") {
    Game game;
    OptionTable table = table_for(game);
    CHECK(table.set("display.aspect", std::string("16:9")));
    CHECK(game.aspect == "16:9");
    CHECK_FALSE(table.set("display.aspect", std::string("21:9")));
    CHECK(game.aspect == "16:9");
}

TEST_CASE("set refuses a wrong type, an unknown id and an action") {
    Game game;
    OptionTable table = table_for(game);
    CHECK_FALSE(table.set("display.fullscreen", 1.0));
    CHECK_FALSE(table.set("display.nothing", true));
    CHECK_FALSE(table.set("display.reset", true));
    CHECK_FALSE(game.fullscreen);
}

TEST_CASE("a disabled row is not changed or run") {
    Game game;
    OptionTable table = table_for(game);
    CHECK(table.run("display.reset"));
    CHECK(game.resets == 1);
    game.fullscreen = true;
    CHECK_FALSE(table.run("display.reset"));
    CHECK(game.resets == 1);
    CHECK_FALSE(table.run("display.aspect"));
}
