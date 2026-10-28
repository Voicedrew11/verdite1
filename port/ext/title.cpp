// The title menu under the logo (kf/platform/title.h), on verdite/title_menu.h: the card slots and
// kept coop worlds it offers, the list of public rooms while it is shown, and the room a co-op
// choice joins. The game starts once the player has chosen and, for co-op, the world can go in.
#include "coop_party.h"
#include "internal.hpp"

#include <kf/game/player.h>
#include <kf/game/save.h>
#include <kf/platform/host.h>
#include <kf/platform/input.h>
#include <kf/platform/menu_pointer.h>
#include <kf/platform/saves.h>
#include <kf/platform/title.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kf::ext {
namespace {

using Clock = std::chrono::steady_clock;
constexpr auto host_answer_wait = std::chrono::seconds(30);   // a guest in the room, not yet welcomed

struct Title {
    std::optional<verdite::TitleMenu> menu;
    bool open = false;
    std::array<KfSaveSlotSummary, KF_SAVE_SLOT_COUNT> slots{};
    std::optional<SaveSlot> continue_slot;   // loaded at the first point between updates
    bool returning = false;                  // a coop session from the title ended
    bool waiting = false;                    // on a co-op choice's room
    Clock::time_point waiting_since{};
    bool listing = false;
    std::vector<verdite::TitleRoom> rooms;
    verdite::RoomListState list_state = verdite::RoomListState::Connecting;
};
Title title;

std::uint64_t random_seed() {
    std::random_device random;
    return (static_cast<std::uint64_t>(random()) << 32) | random();
}

// Made at the first open, with the seed new rooms' codes are drawn from.
verdite::TitleMenu &menu() {
    if (!title.menu)
        title.menu.emplace(random_seed());
    return *title.menu;
}

std::vector<verdite::TitleSave> read_saves() {
    std::vector<verdite::TitleSave> saves;
    title.slots = {};
    if (save_system_read_catalog(title.slots) != KF_SAVE_RESULT_OK)
        title.slots = {};
    saves.reserve(title.slots.size());
    for (const KfSaveSlotSummary &slot : title.slots)
        saves.push_back({slot.state == KfSaveSlotState::Ready, {}});
    return saves;
}

// The player's profile as the menu shows it: the colours' and helmets' counts are coop's.
verdite::TitleProfile read_profile() {
    const CoopProfile profile = coop_own_profile();
    return {profile.name, profile.colour, coop_colour_count, profile.helmet, online_player_helmet_count};
}

// The worlds this machine hosts whose codes the menu can show.
std::vector<verdite::TitleWorld> read_worlds() {
    std::vector<verdite::TitleWorld> worlds;
    for (const std::string &room : coop_world_rooms()) {
        if (!verdite::room_code_valid(room))
            continue;
        std::vector<u8> bytes;
        if (coop_world_read(room, bytes) != SaveFileResult::Ok)
            continue;
        const auto file = coop_world_decode(bytes);
        if (!file)
            continue;
        const CoopParty &party = file->live.party;
        worlds.push_back({room, static_cast<std::uint32_t>(party.highest_floor()),
            static_cast<std::uint32_t>(party.characters().size())});
    }
    std::ranges::sort(worlds, {}, &verdite::TitleWorld::code);
    return worlds;
}

void keep_room_list() {
    verdite::TitleMenu &shown = menu();
    if (shown.wants_room_list() != title.listing) {
        title.listing = shown.wants_room_list();
        title.rooms.clear();
        title.list_state = verdite::RoomListState::Connecting;
        if (title.listing)
            online_list_open();
        else
            online_list_close();
    }
    if (title.listing && online_rooms(title.rooms, title.list_state))
        shown.set_rooms(title.rooms, title.list_state);
}

const char *failure_id(OnlineFailure failure) {
    switch (failure) {
    case OnlineFailure::NoHost: return "no_host";
    case OnlineFailure::InUse: return "in_use";
    case OnlineFailure::Full: return "full";
    case OnlineFailure::OtherBuild: return "other_build";
    case OnlineFailure::Unreachable: return "unreachable";
    case OnlineFailure::NoOnline: return "no_online";
    case OnlineFailure::None:
    case OnlineFailure::Other: break;
    }
    return "other";
}

std::optional<TitleStart> finish(TitleStart start) {
    title.open = false;
    title.waiting = false;
    if (title.listing)
        online_list_close();
    title.listing = false;
    return start;
}

void wait_on(std::string_view code, RoomRole role, std::string_view listed_as) {
    online_join_from_title(code, role, listed_as);
    title.waiting = true;
    title.waiting_since = Clock::now();
}

std::optional<TitleStart> take(const verdite::TitleChoice &choice) {
    using verdite::TitleAction;
    switch (choice.action) {
    case TitleAction::NewGame:
        return finish(TitleStart::NewGame);
    case TitleAction::Continue:
        title.continue_slot = static_cast<SaveSlot>(static_cast<u8>(SaveSlot::First) + choice.slot);
        coop_keep_leftover_aside();
        return finish(TitleStart::Continue);
    case TitleAction::Host:
        wait_on(choice.code, RoomRole::First, choice.listed ? coop_own_profile().name : std::string());
        break;
    case TitleAction::Join:
        wait_on(choice.code, RoomRole::Later, {});
        break;
    case TitleAction::CancelJoin:
        online_stop();
        title.waiting = false;
        break;
    case TitleAction::Profile:
        coop_set_own_profile({choice.profile.name, static_cast<std::uint8_t>(choice.profile.colour),
            static_cast<std::uint8_t>(choice.profile.helmet)});
        break;
    }
    return std::nullopt;
}

// The room of a co-op choice: in and ready, failed, or still on its way.
std::optional<TitleStart> keep_waiting() {
    const OnlineStatus status = online_status();
    if (status.in_room && coop_ready_from_title()) {
        menu().coop_started();
        coop_start_from_title();
        return finish(TitleStart::Coop);
    }
    if (!status.joining && !status.in_room) {
        title.waiting = false;
        menu().coop_failed(failure_id(status.failure));
        return std::nullopt;
    }
    if (Clock::now() - title.waiting_since > host_answer_wait) {
        online_stop();
        title.waiting = false;
        menu().coop_failed("no_answer");
    }
    return std::nullopt;
}

void press(u32 pressed) {
    struct Press {
        Button button;
        verdite::TitleInput input;
    };
    constexpr std::array<Press, 7> presses = {{{Button::Up, verdite::TitleInput::Up},
        {Button::Down, verdite::TitleInput::Down}, {Button::Left, verdite::TitleInput::Left},
        {Button::Right, verdite::TitleInput::Right}, {Button::Confirm, verdite::TitleInput::Confirm},
        {Button::Start, verdite::TitleInput::Confirm}, {Button::Back, verdite::TitleInput::Back}}};
    for (const Press &each : presses)
        if (pressed & each.button) {
            menu().press(each.input);
            return;
        }
}

// ---- the text, in the menu font: English capitals and digits, or kana and digits ----

struct Words {
    std::string_view english, japanese;
    std::string_view in(Language language) const { return language == Language::English ? english : japanese; }
};

constexpr Words continue_word{"CONTINUE", "つづきから"}, new_game_word{"NEW GAME", "はじめから"},
    coop_word{"COOP", "マルチプレイ"}, load_word{"LOAD", "ロード"}, host_word{"HOST", "ホスト"},
    join_code_word{"JOIN BY CODE", "コードでさんか"}, room_list_word{"PUBLIC ROOMS", "こうかいルーム"},
    new_world_word{"NEW WORLD", "あたらしいワールド"}, private_word{"PRIVATE", "ひこうかい"},
    public_word{"PUBLIC", "こうかい"}, public_world_word{"PUBLIC WORLD", "こうかいワールド"},
    room_code_word{"ROOM CODE", "ルームコード"}, start_word{"START", "はじめる"}, delete_word{"DELETE", "けす"},
    join_word{"JOIN", "さんか"}, connecting_word{"CONNECTING", "つないでいます"},
    no_rooms_word{"NO PUBLIC ROOMS", "ルームはありません"}, no_list_word{"THE LIST CANNOT BE REACHED", "いちらんがよめません"},
    empty_word{"EMPTY", "データなし"}, damaged_word{"DAMAGED", "こわれています"},
    experience_word{"EXP", "けいけんち"}, share_word{"TELL YOUR FRIENDS THIS CODE", "このコードをともだちに"},
    profile_word{"PROFILE", "プレイヤー"}, name_word{"NAME", "なまえ"}, colour_word{"COLOUR", "いろ"},
    helmet_word{"HELMET", "かぶと"}, done_word{"DONE", "おわり"};

struct Failure {
    std::string_view id;
    Words words;
};
constexpr std::array<Failure, 8> failures = {{
    {"no_host", {"NO ONE IS HOSTING THAT ROOM", "そのルームにホストがいません"}},
    {"in_use", {"THAT ROOM IS ALREADY IN USE", "そのルームはつかわれています"}},
    {"full", {"THAT ROOM IS FULL", "そのルームはまんいんです"}},
    {"other_build", {"THAT ROOM HAS ANOTHER VERSION", "ゲームのはんがちがいます"}},
    {"unreachable", {"THE SERVER CANNOT BE REACHED", "サーバーにつながりません"}},
    {"no_online", {"THIS BUILD HAS NO ONLINE PLAY", "オンラインはつかえません"}},
    {"no_answer", {"THE HOST DID NOT ANSWER", "ホストのへんじがありません"}},
    {"other", {"COULD NOT JOIN THE ROOM", "さんかできませんでした"}},
}};

std::string failure_text(std::string_view id, Language language) {
    const auto found = std::ranges::find(failures, id, &Failure::id);
    return std::string((found != failures.end() ? found->words : failures.back().words).in(language));
}

std::string floor_text(std::uint32_t floor, Language language) {
    return language == Language::English ? "FLOOR " + std::to_string(floor) : std::to_string(floor) + "かい";
}

// A generated name is PLAYER and four digits. The kana font has no capitals, so in Japanese PLAYER is
// プレイヤー and a chosen name loses its letters (docs/verdite-port.md, "Language handling").
std::string name_text(std::string_view name, Language language) {
    constexpr std::string_view generated = "PLAYER";
    if (language != Language::English && name.starts_with(generated))
        return "プレイヤー" + std::string(name.substr(generated.size()));
    return std::string(name);
}

std::string profile_words(const CoopProfileWords &words, Language language) {
    return std::string(language == Language::English ? words.english : words.japanese);
}

std::string slot_text(const KfSaveSlotSummary &slot, Language language) {
    if (slot.state == KfSaveSlotState::Empty || slot.state == KfSaveSlotState::Unavailable)
        return std::string(empty_word.in(language));
    if (slot.state == KfSaveSlotState::Damaged)
        return std::string(damaged_word.in(language));
    return floor_text(kf_enum_encode<u32>(slot.current_floor), language) + "  " +
           std::string(experience_word.in(language)) + " " + std::to_string(slot.experience);
}

std::string row_text(const verdite::TitleRowView &row, std::size_t index, Language language) {
    using verdite::TitleRowKind;
    switch (row.kind) {
    case TitleRowKind::Continue: return std::string(continue_word.in(language));
    case TitleRowKind::NewGame: return std::string(new_game_word.in(language));
    case TitleRowKind::Coop: return std::string(coop_word.in(language));
    case TitleRowKind::Slot: return std::to_string(index + 1) + "  " + slot_text(title.slots[index], language);
    case TitleRowKind::Host: return std::string(host_word.in(language));
    case TitleRowKind::JoinCode: return std::string(join_code_word.in(language));
    case TitleRowKind::RoomList: return std::string(room_list_word.in(language));
    case TitleRowKind::NewPrivateWorld:
    case TitleRowKind::NewPublicWorld: return std::string(new_world_word.in(language));
    case TitleRowKind::KeptWorld:
        return verdite::room_code_is_public(row.world->code) ? std::string(public_world_word.in(language)) : row.world->code;
    case TitleRowKind::Start: return std::string(start_word.in(language));
    case TitleRowKind::Letter: return std::string(1, row.letter);
    case TitleRowKind::Delete: return std::string(delete_word.in(language));
    case TitleRowKind::Join: return std::string(join_word.in(language));
    case TitleRowKind::Room: return name_text(row.room->host, language);
    case TitleRowKind::Profile: return std::string(profile_word.in(language));
    case TitleRowKind::ProfileName: return std::string(name_word.in(language));
    case TitleRowKind::ProfileColour: return std::string(colour_word.in(language));
    case TitleRowKind::ProfileHelmet: return std::string(helmet_word.in(language));
    case TitleRowKind::Done: return std::string(done_word.in(language));
    }
    return {};
}

std::string row_value(const verdite::TitleView &view, const verdite::TitleRowView &row, Language language) {
    using verdite::TitleRowKind;
    const verdite::TitleProfile &profile = view.profile;
    switch (row.kind) {
    case TitleRowKind::ProfileName: return name_text(profile.name, language);
    case TitleRowKind::ProfileColour:
        return profile.colour < coop_colour_count ? profile_words(coop_colours()[profile.colour].name, language) : std::string();
    case TitleRowKind::ProfileHelmet:
        return profile.helmet < online_player_helmet_count ? profile_words(coop_helmet_names()[profile.helmet], language)
                                                           : std::string();
    case TitleRowKind::NewPrivateWorld: return std::string(private_word.in(language));
    case TitleRowKind::NewPublicWorld: return std::string(public_word.in(language));
    case TitleRowKind::KeptWorld: return floor_text(row.world->highest_floor, language);
    case TitleRowKind::Room:
        return floor_text(row.room->floor, language) + "  " + std::to_string(row.room->players) + "/" +
               std::to_string(row.room->most);
    default: return {};
    }
}

std::string heading(verdite::TitlePage page, Language language) {
    using verdite::TitlePage;
    switch (page) {
    case TitlePage::Top: return {};
    case TitlePage::Continue: return std::string(load_word.in(language));
    case TitlePage::Coop: return std::string(coop_word.in(language));
    case TitlePage::Host: return std::string(host_word.in(language));
    case TitlePage::HostCode:
    case TitlePage::JoinCode: return std::string(room_code_word.in(language));
    case TitlePage::RoomList: return std::string(room_list_word.in(language));
    case TitlePage::Waiting: return std::string(connecting_word.in(language));
    case TitlePage::Profile: return std::string(profile_word.in(language));
    case TitlePage::NameEntry: return std::string(name_word.in(language));
    }
    return {};
}

std::string note(const verdite::TitleView &view, Language language) {
    using verdite::TitlePage;
    if (!view.failure.empty())
        return failure_text(view.failure, language);
    switch (view.page) {
    case TitlePage::HostCode: return std::string(share_word.in(language));
    case TitlePage::Waiting: return verdite::room_code_is_public(view.code) ? std::string() : view.code;
    case TitlePage::RoomList:
        if (view.list_state == verdite::RoomListState::Failed)
            return std::string(no_list_word.in(language));
        if (view.list_state == verdite::RoomListState::Connecting)
            return std::string(connecting_word.in(language));
        return view.rows.empty() ? std::string(no_rooms_word.in(language)) : std::string();
    default: return {};
    }
}

} // namespace

