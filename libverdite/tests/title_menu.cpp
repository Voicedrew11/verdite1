#include <verdite/title_menu.h>

#include <doctest/doctest.h>

#include <algorithm>

using namespace verdite;

namespace {

std::vector<TitleSave> saves(bool first, bool second, bool third) {
    return {{first, first ? "LEVEL 3" : ""}, {second, ""}, {third, ""}};
}

TitleRowKind selected(const TitleMenu &menu) {
    const auto view = menu.view();
    const auto found = std::ranges::find(view.rows, true, &TitleRowView::selected);
    REQUIRE(found != view.rows.end());
    return found->kind;
}

void go_to(TitleMenu &menu, TitleRowKind kind) {
    const auto rows = menu.view().rows;
    const auto found = std::ranges::find(rows, kind, &TitleRowView::kind);
    REQUIRE(found != rows.end());
    menu.point(static_cast<std::size_t>(found - rows.begin()));
    menu.press(TitleInput::Confirm);
}

// The choice made, which must be there.
TitleChoice taken(TitleMenu &menu) {
    const auto choice = menu.take_choice();
    REQUIRE(choice.has_value());
    return choice.value_or(TitleChoice{});
}

TitleMenu open_menu(std::vector<TitleSave> slots = saves(false, false, false), std::vector<TitleWorld> worlds = {}) {
    TitleMenu menu(1234);
    menu.open(std::move(slots), std::move(worlds));
    return menu;
}

} // namespace

TEST_CASE("room codes: a private one is digits, a public one P and letters, and the two never meet") {
    for (std::uint64_t bits : {0ULL, 1ULL, 0xffffffffffffffffULL, 0x123456789abcdefULL}) {
        const std::string closed = room_code_private(bits);
        const std::string open = room_code_public(bits);
        CHECK(closed.size() == private_code_length);
        CHECK(open.size() == public_code_length);
        CHECK(room_code_valid(closed));
        CHECK(room_code_valid(open));
        CHECK_FALSE(room_code_is_public(closed));
        CHECK(room_code_is_public(open));
    }
    CHECK(public_code_letters.find_first_of("01OI") == std::string_view::npos);
    CHECK(room_code_valid("01234567"));
    CHECK_FALSE(room_code_valid("0123456"));
    CHECK_FALSE(room_code_valid("0123456A"));
    CHECK_FALSE(room_code_valid("PABCDEF0"));
    CHECK_FALSE(room_code_valid("QABCDEFG"));
    CHECK_FALSE(room_code_valid("pabcdefg"));
    CHECK_FALSE(room_code_valid(""));
}

TEST_CASE("the top page starts on CONTINUE with a save, on NEW GAME without") {
    auto without = open_menu();
    CHECK(selected(without) == TitleRowKind::NewGame);
    CHECK_FALSE(without.view().rows[0].enabled);

    auto with = open_menu(saves(false, true, false));
    CHECK(selected(with) == TitleRowKind::Continue);
    CHECK(with.view().rows[0].enabled);
}

TEST_CASE("NEW GAME is chosen once") {
    auto menu = open_menu();
    menu.press(TitleInput::Confirm);
    const TitleChoice choice = taken(menu);
    CHECK(choice.action == TitleAction::NewGame);
    CHECK_FALSE(menu.take_choice());
}

TEST_CASE("CONTINUE lists the slots and refuses an empty one") {
    auto menu = open_menu(saves(true, false, true));
    menu.press(TitleInput::Confirm);
    REQUIRE(menu.view().page == TitlePage::Continue);
    REQUIRE(menu.view().rows.size() == 3);
    CHECK(menu.view().rows[0].save->summary == "LEVEL 3");
    menu.press(TitleInput::Down);
    menu.press(TitleInput::Confirm);
    CHECK_FALSE(menu.take_choice());
    menu.press(TitleInput::Down);
    menu.press(TitleInput::Confirm);
    const TitleChoice choice = taken(menu);
    CHECK(choice.action == TitleAction::Continue);
    CHECK(choice.slot == 2);
}

