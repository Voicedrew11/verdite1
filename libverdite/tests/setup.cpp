#include <verdite/setup.h>

#include <doctest/doctest.h>

#include <string>
#include <utility>
#include <vector>

using namespace verdite;

namespace {

constexpr auto desktop = static_cast<PlatformFeatures>(PlatformFeature::Window) |
                         static_cast<PlatformFeatures>(PlatformFeature::Mouse);
constexpr auto phone = static_cast<PlatformFeatures>(PlatformFeature::Touch);

// A game's settings and their table.
struct Game {
    bool fullscreen = false;
    std::string aspect = "4:3";
    bool smooth = false;
    bool lights = false;
    double strength = 100;   // enabled only with the lights on
    std::string turn = "fixed";
    double turn_speed = 1;
};

OptionRow row(std::string id, OptionKind kind, std::string label) {
    OptionRow made;
    made.id = std::move(id);
    made.kind = kind;
    made.label = std::move(label);
    return made;
}

template <typename T>
void bind(OptionRow &made, T &field) {
    made.get = [&field] { return OptionValue(field); };
    made.set = [&field](const OptionValue &v) { field = std::get<T>(v); };
}

OptionTable table_for(Game &game) {
    OptionRow fullscreen = row("display.fullscreen", OptionKind::Toggle, "Fullscreen");
    fullscreen.needs = static_cast<PlatformFeatures>(PlatformFeature::Window);
    bind(fullscreen, game.fullscreen);
    OptionRow aspect = row("display.aspect", OptionKind::Choice, "Aspect");
    aspect.choices = {{std::string("4:3"), "4:3"}, {std::string("16:9"), "Wide"}};
    bind(aspect, game.aspect);
    OptionRow smooth = row("display.smooth", OptionKind::Toggle, "Smoothing");
    bind(smooth, game.smooth);
    OptionRow lights = row("display.lights", OptionKind::Toggle, "Lights");
    bind(lights, game.lights);
    OptionRow strength = row("display.strength", OptionKind::Number, "Strength");
    strength.min = 0;
    strength.max = 200;
    strength.step = 10;
    strength.format = "%.0f%%";
    bind(strength, game.strength);
    strength.state = [&game] {
        OptionState s;
        s.enabled = game.lights;
        return s;
    };
    OptionRow turn = row("enemy.turn", OptionKind::Choice, "Enemy turn");
    turn.choices = {{std::string("fixed"), "Fixed"}, {std::string("auto"), "Automatic"}};
    bind(turn, game.turn);
    OptionRow turn_speed = row("enemy.turn_speed", OptionKind::Number, "Enemy turn speed");
    turn_speed.min = 1;
    turn_speed.max = 4;
    turn_speed.step = 0.25;
    bind(turn_speed, game.turn_speed);
    return OptionTable({OptionPage{"All", {OptionSection{"", {fullscreen, aspect, smooth, lights, strength, turn, turn_speed}}}}});
}

SetupChoice choice(std::string label, std::vector<std::string> tiers, std::vector<SetupValue> values) {
    SetupChoice made;
    made.label = std::move(label);
    made.tiers = std::move(tiers);
    made.values = std::move(values);
    return made;
}

SetupSpec spec() {
    SetupSpec s;
    s.version = 2;
    s.tiers = {{"original", "Original", ""}, {"balanced", "Balanced", ""}, {"modern", "Modern", ""}};
    s.presets = {{"original", "Original", "", "original", {}},
                 {"modern", "Modern", "", "modern", {{"display.aspect", std::string("16:9")}}},
                 {"custom", "Custom", "", "", {}}};
    SetupPage display{"display", "Display", "", {SetupOptionItem{"display.fullscreen", {}}, SetupOptionItem{"display.aspect", {}}}, {}};
    SetupGroup picture{"picture", "Picture", "", {
        choice("Rough", {"original"}, {{"display.smooth", false}}),
        choice("Smooth", {"balanced", "modern"}, {{"display.smooth", true}}),
    }};
    SetupGroup lighting{"lighting", "Lighting", "", {
        choice("Plain", {"original", "balanced"}, {{"display.lights", false}}),
        choice("Lit", {"modern"}, {{"display.lights", true}, {"display.strength", 150.0}}),
    }};
    SetupPage looks{"looks", "Looks", "", {picture, lighting}, SetupPageTiers::Offered};
    SetupGroup turning{"turning", "Enemy turn", "", {
        choice("Fixed", {"original", "balanced"}, {{"enemy.turn", std::string("fixed")}}),
        choice("Automatic", {"modern"}, {{"enemy.turn", std::string("auto")}}),
    }};
    SetupPage enemies{"enemies", "Enemies", "", {turning, SetupOptionItem{"enemy.turn_speed", SetupValue{"enemy.turn", std::string("fixed")}}}, {}};
    SetupPage touch_only{"touch", "Touch", "", {SetupOptionItem{"touch.size", {}}}, {}};
    s.pages = {display, looks, touch_only, enemies};
    return s;
}

struct Screen final : SetupScreen {
    std::vector<SetupPageView> shown;
    int closes = 0;
    void show(const SetupPageView &view) override { shown.push_back(view); }
    void close() override { ++closes; }
};

struct Run {
    Game game;
    OptionTable table = table_for(game);
    SetupSpec setup_spec = spec();
    std::vector<int> seen;
    Screen screen;
    Setup setup;
    explicit Run(PlatformFeatures has = desktop) : setup(setup_spec, table, has, [this](int v) { seen.push_back(v); }) {
        setup.start(screen);
    }
};

} // namespace

