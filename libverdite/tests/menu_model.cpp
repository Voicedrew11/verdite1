// Moving about a menu (verdite/menu_model.h), on stand-in rows. From
// holyland's tests/ui_menu_test.cpp.
#include <doctest/doctest.h>

#include <verdite/menu_model.h>

#include <vector>

using namespace verdite;

namespace {

struct FakeRow {
    MenuRowKind kind;
    bool enabled = true;
    int choices = 0;
    int value = 0; // choice: the chosen; toggle: 0 / 1; number: the value
    int activated = 0;
    int listened = -1; // a binding: the slot last listened for
    int cleared = -1;  // and last cleared
    std::vector<bool> dimmed = {}; // a choice's choices that cannot be chosen (missing: all can)
};

class FakeRows final : public MenuRows {
public:
    std::vector<std::vector<FakeRow>> tabs;

    int tab_count() const override {
        return static_cast<int>(tabs.size());
    }
    int row_count(int tab) const override {
        return static_cast<int>(tabs[static_cast<size_t>(tab)].size());
    }
    MenuRowShape shape(int tab, int row) const override {
        const FakeRow &r = tabs[static_cast<size_t>(tab)][static_cast<size_t>(row)];
        return {r.kind, r.enabled, r.choices, r.value};
    }
    void choose(int tab, int row, int choice) override {
        tabs[static_cast<size_t>(tab)][static_cast<size_t>(row)].value = choice;
    }
    void step(int tab, int row, int delta) override {
        tabs[static_cast<size_t>(tab)][static_cast<size_t>(row)].value += delta;
    }
    void activate(int tab, int row) override {
        tabs[static_cast<size_t>(tab)][static_cast<size_t>(row)].activated++;
    }
    void listen(int tab, int row, int slot) override {
        tabs[static_cast<size_t>(tab)][static_cast<size_t>(row)].listened = slot;
    }
    void clear(int tab, int row, int slot) override {
        tabs[static_cast<size_t>(tab)][static_cast<size_t>(row)].cleared = slot;
    }
    bool choice_enabled(int tab, int row, int choice) const override {
        const std::vector<bool> &dimmed = tabs[static_cast<size_t>(tab)][static_cast<size_t>(row)].dimmed;
        return static_cast<size_t>(choice) >= dimmed.size() || !dimmed[static_cast<size_t>(choice)];
    }
};

// Tab 0: heading, choice (3), toggle, disabled toggle, number, action. Tab 1: heading only. Tab 2: one toggle.
FakeRows make_rows() {
    FakeRows rows;
    rows.tabs = {
        {{MenuRowKind::Heading}, {MenuRowKind::Choice, true, 3, 1}, {MenuRowKind::Toggle}, {MenuRowKind::Toggle, false},
         {MenuRowKind::Number, true, 0, 10}, {MenuRowKind::Action}},
        {{MenuRowKind::Heading}},
        {{MenuRowKind::Toggle}},
    };
    return rows;
}

} // namespace

TEST_CASE("menu model: the focus skips headings and dimmed rows and stops at the ends") {
    FakeRows rows = make_rows();
    MenuModel menu(rows);
    menu.open_at(0);
    CHECK(menu.row() == 1); // past the heading
    menu.press(MenuInput::Up);
    CHECK(menu.row() == 1); // not past the top
    menu.press(MenuInput::Down);
    CHECK(menu.row() == 2);
    menu.press(MenuInput::Down);
    CHECK(menu.row() == 4); // past the disabled toggle
    menu.press(MenuInput::Down);
    menu.press(MenuInput::Down);
    CHECK(menu.row() == 5); // not past the bottom
}

TEST_CASE("menu model: left and right step choices, turn toggles and step numbers") {
    FakeRows rows = make_rows();
    MenuModel menu(rows);
    menu.open_at(0);
    menu.press(MenuInput::Right);
    CHECK(rows.tabs[0][1].value == 2);
    menu.press(MenuInput::Right);
    CHECK(rows.tabs[0][1].value == 2); // not past the last choice
    menu.press(MenuInput::Left);
    menu.press(MenuInput::Left);
    menu.press(MenuInput::Left);
    CHECK(rows.tabs[0][1].value == 0);
    // a value that is none of the choices: Right gives the first, Left the last
    rows.tabs[0][1].value = -1;
    menu.press(MenuInput::Right);
    CHECK(rows.tabs[0][1].value == 0);
    rows.tabs[0][1].value = -1;
    menu.press(MenuInput::Left);
    CHECK(rows.tabs[0][1].value == 2);

    menu.press(MenuInput::Down); // the toggle
    menu.press(MenuInput::Right);
    CHECK(rows.tabs[0][2].value == 1);
    menu.press(MenuInput::Right);
    CHECK(rows.tabs[0][2].value == 1);
    menu.press(MenuInput::Left);
    CHECK(rows.tabs[0][2].value == 0);

    menu.press(MenuInput::Down); // the number
    menu.press(MenuInput::Right);
    menu.press(MenuInput::Right);
    menu.press(MenuInput::Left);
    CHECK(rows.tabs[0][4].value == 11);
}

