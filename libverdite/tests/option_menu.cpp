// The option table as a menu (verdite/option_menu.h)
#include <verdite/option_menu.h>

#include <doctest/doctest.h>

#include <string>
#include <utility>
#include <vector>

using namespace verdite;

namespace {

OptionRow make_row(std::string id, OptionKind kind, std::string label) {
    OptionRow row;
    row.id = std::move(id);
    row.kind = kind;
    row.label = std::move(label);
    return row;
}

struct Game {
    bool fullscreen = false;
    std::string aspect = "4:3";
    double volume = 50;
    int resets = 0;
    bool fine = false;
};

OptionTable make_table(Game &game) {
    OptionRow fullscreen = make_row("display.fullscreen", OptionKind::Toggle, "Fullscreen");
    fullscreen.help = "Fill the screen.";
    fullscreen.get = [&] { return OptionValue(game.fullscreen); };
    fullscreen.set = [&](const OptionValue &v) { game.fullscreen = std::get<bool>(v); };
    OptionRow aspect = make_row("display.aspect", OptionKind::Choice, "Aspect <ratio>");
    aspect.choices = {{std::string("4:3"), "4:3"}, {std::string("16:9"), "16:9"}};
    aspect.get = [&] { return OptionValue(game.aspect); };
    aspect.set = [&](const OptionValue &v) { game.aspect = std::get<std::string>(v); };
    OptionRow volume = make_row("audio.volume", OptionKind::Number, "Volume");
    volume.min = 0;
    volume.max = 100;
    volume.step = 10;
    volume.format = "%.0f";
    volume.get = [&] { return OptionValue(game.volume); };
    volume.set = [&](const OptionValue &v) { game.volume = std::get<double>(v); };
    OptionRow reset = make_row("audio.reset", OptionKind::Action, "Reset");
    reset.run = [&] { game.resets++; };
    OptionRow fine = make_row("audio.fine", OptionKind::Toggle, "Fine");
    fine.get = [&] { return OptionValue(game.fine); };
    fine.set = [&](const OptionValue &v) { game.fine = std::get<bool>(v); };
    fine.state = [] { return OptionState{true, false, "Needs a pad"}; };
    OptionRow jump = make_row("controls.bindings.keys.jump", OptionKind::Status, "Jump");
    jump.get = [] { return OptionValue(std::string("Space")); };
    OptionRow version = make_row("about.version", OptionKind::Status, "Version");
    version.get = [] { return OptionValue(std::string("1.0")); };

    return OptionTable({
        {"Display", {{"Window", {fullscreen, aspect}}}},
        {"Audio", {{"", {volume, reset, fine}}}},
        {"Controls", {{"Keys", {jump, version}}}},
    });
}

class FakeBindings final : public BindingSlots {
public:
    std::string listened_row;
    int listened_slot = -1;
    int cleared_slot = -1;