TEST_CASE("rows wrap, and Back returns to the row that led to the page") {
    auto menu = open_menu();
    menu.press(TitleInput::Up);
    CHECK(selected(menu) == TitleRowKind::Continue);
    menu.press(TitleInput::Up);
    CHECK(selected(menu) == TitleRowKind::Coop);
    menu.press(TitleInput::Confirm);
    CHECK(menu.view().page == TitlePage::Coop);
    go_to(menu, TitleRowKind::RoomList);
    CHECK(menu.wants_room_list());
    menu.press(TitleInput::Back);
    CHECK(menu.view().page == TitlePage::Coop);
    CHECK(selected(menu) == TitleRowKind::RoomList);
    CHECK_FALSE(menu.wants_room_list());
    menu.press(TitleInput::Back);
    CHECK(selected(menu) == TitleRowKind::Coop);
    menu.press(TitleInput::Back);
    CHECK(menu.view().page == TitlePage::Top);
}

TEST_CASE("a new private world shows its code before START, and START hosts it unlisted") {
    auto menu = open_menu();
    go_to(menu, TitleRowKind::Coop);
    go_to(menu, TitleRowKind::Host);
    go_to(menu, TitleRowKind::NewPrivateWorld);
    REQUIRE(menu.view().page == TitlePage::HostCode);
    const std::string code = menu.view().code;
    CHECK(room_code_valid(code));
    CHECK_FALSE(room_code_is_public(code));
    CHECK_FALSE(menu.take_choice());
    menu.press(TitleInput::Confirm);
    const TitleChoice choice = taken(menu);
    CHECK(choice.action == TitleAction::Host);
    CHECK(choice.code == code);
    CHECK_FALSE(choice.listed);
    CHECK(menu.view().page == TitlePage::Waiting);
    CHECK(menu.view().code == code);
}

TEST_CASE("a new public world is hosted listed at once, under a public code") {
    auto menu = open_menu();
    go_to(menu, TitleRowKind::Coop);
    go_to(menu, TitleRowKind::Host);
    go_to(menu, TitleRowKind::NewPublicWorld);
    const TitleChoice choice = taken(menu);
    CHECK(choice.listed);
    CHECK(room_code_is_public(choice.code));
}

TEST_CASE("a kept world is hosted again under its code, listed when the code is public") {
    auto menu = open_menu(saves(false, false, false), {{"12345678", 3, 2}, {"PABCDEFG", 1, 1}});
    go_to(menu, TitleRowKind::Coop);
    go_to(menu, TitleRowKind::Host);
    REQUIRE(menu.view().rows.size() == 4);
    CHECK(menu.view().rows[2].world->code == "12345678");
    menu.point(3);
    menu.press(TitleInput::Confirm);
    const TitleChoice choice = taken(menu);
    CHECK(choice.code == "PABCDEFG");
    CHECK(choice.listed);
}

TEST_CASE("the join page takes typing and the grid, and JOIN only for a whole code") {
    auto menu = open_menu();
    go_to(menu, TitleRowKind::Coop);
    go_to(menu, TitleRowKind::JoinCode);
    REQUIRE(menu.view().page == TitlePage::JoinCode);
    CHECK(menu.view().grid_columns > 0);
    for (char letter : std::string_view("1a2 3"))
        menu.type(letter);
    CHECK(menu.view().code == "123");
    // The grid's first digit is 0; Right moves along its line.
    menu.press(TitleInput::Right);
    menu.press(TitleInput::Right);
    menu.press(TitleInput::Right);
    menu.press(TitleInput::Right);
    menu.press(TitleInput::Confirm);
    CHECK(menu.view().code == "1234");
    const auto join = [&] {
        const auto rows = menu.view().rows;
        return *std::ranges::find(rows, TitleRowKind::Join, &TitleRowView::kind);
    };
    CHECK_FALSE(join().enabled);
    for (char digit : std::string_view("5678"))
        menu.type(digit);
    CHECK(join().enabled);
    menu.erase();
    CHECK(menu.view().code == "1234567");
    menu.type('8');
    go_to(menu, TitleRowKind::Join);
    const TitleChoice choice = taken(menu);
    CHECK(choice.action == TitleAction::Join);
    CHECK(choice.code == "12345678");
}