TEST_CASE("menu model: confirm opens a choice list, which up, down, confirm and back work") {
    FakeRows rows = make_rows();
    MenuModel menu(rows);
    menu.open_at(0);
    menu.press(MenuInput::Confirm);
    CHECK((menu.list_open() && menu.list_choice() == 1)); // on the chosen one
    menu.press(MenuInput::Down);
    menu.press(MenuInput::Down);
    CHECK(menu.list_choice() == 2); // not past the end
    CHECK(rows.tabs[0][1].value == 1); // nothing chosen yet
    CHECK(menu.press(MenuInput::Back) == MenuResult::Stay);
    CHECK((!menu.list_open() && rows.tabs[0][1].value == 1)); // Back: nothing chosen
    menu.press(MenuInput::Confirm);
    menu.press(MenuInput::Up);
    menu.press(MenuInput::Confirm);
    CHECK((!menu.list_open() && rows.tabs[0][1].value == 0));
    // in a list, Left and Right do nothing and Up and Down move only the list
    menu.press(MenuInput::Confirm);
    menu.press(MenuInput::Right);
    CHECK((rows.tabs[0][1].value == 0 && menu.row() == 1));
    menu.press(MenuInput::Back);

    menu.press(MenuInput::Down); // the toggle: Confirm turns it over
    menu.press(MenuInput::Confirm);
    CHECK((rows.tabs[0][2].value == 1 && !menu.list_open()));
    menu.press(MenuInput::Confirm);
    CHECK(rows.tabs[0][2].value == 0);

    menu.press(MenuInput::Down);
    menu.press(MenuInput::Down); // the action
    menu.press(MenuInput::Confirm);
    CHECK(rows.tabs[0][5].activated == 1);
    CHECK(menu.press(MenuInput::Back) == MenuResult::Close);
}

TEST_CASE("menu model: tabs go round and a tab change closes a list") {
    FakeRows rows = make_rows();
    MenuModel menu(rows);
    menu.open_at(0);
    menu.press(MenuInput::Confirm); // a list open: a tab change closes it
    menu.press(MenuInput::NextTab);
    CHECK((menu.tab() == 1 && !menu.list_open()));
    CHECK(menu.row() == -1); // nothing to use there
    menu.press(MenuInput::Down);
    menu.press(MenuInput::Confirm); // nothing happens
    CHECK(menu.row() == -1);
    menu.press(MenuInput::NextTab);
    CHECK((menu.tab() == 2 && menu.row() == 0));
    menu.press(MenuInput::NextTab);
    CHECK((menu.tab() == 0 && menu.row() == 1)); // round
    menu.press(MenuInput::PreviousTab);
    CHECK(menu.tab() == 2);
    menu.open_at(7); // no such tab: the first
    CHECK(menu.tab() == 0);
}

TEST_CASE("menu model: a row hidden or dimmed under the focus gives it to its neighbour") {
    FakeRows rows = make_rows();
    MenuModel menu(rows);
    menu.open_at(0);
    menu.press(MenuInput::Down); // the toggle, row 2
    rows.tabs[0][2].enabled = false;
    menu.press(MenuInput::Left); // settles first: the focus goes on to the number
    CHECK(menu.row() == 4);
    CHECK(rows.tabs[0][4].value == 9);
    // the rows below go: the focus goes back up
    rows.tabs[0].resize(3);
    menu.press(MenuInput::Down);
    CHECK(menu.row() == 1);
    // the list's row stops being a choice: the list closes
    menu.press(MenuInput::Confirm);
    CHECK(menu.list_open());
    rows.tabs[0][1].kind = MenuRowKind::Action;
    menu.press(MenuInput::Down);
    CHECK(!menu.list_open());
}