TEST_CASE("setup is due until its version is seen") {
    const SetupSpec s = spec();
    CHECK(setup_due(s, 0));
    CHECK(setup_due(s, 1));
    CHECK_FALSE(setup_due(s, 2));
}

TEST_CASE("start shows the welcome with every preset") {
    Run run;
    REQUIRE(run.screen.shown.size() == 1);
    const SetupPageView &view = run.screen.shown.back();
    CHECK(view.step == SetupStep::Welcome);
    REQUIRE(view.presets.size() == 3);
    CHECK(view.presets[2]->id == "custom");
    CHECK(view.page_count == 3);   // the touch page has nothing on a desktop
}

TEST_CASE("a preset stages its tier and its values, and Finish sets them") {
    Run run;
    CHECK(run.setup.choose_preset("modern"));
    CHECK(run.setup.view().step == SetupStep::Page);
    CHECK(run.game.aspect == "4:3");   // nothing set until Finish
    while (run.setup.view().step == SetupStep::Page)
        CHECK(run.setup.next());
    const SetupPageView summary = run.setup.view();
    CHECK(summary.step == SetupStep::Summary);
    CHECK(summary.page == 3);
    REQUIRE(summary.changes.size() == 5);
    CHECK(summary.changes[0].option_label == "Smoothing");
    CHECK(summary.changes[0].from == "Off");
    CHECK(summary.changes[0].to == "On");
    CHECK(summary.changes[2].option_label == "Strength");
    CHECK(summary.changes[2].to == "150%");
    CHECK(summary.changes[4].to == "Wide");
    CHECK(run.setup.finish());
    // Strength is enabled only once the lights are on: a second pass sets it.
    CHECK(run.game.smooth);
    CHECK(run.game.lights);
    CHECK(run.game.strength == 150);
    CHECK(run.game.turn == "auto");
    CHECK(run.game.aspect == "16:9");
    CHECK(run.seen == std::vector<int>{2});
    CHECK(run.screen.closes == 1);
    CHECK_FALSE(run.setup.running());
}

TEST_CASE("a page shows each group's choice from what is staged, and the tier they share") {
    Run run;
    CHECK(run.setup.choose_preset("original"));
    CHECK(run.setup.next());
    SetupPageView looks = run.setup.view();
    CHECK(looks.spec_page->id == "looks");
    REQUIRE(looks.items.size() == 2);
    CHECK(std::get<SetupGroupView>(looks.items[0]).chosen == 0u);
    REQUIRE(looks.tiers.size() == 3);
    CHECK(looks.page_tier == 0u);
    CHECK(run.setup.choose("picture", 1));
    looks = run.setup.view();
    CHECK(std::get<SetupGroupView>(looks.items[0]).chosen == 1u);
    CHECK(looks.page_tier == 1u);   // smooth and plain: balanced
    CHECK(run.setup.choose_page_tier("modern"));
    looks = run.setup.view();
    CHECK(std::get<SetupGroupView>(looks.items[1]).chosen == 1u);
    CHECK(looks.page_tier == 2u);
    CHECK_FALSE(run.setup.choose("picture", 2));
    CHECK_FALSE(run.setup.choose("turning", 0));   // not on this page
}

TEST_CASE("custom starts from the settings, and a group matching no choice has none") {
    Run run;
    run.game.smooth = true;
    run.game.lights = true;
    run.game.strength = 50;
    CHECK(run.setup.choose_preset("custom"));
    CHECK(run.setup.next());
    const SetupPageView looks = run.setup.view();
    CHECK(std::get<SetupGroupView>(looks.items[0]).chosen == 1u);
    CHECK_FALSE(std::get<SetupGroupView>(looks.items[1]).chosen);
    CHECK_FALSE(looks.page_tier);
}