TEST_CASE("the code stops at its length") {
    auto menu = open_menu();
    go_to(menu, TitleRowKind::Coop);
    go_to(menu, TitleRowKind::JoinCode);
    for (int i = 0; i < 12; ++i)
        menu.type('9');
    CHECK(menu.view().code.size() == private_code_length);
}

TEST_CASE("the grid: Down from its last line reaches DELETE, Up from DELETE goes back") {
    auto menu = open_menu();
    go_to(menu, TitleRowKind::Coop);
    go_to(menu, TitleRowKind::JoinCode);
    const std::size_t lines = private_code_digits.size() / menu.view().grid_columns;
    for (std::size_t i = 0; i < lines; ++i)
        menu.press(TitleInput::Down);
    CHECK(selected(menu) == TitleRowKind::Delete);
    menu.press(TitleInput::Down);
    CHECK(selected(menu) == TitleRowKind::Join);
    menu.press(TitleInput::Up);
    menu.press(TitleInput::Up);
    CHECK(selected(menu) == TitleRowKind::Letter);
    menu.press(TitleInput::Left);
    CHECK(selected(menu) == TitleRowKind::Letter);
}

TEST_CASE("the room list follows the service, keeps the cursor on its room, and refuses a full one") {
    auto menu = open_menu();
    go_to(menu, TitleRowKind::Coop);
    go_to(menu, TitleRowKind::RoomList);
    CHECK(menu.view().list_state == RoomListState::Connecting);
    menu.set_rooms({{"PAAAAAAA", "ONE", 1, 1, 4}, {"PBBBBBBB", "TWO", 2, 4, 4}}, RoomListState::Listing);
    menu.press(TitleInput::Down);
    CHECK(menu.view().rows[1].room->host == "TWO");
    CHECK_FALSE(menu.view().rows[1].enabled);
    menu.press(TitleInput::Confirm);
    CHECK_FALSE(menu.take_choice());
    menu.set_rooms({{"PCCCCCCC", "NEW", 1, 1, 4}, {"PAAAAAAA", "ONE", 1, 1, 4}, {"PBBBBBBB", "TWO", 2, 3, 4}},
        RoomListState::Listing);
    CHECK(menu.view().rows[2].selected);
    menu.press(TitleInput::Confirm);
    const TitleChoice choice = taken(menu);
    CHECK(choice.code == "PBBBBBBB");
}

TEST_CASE("a failed join goes back to its page saying why; Back while waiting cancels it") {
    auto menu = open_menu();
    go_to(menu, TitleRowKind::Coop);
    go_to(menu, TitleRowKind::JoinCode);
    for (char digit : std::string_view("12345678"))
        menu.type(digit);
    go_to(menu, TitleRowKind::Join);
    REQUIRE(menu.take_choice());
    menu.coop_failed("NO ONE IS HOSTING");
    CHECK(menu.view().page == TitlePage::JoinCode);
    CHECK(menu.view().code == "12345678");
    CHECK(menu.view().failure == "NO ONE IS HOSTING");
    CHECK(selected(menu) == TitleRowKind::Join);
    menu.press(TitleInput::Down);
    CHECK(menu.view().failure.empty());

    go_to(menu, TitleRowKind::Join);
    REQUIRE(menu.take_choice());
    menu.press(TitleInput::Back);
    const TitleChoice cancel = taken(menu);
    CHECK(cancel.action == TitleAction::CancelJoin);
    CHECK(menu.view().page == TitlePage::JoinCode);
    // A failure arriving after the cancel changes nothing.
    menu.coop_failed("late");
    CHECK(menu.view().failure.empty());
}