TEST_CASE("menu model: the pointer does what the keys do") {
    FakeRows rows = make_rows();
    MenuModel menu(rows);
    menu.open_at(0);
    menu.point_at_row(4);
    CHECK(menu.row() == 4);
    menu.point_at_row(3); // disabled: the focus stays
    menu.point_at_row(0); // a heading
    CHECK(menu.row() == 4);
    menu.press_row(2);
    CHECK((menu.row() == 2 && rows.tabs[0][2].value == 1));
    menu.press_step(1, 1);
    CHECK((menu.row() == 1 && rows.tabs[0][1].value == 2));
    menu.press_row(1);
    CHECK(menu.list_open());
    menu.point_at_row(5); // a list open: the pointer leaves the focus
    CHECK(menu.row() == 1);
    menu.press_choice(0);
    CHECK((!menu.list_open() && rows.tabs[0][1].value == 0));
    menu.press_row(1);
    menu.press_choice(9); // no such choice
    CHECK((menu.list_open() && rows.tabs[0][1].value == 0));
    menu.press_row(4); // a click outside the list closes it, and does nothing else
    CHECK((!menu.list_open() && menu.row() == 1));
    menu.press_tab(2);
    CHECK((menu.tab() == 2 && menu.row() == 0));
}

// Bindings: Left / Right choose the slot (not past the ends) and keep it from row to row; Confirm listens there,
// Clear empties it; a click on a slot listens there; Clear does nothing on other rows
TEST_CASE("menu model: binding slots are chosen, listened for and cleared") {
    FakeRows rows;
    rows.tabs = {{{MenuRowKind::Heading}, {MenuRowKind::Binding, true, 3}, {MenuRowKind::Binding, true, 3}, {MenuRowKind::Toggle}}};
    MenuModel menu(rows);
    menu.open_at(0);
    CHECK((menu.row() == 1 && menu.slot() == 0));
    menu.press(MenuInput::Left);
    CHECK(menu.slot() == 0);
    menu.press(MenuInput::Right);
    menu.press(MenuInput::Right);
    menu.press(MenuInput::Right);
    CHECK(menu.slot() == 2);
    menu.press(MenuInput::Confirm);
    CHECK((rows.tabs[0][1].listened == 2 && !menu.list_open()));
    menu.press(MenuInput::Down);
    CHECK((menu.row() == 2 && menu.slot() == 2));
    menu.press(MenuInput::Left);
    menu.press(MenuInput::Clear);
    CHECK((rows.tabs[0][2].cleared == 1 && rows.tabs[0][2].listened == -1));
    menu.press(MenuInput::Down);
    menu.press(MenuInput::Clear);
    CHECK(rows.tabs[0][3].value == 0);
    menu.press_slot(1, 0);
    CHECK((menu.row() == 1 && menu.slot() == 0 && rows.tabs[0][1].listened == 0));
    menu.press_slot(3, 1); // not a binding: nothing
    CHECK((menu.row() == 1 && rows.tabs[0][3].value == 0));
    menu.press_slot(2, 3); // no such slot
    CHECK(menu.row() == 1);
}