// From title.h's port side: called by coop.cpp.
void title_return() {
    title.returning = true;
}

} // namespace kf::ext

namespace kf {

using ext::title;

void title_open() {
    ext::menu().open(ext::read_saves(), ext::read_worlds(), ext::read_profile());
    title.open = true;
    title.returning = false;
    title.waiting = false;
    title.continue_slot.reset();
}

std::optional<TitleStart> title_update(u32 pressed) {
    if (!title.open)
        return std::nullopt;
    ext::keep_room_list();
    if (const auto pointed = host_menu_pointed_row(MenuWidget::List))
        ext::menu().point(static_cast<std::size_t>(*pointed));
    ext::press(pressed);
    if (const auto choice = ext::menu().take_choice())
        if (const auto start = ext::take(*choice))
            return start;
    if (title.waiting)
        return ext::keep_waiting();
    return std::nullopt;
}

TitleText title_text(Language language) {
    TitleText text;
    if (!title.menu)
        return text;
    const verdite::TitleView view = ext::menu().view();
    text.heading = ext::heading(view.page, language);
    text.grid_columns = static_cast<u32>(view.grid_columns);
    text.grid_cells = static_cast<u32>(std::ranges::count(view.rows, verdite::TitleRowKind::Letter, &verdite::TitleRowView::kind));
    for (std::size_t i = 0; i < view.rows.size(); ++i) {
        const verdite::TitleRowView &row = view.rows[i];
        text.rows.push_back({ext::row_text(row, i, language), ext::row_value(view, row, language), row.enabled, row.selected});
    }
    text.note = ext::note(view, language);
    if (view.page == verdite::TitlePage::HostCode || view.page == verdite::TitlePage::JoinCode)
        text.code = view.code;
    if (view.page == verdite::TitlePage::NameEntry)
        text.code = ext::name_text(view.code, language);
    return text;
}

bool title_is_open() {
    return title.open;
}

bool title_type(char key) {
    if (!title.open)
        return false;
    const verdite::TitlePage page = ext::menu().view().page;
    const bool letter = (key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z');
    if (page != verdite::TitlePage::JoinCode && page != verdite::TitlePage::NameEntry)
        return false;
    if (key == '\b')
        ext::menu().erase();
    else if ((key >= '0' && key <= '9') || (letter && page == verdite::TitlePage::NameEntry))
        ext::menu().type(key);
    else
        return false;
    return true;
}

void title_between_updates() {
    if (!title.continue_slot)
        return;
    const bool loaded =
        player_replace_game([] { return save_system_read_slot(*title.continue_slot) == KF_SAVE_RESULT_OK; });
    title.continue_slot.reset();
    if (!loaded)
        host_show_message("That save could not be loaded.");
}

bool title_return_requested() {
    return std::exchange(title.returning, false);
}

} // namespace kf
