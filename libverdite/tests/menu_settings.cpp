#include <verdite/menu_settings.h>

#include <doctest/doctest.h>

#include <string>
#include <utility>

using namespace verdite;

namespace {

constexpr auto window = static_cast<PlatformFeatures>(PlatformFeature::Window);

struct Game {
    bool fullscreen = false;
    bool lights = true;
    double strength = 100;
    double fps = 60;
    std::string aspect = "4:3";
};

OptionRow row(std::string id, OptionKind kind) {
    OptionRow made;
    made.id = std::move(id);
    made.kind = kind;
    return made;
}

OptionTable table_for(Game &game) {
    OptionRow fullscreen = row("display.fullscreen", OptionKind::Toggle);
    fullscreen.needs = window;
    fullscreen.get = [&] { return OptionValue(game.fullscreen); };
    fullscreen.set = [&](const OptionValue &v) { game.fullscreen = std::get<bool>(v); };
    OptionRow aspect = row("display.aspect", OptionKind::Choice);
    aspect.choices = {{std::string("4:3"), "4:3"}, {std::string("16:9"), "16:9"}, {std::string("21:9"), "21:9"}};
    aspect.get = [&] { return OptionValue(game.aspect); };
    aspect.set = [&](const OptionValue &v) { game.aspect = std::get<std::string>(v); };
    OptionRow lights = row("display.lights", OptionKind::Toggle);
    lights.get = [&] { return OptionValue(game.lights); };
    lights.set = [&](const OptionValue &v) { game.lights = std::get<bool>(v); };
    OptionRow strength = row("display.strength", OptionKind::Number);
    strength.min = 0;
    strength.max = 200;
    strength.step = 50;
    strength.get = [&] { return OptionValue(game.strength); };
    strength.set = [&](const OptionValue &v) { game.strength = std::get<double>(v); };
    strength.state = [&] {
        OptionState state;
        state.enabled = game.lights;
        return state;
    };
    OptionRow fps = row("display.fps", OptionKind::Number);
    fps.min = 30;
    fps.max = 360;
    fps.get = [&] { return OptionValue(game.fps); };
    fps.set = [&](const OptionValue &v) { game.fps = std::get<double>(v); };
    OptionRow reset = row("display.reset", OptionKind::Action);
    reset.run = [] {};
    return OptionTable({OptionPage{"Display", {OptionSection{"", {fullscreen, aspect, lights, strength, fps, reset}}}}});
}

std::vector<MenuSettingsPage> pages() {
    return {
        MenuSettingsPage{"display", {{"display.fullscreen", {}}, {"display.aspect", {}}, {"display.fps", {30.0, 60.0, 144.0}}}},
        MenuSettingsPage{"lights", {{"display.lights", {}}, {"display.strength", {}}}},
        MenuSettingsPage{"nothing", {{"display.reset", {}}, {"no.such.option", {}}}},
    };
}

} // namespace

TEST_CASE("rows the platform lacks, rows without stops and then empty pages are left out") {
    Game game;
    OptionTable table = table_for(game);
    MenuSettings phone(table, pages(), 0);
    phone.open();
    const auto view = phone.view();
    CHECK(view.page == "display");
    CHECK(view.page_count == 2);
    REQUIRE(view.rows.size() == 2);   // no fullscreen without a window
    CHECK(view.rows[0].option == "display.aspect");
    CHECK(view.rows[0].selected);
    CHECK(std::get<std::string>(view.rows[0].value) == "4:3");
}

TEST_CASE("rows and pages wrap, and a new page starts on its first row") {
    Game game;
    OptionTable table = table_for(game);
    MenuSettings menu(table, pages(), window);
    menu.open();
    menu.move_row(-1);
    CHECK(menu.view().rows[2].selected);
    menu.move_page(-1);
    CHECK(menu.view().page == "lights");
    CHECK(menu.view().rows[0].selected);
    menu.select_row(1);
    CHECK(menu.view().rows[1].selected);
    menu.select_row(7);   // off the page: ignored
    CHECK(menu.view().rows[1].selected);
    menu.move_page(1);
    CHECK(menu.view().page == "display");
}

TEST_CASE("a step goes round the ends of a choice and a toggle") {
    Game game;
    OptionTable table = table_for(game);
    MenuSettings menu(table, pages(), window);
    menu.open();
    CHECK(menu.step(-1));   // off goes round to on
    CHECK(game.fullscreen);
    CHECK(menu.step(1));
    CHECK_FALSE(game.fullscreen);
    menu.move_row(1);
    CHECK(menu.step(-1));
    CHECK(game.aspect == "21:9");
    CHECK(menu.step(2));
    CHECK(game.aspect == "16:9");
}

TEST_CASE("a number set between stops goes to the nearer stop on the side stepped") {
    Game game;
    game.fps = 137;
    OptionTable table = table_for(game);
    MenuSettings menu(table, pages(), window);
    menu.open();
    menu.select_row(2);
    CHECK(menu.step(1));
    CHECK(game.fps == 144);
    game.fps = 137;
    CHECK(menu.step(-1));
    CHECK(game.fps == 60);
    game.fps = 300;
    CHECK(menu.step(1));   // nothing above: round to the first
    CHECK(game.fps == 30);
}

TEST_CASE("a number without given stops walks its own step") {
    Game game;
    OptionTable table = table_for(game);
    MenuSettings menu(table, pages(), window);
    menu.open();
    menu.move_page(1);
    menu.select_row(1);
    CHECK(menu.step(1));
    CHECK(game.strength == 150);
    CHECK(menu.step(2));
    CHECK(game.strength == 0);   // 200, then round to 0
}

TEST_CASE("a disabled row is dimmed and refuses a step") {
    Game game;
    game.lights = false;
    OptionTable table = table_for(game);
    MenuSettings menu(table, pages(), window);
    menu.open();
    menu.move_page(1);
    menu.select_row(1);
    CHECK_FALSE(menu.view().rows[1].enabled);
    CHECK_FALSE(menu.step(1));
    CHECK(game.strength == 100);
    CHECK_FALSE(menu.changed());
}

TEST_CASE("changed is false once every value is back where the visit found it") {
    Game game;
    OptionTable table = table_for(game);
    MenuSettings menu(table, pages(), window);
    menu.open();
    CHECK_FALSE(menu.changed());
    menu.select_row(1);
    menu.step(1);
    CHECK(menu.changed());
    menu.step(-1);
    CHECK_FALSE(menu.changed());
}

TEST_CASE("put_back restores every option, the one that disabled another first") {
    Game game;
    OptionTable table = table_for(game);
    MenuSettings menu(table, pages(), window);
    menu.open();
    menu.select_row(1);
    menu.step(1);
    menu.move_page(1);
    menu.select_row(1);
    menu.step(1);            // strength 150
    menu.select_row(0);
    menu.step(1);            // lights off: strength now disabled
    CHECK(menu.changed());
    menu.put_back();
    CHECK(game.aspect == "4:3");
    CHECK(game.lights);
    CHECK(game.strength == 100);
    CHECK_FALSE(menu.changed());
}

TEST_CASE("keep ends the visit with the changes, and open starts a new one") {
    Game game;
    OptionTable table = table_for(game);
    MenuSettings menu(table, pages(), window);
    menu.open();
    menu.move_page(1);
    menu.step(1);
    menu.keep();
    CHECK_FALSE(menu.changed());
    CHECK_FALSE(game.lights);
    menu.open();
    CHECK(menu.view().page == "lights");   // the page last shown
    menu.put_back();                       // nothing staged in this visit
    CHECK_FALSE(game.lights);
}