    int slot_count(std::string_view row_id) const override {
        return row_id.starts_with("controls.bindings.") ? 2 : 0;
    }
    std::string slot_rml(std::string_view, int slot) const override {
        return slot == 0 ? "<key>Space</key>" : "";
    }
    void listen(std::string_view row_id, int slot) override {
        listened_row = row_id;
        listened_slot = slot;
    }
    void clear(std::string_view, int slot) override { cleared_slot = slot; }
    bool listening(std::string_view row_id, int slot) const override {
        return row_id == listened_row && slot == listened_slot;
    }
    std::string fixed_rml(std::string_view) const override { return "Esc"; }
};

bool contains(const std::string &text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

} // namespace

TEST_CASE("option menu: pages are tabs, sections a heading and their rows") {
    Game game;
    OptionTable table = make_table(game);
    OptionMenu menu(table, 0, nullptr);
    REQUIRE(menu.tab_count() == 3);
    CHECK(menu.row_count(0) == 3);   // heading, fullscreen, aspect
    CHECK(menu.row_count(1) == 3);   // no heading for an untitled section
    CHECK(menu.shape(0, 0).kind == MenuRowKind::Heading);
    CHECK(menu.shape(0, 1).kind == MenuRowKind::Toggle);
    CHECK(menu.shape(0, 2).kind == MenuRowKind::Choice);
    CHECK(menu.shape(0, 2).choice_count == 2);
    CHECK(menu.shape(0, 2).current == 0);
    CHECK(menu.shape(1, 0).kind == MenuRowKind::Number);
    CHECK(menu.shape(1, 1).kind == MenuRowKind::Action);
    CHECK_FALSE(menu.shape(1, 2).enabled);
    CHECK(menu.shape(2, 1).kind == MenuRowKind::Status);   // a binding without BindingSlots
    CHECK(menu.tab_of("Audio") == 1);
    CHECK(menu.tab_of("Nope") == -1);
    CHECK(menu.tab_title(1) == "Audio");
    CHECK(menu.tab_title(-1).empty());
    CHECK(menu.tab_title(menu.tab_count()).empty());
}

TEST_CASE("option menu: the model's inputs change the table's settings") {
    Game game;
    OptionTable table = make_table(game);
    OptionMenu menu(table, 0, nullptr);
    MenuModel model(menu);
    model.open_at(0);
    CHECK(menu.focused_id(model) == "display.fullscreen");
    model.press(MenuInput::Confirm);
    CHECK(game.fullscreen);
    model.press(MenuInput::Down);
    model.press(MenuInput::Right);
    CHECK(game.aspect == "16:9");
    model.press(MenuInput::NextTab);
    model.press(MenuInput::Right);
    model.press(MenuInput::Right);
    CHECK(game.volume == doctest::Approx(70));
    for (int i = 0; i < 10; i++) {
        model.press(MenuInput::Right);
    }
    CHECK(game.volume == doctest::Approx(100));   // clamped by the table
    model.press(MenuInput::Down);
    model.press(MenuInput::Confirm);
    CHECK(game.resets == 1);
    model.press(MenuInput::Down);   // the dimmed row is never focused
    CHECK(menu.focused_id(model) == "audio.reset");
}

TEST_CASE("option menu: the RML names each part and escapes text") {
    Game game;
    OptionTable table = make_table(game);
    OptionMenu menu(table, 0, nullptr);
    MenuModel model(menu);
    model.open_at(0);
    std::string tabs = menu.tabs_rml(model);
    CHECK(contains(tabs, "<tab data-action=\"tab\" data-value=\"0\" class=\"chosen\">Display</tab>"));
    CHECK(contains(tabs, "data-value=\"2\">Controls</tab>"));
    std::string rows = menu.rows_rml(model);
    CHECK(contains(rows, "<row class=\"heading\">Window</row>"));
    CHECK(contains(rows, "id=\"row-1\" data-action=\"row\" data-hover=\"row\" data-value=\"1\" "
                         "class=\"toggle focused\""));
    CHECK(contains(rows, "<label>Aspect &lt;ratio&gt;</label>"));
    CHECK(contains(rows, "<value>Off</value>"));
    CHECK(contains(menu.help_rml(model), "<h2>Fullscreen</h2><p>Fill the screen.</p>"));

    model.press(MenuInput::Down);
    model.press(MenuInput::Confirm);
    rows = menu.rows_rml(model);
    CHECK(contains(rows, "class=\"choice focused open\""));
    CHECK(contains(rows, "<list><choice data-action=\"choice\" data-value=\"0\" "
                         "class=\"highlighted chosen\">4:3</choice>"));

    model.press(MenuInput::Back);
    model.press(MenuInput::NextTab);
    rows = menu.rows_rml(model);
    CHECK(contains(rows, "<arrow data-action=\"step\" data-value=\"0:-1\">"));
    CHECK(contains(rows, "<bar><fill style=\"width: 50.0%\"/></bar>"));
    CHECK(contains(rows, "class=\"toggle disabled\""));
    CHECK(contains(rows, "<note>Needs a pad</note>"));
}

TEST_CASE("option menu: presses and hovers from the document reach the model") {
    Game game;
    OptionTable table = make_table(game);
    OptionMenu menu(table, 0, nullptr);
    MenuModel model(menu);
    model.open_at(0);
    CHECK(menu.press(model, "tab", "1"));
    CHECK(model.tab() == 1);
    CHECK(menu.press(model, "step", "0:1"));
    CHECK(game.volume == doctest::Approx(60));
    CHECK(menu.hover(model, "row", "1"));
    CHECK(model.row() == 1);
    CHECK(menu.press(model, "row", "1"));
    CHECK(game.resets == 1);
    CHECK_FALSE(menu.press(model, "row", "x"));
    CHECK_FALSE(menu.press(model, "step", "0"));
    CHECK_FALSE(menu.press(model, "console", "1"));
    CHECK_FALSE(menu.hover(model, "tab", "1"));
}

TEST_CASE("option menu: binding rows take their slots from BindingSlots") {
    Game game;
    OptionTable table = make_table(game);
    FakeBindings bindings;
    OptionMenu menu(table, 0, &bindings);
    MenuModel model(menu);
    model.open_at(2);
    CHECK(menu.shape(2, 1).kind == MenuRowKind::Binding);
    CHECK(menu.shape(2, 1).choice_count == 2);
    CHECK(menu.shape(2, 2).kind == MenuRowKind::Status);
    CHECK(model.row() == 1);
    model.press(MenuInput::Right);
    model.press(MenuInput::Confirm);
    CHECK(bindings.listened_row == "controls.bindings.keys.jump");
    CHECK(bindings.listened_slot == 1);
    std::string rows = menu.rows_rml(model);
    CHECK(contains(rows, "<fixed>Esc</fixed><slot data-action=\"slot\" data-value=\"1:0\"><key>Space</key></slot>"));
    CHECK(contains(rows, "data-value=\"1:1\" class=\"chosen listening\"></slot>"));
    CHECK(contains(rows, "<value>1.0</value>"));
    model.press(MenuInput::Clear);
    CHECK(bindings.cleared_slot == 1);
    CHECK(menu.press(model, "slot", "1:0"));
    CHECK(bindings.listened_slot == 0);
}

TEST_CASE("option menu: a choice's place and a number's share") {
    OptionRow row = make_row("x", OptionKind::Choice, "X");
    row.choices = {{1.0, "one"}, {2.0, "two"}};
    CHECK(option_choice_index(row, 2.0) == 1);
    CHECK(option_choice_index(row, 3.0) == -1);
    OptionRow number = make_row("n", OptionKind::Number, "N");
    number.min = 10;
    number.max = 20;
    CHECK(option_number_share(number, 15.0) == doctest::Approx(0.5));
    CHECK(option_number_share(number, 40.0) == doctest::Approx(1.0));
    CHECK(option_number_share(number, std::string("no")) == doctest::Approx(0.0));
}
