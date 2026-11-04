#include <verdite/title_menu.h>

#include <doctest/doctest.h>

#include <algorithm>

using namespace verdite;

namespace {

std::vector<TitleSave> saves(bool first, bool second, bool third) {
    return {{first, first ? "LEVEL 3" : "", 0, {}}, {second, "", 0, {}}, {third, "", 0, {}}};
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

TitleProfile profile() {
    return {"PLAYER1234", 0, 8, 0, 2};
}

TitleMenu open_menu(std::vector<TitleSave> slots = saves(false, false, false), std::vector<TitleWorld> worlds = {}) {
    TitleMenu menu(1234);
    menu.open(std::move(slots), std::move(worlds), profile(), {{"one", "ONE", TitleModState::Off, {}}, {"two", "TWO", TitleModState::Off, {}}, {"bad", "BAD", TitleModState::Failed, {}}});
    return menu;
}

TitleMenu open_mods(std::vector<TitleMod> mods, std::vector<TitleSave> slots = saves(false, false, false)) {
    TitleMenu menu(1234);
    menu.open(std::move(slots), {}, profile(), std::move(mods));
    return menu;
}

TitleMenu open_profile() {
    auto menu = open_menu();
    go_to(menu, TitleRowKind::Coop);
    go_to(menu, TitleRowKind::Profile);
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

TEST_CASE("the top page moves across and wraps, and Up and Down do nothing there") {
    auto menu = open_menu();
    CHECK(menu.view().rows.size() == 4);
    menu.press(TitleInput::Up);
    CHECK(selected(menu) == TitleRowKind::NewGame);
    menu.press(TitleInput::Down);
    CHECK(selected(menu) == TitleRowKind::NewGame);
    menu.press(TitleInput::Right);
    CHECK(selected(menu) == TitleRowKind::Coop);
    menu.press(TitleInput::Right);
    CHECK(selected(menu) == TitleRowKind::Mods);
    menu.press(TitleInput::Right);
    CHECK(selected(menu) == TitleRowKind::Continue);
    menu.press(TitleInput::Left);
    CHECK(selected(menu) == TitleRowKind::Mods);
}

TEST_CASE("MODS lists the mods, and Confirm asks for each one's switch the other way") {
    auto menu = open_menu();
    go_to(menu, TitleRowKind::Mods);
    REQUIRE(menu.view().page == TitlePage::Mods);
    REQUIRE(menu.view().rows.size() == 3);
    CHECK(menu.view().rows[1].mod->name == "TWO");

    menu.press(TitleInput::Confirm);
    TitleChoice choice = taken(menu);
    CHECK(choice.action == TitleAction::EnableMod);
    CHECK(choice.code == "one");

    menu.set_mods({{"one", "ONE", TitleModState::On, {}}, {"two", "TWO", TitleModState::On, {}}, {"bad", "BAD", TitleModState::Failed, {}}});
    menu.press(TitleInput::Confirm);
    CHECK(taken(menu).action == TitleAction::DisableMod);

    menu.press(TitleInput::Up);
    CHECK(menu.view().rows[2].selected);
    menu.press(TitleInput::Confirm);
    choice = taken(menu);
    CHECK(choice.action == TitleAction::DisableMod);
    CHECK(choice.code == "bad");

    menu.set_mods({{"one", "ONE", TitleModState::On, {}}});
    CHECK(menu.view().rows[0].selected);
    menu.set_mods({});
    CHECK(menu.view().rows.empty());
    menu.press(TitleInput::Confirm);
    CHECK_FALSE(menu.take_choice());

    menu.press(TitleInput::Back);
    CHECK(menu.view().page == TitlePage::Top);
    CHECK(selected(menu) == TitleRowKind::Mods);
}

TEST_CASE("CO-OP asks first only while a mod is On; Failed, Held and Off do not") {
    for (TitleModState state : {TitleModState::Off, TitleModState::Failed, TitleModState::Held}) {
        auto menu = open_mods({{"a", "A", state, {}}});
        go_to(menu, TitleRowKind::Coop);
        CHECK(menu.view().page == TitlePage::Coop);
    }
    auto menu = open_mods({{"a", "A", TitleModState::Off, {}}, {"b", "B", TitleModState::On, {}}});
    go_to(menu, TitleRowKind::Coop);
    REQUIRE(menu.view().page == TitlePage::CoopWarning);
    CHECK(selected(menu) == TitleRowKind::EnterCoop);
    CHECK(menu.view().rows.size() == 2);
    CHECK(menu.view().rows[1].kind == TitleRowKind::GoBack);
}

TEST_CASE("the CO-OP warning: ENTER goes on to CO-OP, BACK returns to the CO-OP row") {
    auto menu = open_mods({{"a", "A", TitleModState::On, {}}});
    go_to(menu, TitleRowKind::Coop);
    menu.press(TitleInput::Confirm);
    CHECK(menu.view().page == TitlePage::Coop);
    menu.press(TitleInput::Back);
    CHECK(menu.view().page == TitlePage::Top);
    CHECK(selected(menu) == TitleRowKind::Coop);

    go_to(menu, TitleRowKind::Coop);
    menu.press(TitleInput::Down);
    CHECK(selected(menu) == TitleRowKind::GoBack);
    menu.press(TitleInput::Confirm);
    CHECK(menu.view().page == TitlePage::Top);
    CHECK(selected(menu) == TitleRowKind::Coop);
    CHECK_FALSE(menu.take_choice());
}

TEST_CASE("Back from CO-OP goes to the top page, not the warning") {
    auto menu = open_mods({{"a", "A", TitleModState::On, {}}});
    go_to(menu, TitleRowKind::Coop);
    menu.press(TitleInput::Confirm);
    REQUIRE(menu.view().page == TitlePage::Coop);
    menu.press(TitleInput::Back);
    CHECK(menu.view().page == TitlePage::Top);
    CHECK(selected(menu) == TitleRowKind::Coop);
}

TEST_CASE("a save made with other mods asks first, with the cursor on BACK, and LOAD ANYWAY loads that slot") {
    auto slots = saves(true, true, false);
    slots[1].mod_changes = {{"ONE", TitleSaveModChange::Added}, {"TWO", TitleSaveModChange::OtherVersion}};
    auto menu = open_menu(slots);
    menu.press(TitleInput::Confirm);
    menu.press(TitleInput::Down);
    menu.press(TitleInput::Confirm);
    REQUIRE(menu.view().page == TitlePage::LoadWarning);
    CHECK(selected(menu) == TitleRowKind::GoBack);
    CHECK_FALSE(menu.take_choice());
    const auto rows = menu.view().rows;
    REQUIRE(rows.size() == 4);
    CHECK(rows[0].kind == TitleRowKind::LoadAnyway);
    CHECK(rows[2].kind == TitleRowKind::SaveMod);
    CHECK(rows[2].save_mod->name == "ONE");
    CHECK(rows[3].save_mod->change == TitleSaveModChange::OtherVersion);
    CHECK_FALSE(rows[2].enabled);

    menu.press(TitleInput::Up);
    CHECK(selected(menu) == TitleRowKind::LoadAnyway);
    menu.press(TitleInput::Up);
    CHECK(selected(menu) == TitleRowKind::SaveMod);
    menu.press(TitleInput::Confirm);
    CHECK_FALSE(menu.take_choice());
    menu.press(TitleInput::Down);
    CHECK(selected(menu) == TitleRowKind::LoadAnyway);
    menu.press(TitleInput::Confirm);
    const TitleChoice choice = taken(menu);
    CHECK(choice.action == TitleAction::Continue);
    CHECK(choice.slot == 1);
}

TEST_CASE("GO BACK and Back on a save's warning return to CONTINUE on that slot") {
    auto slots = saves(true, true, false);
    slots[1].mod_changes = {{"ONE", TitleSaveModChange::Missing}};
    auto menu = open_menu(slots);
    menu.press(TitleInput::Confirm);
    menu.press(TitleInput::Down);
    menu.press(TitleInput::Confirm);
    REQUIRE(menu.view().page == TitlePage::LoadWarning);
    menu.press(TitleInput::Confirm);
    REQUIRE(menu.view().page == TitlePage::Continue);
    CHECK(menu.view().rows[1].selected);
    CHECK_FALSE(menu.take_choice());

    menu.press(TitleInput::Confirm);
    REQUIRE(menu.view().page == TitlePage::LoadWarning);
    menu.press(TitleInput::Back);
    CHECK(menu.view().page == TitlePage::Continue);
    CHECK(menu.view().rows[1].selected);
    CHECK_FALSE(menu.take_choice());
}

TEST_CASE("a save made with the mods on now goes straight to its game") {
    auto menu = open_menu(saves(true, false, false));
    menu.press(TitleInput::Confirm);
    menu.press(TitleInput::Confirm);
    CHECK(menu.view().page == TitlePage::Continue);
    const TitleChoice choice = taken(menu);
    CHECK(choice.action == TitleAction::Continue);
    CHECK(choice.slot == 0);
}

TEST_CASE("a mod with settings opens its page; the switch flips by Confirm and by Left") {
    auto menu = open_mods({{"hud", "HUD", TitleModState::Off, {{"VOLUME", "5", true}, {"SPEED", "2", false}, {"SCALE", "1", true}}}});
    go_to(menu, TitleRowKind::Mods);
    menu.press(TitleInput::Confirm);
    REQUIRE(menu.view().page == TitlePage::ModSettings);
    const auto rows = menu.view().rows;
    REQUIRE(rows.size() == 4);
    CHECK(selected(menu) == TitleRowKind::ModSwitch);
    CHECK(rows[0].mod->name == "HUD");
    CHECK(rows[1].setting->label == "VOLUME");
    CHECK_FALSE(rows[2].enabled);
    CHECK_FALSE(menu.take_choice());

    menu.press(TitleInput::Confirm);
    TitleChoice choice = taken(menu);
    CHECK(choice.action == TitleAction::EnableMod);
    CHECK(choice.code == "hud");
    menu.press(TitleInput::Left);
    choice = taken(menu);
    CHECK(choice.action == TitleAction::EnableMod);
    CHECK(choice.code == "hud");
    menu.set_mods({{"hud", "HUD", TitleModState::On, {{"VOLUME", "5", true}, {"SPEED", "2", false}, {"SCALE", "1", true}}}});
    menu.press(TitleInput::Right);
    choice = taken(menu);
    CHECK(choice.action == TitleAction::DisableMod);
    CHECK(choice.code == "hud");
}

TEST_CASE("a setting steps by Confirm, Left and Right, with its slot and delta; a disabled one does not step") {
    auto menu = open_mods({{"hud", "HUD", TitleModState::Off, {{"VOLUME", "5", true}, {"SPEED", "2", false}, {"SCALE", "1", true}}}});
    go_to(menu, TitleRowKind::Mods);
    menu.press(TitleInput::Confirm);

    menu.point(1);
    menu.press(TitleInput::Confirm);
    TitleChoice choice = taken(menu);
    CHECK(choice.action == TitleAction::StepModSetting);
    CHECK(choice.code == "hud");
    CHECK(choice.slot == 0);
    CHECK(choice.delta == 1);

    menu.press(TitleInput::Left);
    choice = taken(menu);
    CHECK(choice.slot == 0);
    CHECK(choice.delta == -1);

    menu.point(3);
    menu.press(TitleInput::Right);
    choice = taken(menu);
    CHECK(choice.action == TitleAction::StepModSetting);
    CHECK(choice.slot == 2);
    CHECK(choice.delta == 1);
    menu.press(TitleInput::Left);
    choice = taken(menu);
    CHECK(choice.slot == 2);
    CHECK(choice.delta == -1);

    menu.point(2);
    menu.press(TitleInput::Confirm);
    menu.press(TitleInput::Left);
    menu.press(TitleInput::Right);
    CHECK_FALSE(menu.take_choice());
    CHECK(selected(menu) == TitleRowKind::ModSetting);
}

TEST_CASE("BACK from a mod's settings lands on the mod's row") {
    auto menu = open_mods({{"plain", "PLAIN", TitleModState::Off, {}},
                           {"hud", "HUD", TitleModState::Off, {{"VOLUME", "5", true}}}});
    go_to(menu, TitleRowKind::Mods);
    menu.point(1);
    menu.press(TitleInput::Confirm);
    REQUIRE(menu.view().page == TitlePage::ModSettings);
    menu.press(TitleInput::Back);
    CHECK(menu.view().page == TitlePage::Mods);
    CHECK(selected(menu) == TitleRowKind::Mod);
    CHECK(menu.view().rows[1].selected);
    CHECK(menu.view().rows[1].mod->id == "hud");
}

TEST_CASE("set_mods clamps the cursor to the settings that are left") {
    auto menu = open_mods({{"hud", "HUD", TitleModState::Off, {{"VOLUME", "5", true}, {"SCALE", "1", true}}}});
    go_to(menu, TitleRowKind::Mods);
    menu.press(TitleInput::Confirm);
    menu.point(2);
    menu.set_mods({{"hud", "HUD", TitleModState::Off, {{"VOLUME", "5", true}}}});
    REQUIRE(menu.view().page == TitlePage::ModSettings);
    CHECK(menu.view().rows.size() == 2);
    CHECK(selected(menu) == TitleRowKind::ModSetting);
    CHECK(menu.view().rows[1].selected);
}

TEST_CASE("set_mods takes ModSettings back to MODS when the mod has no settings left") {
    auto menu = open_mods({{"hud", "HUD", TitleModState::Off, {{"VOLUME", "5", true}}}});
    go_to(menu, TitleRowKind::Mods);
    menu.press(TitleInput::Confirm);
    REQUIRE(menu.view().page == TitlePage::ModSettings);
    menu.set_mods({{"hud", "HUD", TitleModState::Off, {}}});
    CHECK(menu.view().page == TitlePage::Mods);
    CHECK(selected(menu) == TitleRowKind::Mod);
    menu.press(TitleInput::Confirm);
    CHECK(taken(menu).action == TitleAction::EnableMod);
}

TEST_CASE("set_mods takes ModSettings back to MODS when the mod is gone") {
    auto menu = open_mods({{"hud", "HUD", TitleModState::Off, {{"VOLUME", "5", true}}}});
    go_to(menu, TitleRowKind::Mods);
    menu.press(TitleInput::Confirm);
    REQUIRE(menu.view().page == TitlePage::ModSettings);
    menu.set_mods({});
    CHECK(menu.view().page == TitlePage::Mods);
    CHECK(menu.view().rows.empty());
}

TEST_CASE("rows wrap, and Back returns to the row that led to the page") {
    auto menu = open_menu();
    go_to(menu, TitleRowKind::Coop);
    REQUIRE(menu.view().page == TitlePage::Coop);
    menu.press(TitleInput::Up);
    CHECK(selected(menu) == TitleRowKind::CoopSettings);
    menu.press(TitleInput::Back);
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

TEST_CASE("a platform that may not host has no HOST row, and joins and lists as any other") {
    TitleMenu menu(1234, TitleHosting::Refused);
    menu.open(saves(false, false, false), {{"12345678", 3, 2}}, profile(), {});
    go_to(menu, TitleRowKind::Coop);
    REQUIRE(menu.view().page == TitlePage::Coop);
    const auto rows = menu.view().rows;
    CHECK(std::ranges::none_of(rows, [](const TitleRowView &each) { return each.kind == TitleRowKind::Host; }));
    CHECK(rows.front().kind == TitleRowKind::JoinCode);
    CHECK(rows.front().selected);
    go_to(menu, TitleRowKind::RoomList);
    CHECK(menu.view().page == TitlePage::RoomList);
    menu.press(TitleInput::Back);
    CHECK(menu.view().page == TitlePage::Coop);
    CHECK(menu.view().rows[1].selected);
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
    REQUIRE(menu.view().rows.size() == 5);
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

TEST_CASE("names: capitals and digits, 1 to name_most") {
    CHECK(name_valid("PLAYER1234"));
    CHECK(name_valid("A"));
    CHECK_FALSE(name_valid(""));
    CHECK_FALSE(name_valid("ABCDEFGHIJKLM"));
    CHECK_FALSE(name_valid("Andrew"));
    CHECK_FALSE(name_valid("AN DREW"));
}

TEST_CASE("PROFILE shows the profile; Left and Right step the colour and helmet, chosen at once") {
    auto menu = open_profile();
    REQUIRE(menu.view().page == TitlePage::Profile);
    CHECK(menu.view().profile.name == "PLAYER1234");
    menu.press(TitleInput::Down);
    CHECK(selected(menu) == TitleRowKind::ProfileColour);
    menu.press(TitleInput::Left);
    TitleChoice choice = taken(menu);
    CHECK(choice.action == TitleAction::Profile);
    CHECK(choice.profile.colour == 7);
    menu.press(TitleInput::Right);
    menu.press(TitleInput::Right);
    CHECK(taken(menu).profile.colour == 1);
    menu.press(TitleInput::Down);
    menu.press(TitleInput::Confirm);
    choice = taken(menu);
    CHECK(choice.profile.helmet == 1);
    CHECK(choice.profile.colour == 1);
    CHECK(menu.view().profile.helmet == 1);
    menu.press(TitleInput::Back);
    CHECK(menu.view().page == TitlePage::Coop);
    CHECK(selected(menu) == TitleRowKind::Profile);
}

TEST_CASE("a single helmet is dimmed and does not step") {
    TitleMenu menu(1);
    menu.open({}, {}, {"A", 0, 8, 0, 1}, {});
    go_to(menu, TitleRowKind::Coop);
    go_to(menu, TitleRowKind::Profile);
    menu.point(2);
    CHECK(selected(menu) == TitleRowKind::ProfileHelmet);
    CHECK_FALSE(menu.view().rows[2].enabled);
    menu.press(TitleInput::Right);
    menu.press(TitleInput::Confirm);
    CHECK_FALSE(menu.take_choice().has_value());
}

TEST_CASE("PROFILE's symbol steps with Left and Right, wrapping, and a single one is dimmed") {
    TitleMenu menu(1);
    menu.open({}, {}, {"A", 0, 8, 0, 2, 0, 2}, {});
    go_to(menu, TitleRowKind::Coop);
    go_to(menu, TitleRowKind::Profile);
    menu.point(3);
    REQUIRE(selected(menu) == TitleRowKind::ProfileSymbol);
    menu.press(TitleInput::Right);
    CHECK(taken(menu).profile.symbol == 1);
    menu.press(TitleInput::Right);
    CHECK(taken(menu).profile.symbol == 0);
    menu.press(TitleInput::Confirm);
    CHECK(taken(menu).profile.symbol == 1);

    TitleMenu single(1);
    single.open({}, {}, {"A", 0, 8, 0, 1}, {});
    go_to(single, TitleRowKind::Coop);
    go_to(single, TitleRowKind::Profile);
    single.point(3);
    REQUIRE(selected(single) == TitleRowKind::ProfileSymbol);
    CHECK_FALSE(single.view().rows[3].enabled);
    single.press(TitleInput::Right);
    CHECK_FALSE(single.take_choice().has_value());
}

TEST_CASE("the name page starts from the name, takes letters as capitals, and DONE chooses it") {
    auto menu = open_profile();
    go_to(menu, TitleRowKind::ProfileName);
    REQUIRE(menu.view().page == TitlePage::NameEntry);
    CHECK(menu.view().code == "PLAYER1234");
    for (int i = 0; i < 10; ++i)
        menu.erase();
    const auto done = [&] {
        const auto rows = menu.view().rows;
        return *std::ranges::find(rows, TitleRowKind::Done, &TitleRowView::kind);
    };
    CHECK_FALSE(done().enabled);
    for (char letter : std::string_view("an-drew 7"))
        menu.type(letter);
    CHECK(menu.view().code == "ANDREW7");
    // The grid's first letter is A; Right moves along its line.
    menu.point(0);
    menu.press(TitleInput::Right);
    menu.press(TitleInput::Confirm);
    CHECK(menu.view().code == "ANDREW7B");
    for (int i = 0; i < 10; ++i)
        menu.type('Z');
    CHECK(menu.view().code.size() == name_most);
    go_to(menu, TitleRowKind::Done);
    const TitleChoice choice = taken(menu);
    CHECK(choice.action == TitleAction::Profile);
    CHECK(choice.profile.name == "ANDREW7BZZZZ");
    CHECK(menu.view().page == TitlePage::Profile);
    CHECK(menu.view().profile.name == "ANDREW7BZZZZ");
}

TEST_CASE("Back from the name page leaves the name as it was") {
    auto menu = open_profile();
    go_to(menu, TitleRowKind::ProfileName);
    menu.erase();
    menu.type('X');
    menu.press(TitleInput::Back);
    CHECK(menu.view().page == TitlePage::Profile);
    CHECK(selected(menu) == TitleRowKind::ProfileName);
    CHECK(menu.view().profile.name == "PLAYER1234");
    CHECK_FALSE(menu.take_choice().has_value());
}

TEST_CASE("the name grid: Down from its last line reaches DELETE, Up from DELETE goes back; a line wraps") {
    auto menu = open_profile();
    go_to(menu, TitleRowKind::ProfileName);
    const std::size_t columns = menu.view().grid_columns;
    const std::size_t lines = (name_letters.size() + columns - 1) / columns;
    menu.point(0);
    menu.press(TitleInput::Left);
    CHECK(menu.view().rows[columns - 1].selected);
    for (std::size_t i = 0; i < lines; ++i)
        menu.press(TitleInput::Down);
    CHECK(selected(menu) == TitleRowKind::Delete);
    menu.press(TitleInput::Down);
    CHECK(selected(menu) == TitleRowKind::Done);
    menu.press(TitleInput::Up);
    menu.press(TitleInput::Up);
    CHECK(menu.view().rows[(lines - 1) * columns].selected);
}

TEST_CASE("set_saves changes what a save warns of") {
    TitleMenu menu(1);
    menu.open(saves(true, false, false), {}, profile(), {});
    go_to(menu, TitleRowKind::Continue);
    menu.set_saves({{true, "", 1, {{"MOD", TitleSaveModChange::Missing}}}, {false, "", 0, {}}, {false, "", 0, {}}});
    go_to(menu, TitleRowKind::Slot);
    CHECK(menu.view().page == TitlePage::LoadWarning);
    menu.press(TitleInput::Back);
    menu.set_saves(saves(true, false, false));
    go_to(menu, TitleRowKind::Slot);
    CHECK(taken(menu).action == TitleAction::Continue);
}

TEST_CASE("the co-op settings are stepped on their own page, the room's under Host") {
    TitleMenu menu(1);
    menu.open({}, {}, {}, {});
    menu.set_coop_settings({{"NAME BARS", "ALWAYS", true}, {"LOCKED", "X", false}}, {{"NEW PLAYERS", "ON", true}});
    while (selected(menu) != TitleRowKind::Coop)
        menu.press(TitleInput::Right);
    menu.press(TitleInput::Confirm);
    REQUIRE(menu.view().page == TitlePage::Coop);
    CHECK_FALSE(menu.shows_coop_settings());
    while (selected(menu) != TitleRowKind::CoopSettings)
        menu.press(TitleInput::Down);
    menu.press(TitleInput::Confirm);
    REQUIRE(menu.view().page == TitlePage::CoopSettings);
    CHECK(menu.shows_coop_settings());
    CHECK(menu.view().rows.front().setting->label == "NAME BARS");

    menu.press(TitleInput::Left);
    auto choice = menu.take_choice();
    REQUIRE(choice.has_value());
    CHECK(choice.value_or(TitleChoice{}).action == TitleAction::StepCoopSetting);
    CHECK_FALSE(choice.value_or(TitleChoice{}).listed);
    CHECK(choice.value_or(TitleChoice{}).delta == -1);
    menu.press(TitleInput::Down);
    menu.press(TitleInput::Confirm);
    CHECK_FALSE(menu.take_choice().has_value());   // dimmed

    menu.press(TitleInput::Back);
    CHECK(selected(menu) == TitleRowKind::CoopSettings);
    while (selected(menu) != TitleRowKind::Host)
        menu.press(TitleInput::Up);
    menu.press(TitleInput::Confirm);
    while (selected(menu) != TitleRowKind::RoomSettings)
        menu.press(TitleInput::Down);
    menu.press(TitleInput::Confirm);
    REQUIRE(menu.view().page == TitlePage::RoomSettings);
    menu.press(TitleInput::Confirm);
    choice = menu.take_choice();
    REQUIRE(choice.has_value());
    CHECK(choice.value_or(TitleChoice{}).listed);
    CHECK(choice.value_or(TitleChoice{}).slot == 0);
    CHECK(choice.value_or(TitleChoice{}).delta == 1);
    menu.press(TitleInput::Back);
    CHECK(selected(menu) == TitleRowKind::RoomSettings);
}