TEST_CASE("an option shown while a value is staged follows it") {
    Run run;
    CHECK(run.setup.choose_preset("original"));
    CHECK(run.setup.next());
    CHECK(run.setup.next());
    SetupPageView enemies = run.setup.view();
    CHECK(enemies.spec_page->id == "enemies");
    REQUIRE(enemies.items.size() == 2);
    CHECK(run.setup.set_option("enemy.turn_speed", 2.1));
    CHECK(std::get<double>(std::get<OptionRowView>(run.setup.view().items[1]).value) == 2.0);
    CHECK(run.setup.choose("turning", 1));
    enemies = run.setup.view();
    CHECK(enemies.items.size() == 1);
    CHECK_FALSE(run.setup.set_option("enemy.turn_speed", 3.0));
}

TEST_CASE("back walks to the welcome; skip sets nothing but marks seen") {
    Run run;
    CHECK_FALSE(run.setup.back());
    CHECK(run.setup.choose_preset("modern"));
    CHECK(run.setup.back());
    CHECK(run.setup.view().step == SetupStep::Welcome);
    CHECK_FALSE(run.setup.next());
    CHECK_FALSE(run.setup.finish());
    CHECK(run.setup.skip());
    CHECK(run.game.aspect == "4:3");
    CHECK(run.seen == std::vector<int>{2});
    CHECK(run.screen.closes == 1);
    CHECK_FALSE(run.setup.skip());
}

TEST_CASE("the summary's back returns to the last page") {
    Run run;
    CHECK(run.setup.choose_preset("original"));
    for (int i = 0; i < 3; ++i)
        CHECK(run.setup.next());
    CHECK(run.setup.view().step == SetupStep::Summary);
    CHECK(run.setup.back());
    CHECK(run.setup.view().spec_page->id == "enemies");
}

TEST_CASE("a phone never sees the rows it cannot use, nor stages their values") {
    Run run(phone);
    CHECK(run.setup.view().page_count == 3);   // the touch page's option is not in the table: still nothing
    CHECK(run.setup.choose_preset("modern"));
    const SetupPageView display = run.setup.view();
    REQUIRE(display.items.size() == 1);
    CHECK(std::get<OptionRowView>(display.items[0]).row->id == "display.aspect");
    CHECK_FALSE(run.setup.set_option("display.fullscreen", true));
}

TEST_CASE("commands drive it as the calls do") {
    Run run;
    CHECK_FALSE(run.setup.command("next"));
    CHECK(run.setup.command("preset original"));
    CHECK(run.setup.command("option display.aspect 16:9"));
    CHECK_FALSE(run.setup.command("option display.aspect 21:9"));
    CHECK(run.setup.command("next"));
    CHECK(run.setup.command("tier modern"));
    CHECK_FALSE(run.setup.command("choose picture x"));
    CHECK(run.setup.command("choose picture 0"));
    CHECK_FALSE(run.setup.command("dance"));
    CHECK(run.setup.command("next"));
    CHECK(run.setup.command("next"));
    CHECK(run.setup.command("finish"));
    CHECK(run.game.aspect == "16:9");
    CHECK_FALSE(run.game.smooth);
    CHECK(run.game.lights);
}

TEST_CASE("a view as JSON") {
    Run run;
    std::string json = setup_json(run.setup.view());
    CHECK(json.starts_with("{\"step\":\"welcome\",\"page\":0,\"pageCount\":3,\"id\":\"\""));
    CHECK(json.find("{\"id\":\"custom\",\"label\":\"Custom\",\"help\":\"\"}") != std::string::npos);
    CHECK(run.setup.command("preset original"));
    CHECK(run.setup.command("next"));
    json = setup_json(run.setup.view());
    CHECK(json.find("\"items\":[{\"type\":\"group\",\"id\":\"picture\",\"label\":\"Picture\",\"help\":\"\",\"chosen\":0,"
                    "\"choices\":[{\"label\":\"Rough\",\"help\":\"\",\"gpuCost\":false}") != std::string::npos);
    CHECK(json.find("\"pageTier\":0") != std::string::npos);
    CHECK(run.setup.command("back"));
    json = setup_json(run.setup.view());
    CHECK(json.find("{\"type\":\"option\",\"row\":{\"id\":\"display.fullscreen\"") != std::string::npos);
}