// The sound each input makes: a move only when something moved or changed, refused for Confirm with nothing to do
TEST_CASE("menu model: each input leaves the cue for what it did") {
    FakeRows rows = make_rows();
    MenuModel menu(rows);
    menu.open_at(0);
    CHECK(menu.take_cue() == MenuCue::None);
    menu.press(MenuInput::Up); // already the first usable row
    CHECK(menu.take_cue() == MenuCue::None);
    menu.press(MenuInput::Down);
    CHECK((menu.row() == 2 && menu.take_cue() == MenuCue::Move));
    CHECK(menu.take_cue() == MenuCue::None); // taken once
    menu.press(MenuInput::Left); // the toggle is off already
    CHECK(menu.take_cue() == MenuCue::None);
    menu.press(MenuInput::Right);
    CHECK((rows.tabs[0][2].value == 1 && menu.take_cue() == MenuCue::Move));
    menu.press(MenuInput::Confirm);
    CHECK((rows.tabs[0][2].value == 0 && menu.take_cue() == MenuCue::Confirm));
    menu.press(MenuInput::Down); // past the disabled row to the number
    CHECK((menu.row() == 4 && menu.take_cue() == MenuCue::Move));
    menu.press(MenuInput::Confirm); // a number: nothing to do
    CHECK(menu.take_cue() == MenuCue::Refused);
    menu.press(MenuInput::Clear); // not a binding
    CHECK(menu.take_cue() == MenuCue::None);
    menu.press(MenuInput::Down);
    menu.press(MenuInput::Down); // the last row: stays
    CHECK((menu.row() == 5 && menu.take_cue() == MenuCue::Move));
    menu.press(MenuInput::Confirm);
    CHECK((rows.tabs[0][5].activated == 1 && menu.take_cue() == MenuCue::Confirm));

    // a choice's list
    menu.open_at(0);
    menu.press(MenuInput::Confirm);
    CHECK((menu.list_open() && menu.take_cue() == MenuCue::Confirm));
    menu.press(MenuInput::Down);
    CHECK(menu.take_cue() == MenuCue::Move);
    menu.press(MenuInput::Down); // the last choice
    CHECK(menu.take_cue() == MenuCue::None);
    menu.press(MenuInput::Back);
    CHECK((!menu.list_open() && menu.take_cue() == MenuCue::Back));
    menu.press(MenuInput::Right);
    CHECK((rows.tabs[0][1].value == 2 && menu.take_cue() == MenuCue::Move));
    menu.press(MenuInput::Right); // the last choice
    CHECK(menu.take_cue() == MenuCue::None);
    menu.press(MenuInput::Confirm);
    menu.press(MenuInput::Confirm);
    CHECK((!menu.list_open() && menu.take_cue() == MenuCue::Confirm));
    CHECK((menu.press(MenuInput::Back) == MenuResult::Close && menu.take_cue() == MenuCue::Back));

    // tabs and the pointer
    menu.press(MenuInput::NextTab);
    CHECK((menu.tab() == 1 && menu.take_cue() == MenuCue::Move));
    menu.press_tab(1); // the tab shown
    CHECK(menu.take_cue() == MenuCue::None);
    menu.press_tab(0);
    CHECK(menu.take_cue() == MenuCue::Move);
    menu.point_at_row(1); // the row with the focus
    CHECK(menu.take_cue() == MenuCue::None);
    menu.point_at_row(4);
    CHECK((menu.row() == 4 && menu.take_cue() == MenuCue::Move));
    menu.point_at_row(0); // a heading
    CHECK(menu.take_cue() == MenuCue::None);
    menu.press_row(3); // dimmed
    CHECK((menu.row() == 4 && menu.take_cue() == MenuCue::Refused));
    menu.press_step(4, 1);
    CHECK((rows.tabs[0][4].value == 11 && menu.take_cue() == MenuCue::Move));
    menu.press_row(1);
    CHECK((menu.list_open() && menu.take_cue() == MenuCue::Confirm));
    menu.press_choice(0);
    CHECK((rows.tabs[0][1].value == 0 && menu.take_cue() == MenuCue::Confirm));
    menu.press_row(1);
    menu.press_row(-1); // outside: closes the list
    CHECK((!menu.list_open() && menu.take_cue() == MenuCue::Back));
}

// A dimmed choice: Left / Right pass over it, its list refuses it (and stays open), a click on it is refused
TEST_CASE("menu model: dimmed choices are passed over and refused") {
    FakeRows rows;
    rows.tabs = {{{MenuRowKind::Choice, true, 4, 0}}};
    rows.tabs[0][0].dimmed = {false, true, true, false};
    MenuModel menu(rows);
    menu.open_at(0);
    menu.press(MenuInput::Right);
    CHECK(rows.tabs[0][0].value == 3);
    static_cast<void>(menu.take_cue());
    menu.press(MenuInput::Right);
    CHECK((rows.tabs[0][0].value == 3 && menu.take_cue() == MenuCue::None));
    menu.press(MenuInput::Left);
    CHECK(rows.tabs[0][0].value == 0);
    menu.press(MenuInput::Confirm);
    CHECK(menu.list_open());
    menu.press(MenuInput::Down);
    static_cast<void>(menu.take_cue());
    menu.press(MenuInput::Confirm);
    CHECK((menu.list_open() && rows.tabs[0][0].value == 0 && menu.take_cue() == MenuCue::Refused));
    menu.press_choice(2);
    CHECK((menu.list_open() && rows.tabs[0][0].value == 0 && menu.take_cue() == MenuCue::Refused));
    menu.press_choice(3);
    CHECK((!menu.list_open() && rows.tabs[0][0].value == 3));
}

TEST_CASE("menu model: a page's data-actions press and point by name and value") {
    FakeRows rows = make_rows();
    MenuModel menu(rows);
    menu.open_at(0);
    CHECK(menu_hover(menu, "row", "2"));
    CHECK(menu.row() == 2);
    CHECK(menu_press(menu, "step", "1:-1"));
    CHECK(rows.tabs[0][1].value == 0);
    CHECK(menu.row() == 1);
    CHECK_FALSE(menu_press(menu, "row", "two"));
    CHECK_FALSE(menu_press(menu, "step", "1"));
    CHECK_FALSE(menu_press(menu, "nothing", "1"));
    CHECK_FALSE(menu_hover(menu, "tab", "0"));
}
