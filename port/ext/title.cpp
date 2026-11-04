// The title menu under the logo (kf/platform/title.h), on verdite/title_menu.h: the card slots and
// kept coop worlds it offers, the list of public rooms while it is shown, and the room a co-op
// choice joins. The game starts once the player has chosen and, for co-op, the world can go in.
#include "coop_party.h"
#include "internal.hpp"
#include "mod_menu.h"

#include <kf/game/player.h>
#include <kf/game/save.h>
#include <kf/platform/host.h>
#include <kf/platform/input.h>
#include <kf/platform/language_runtime.h>
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
    std::string mods_seen;                   // what the menu's mods and saves' warnings were read from
};
Title title;

std::uint64_t random_seed() {
    std::random_device random;
    return (static_cast<std::uint64_t>(random()) << 32) | random();
}

// Made at the first open, with the seed new rooms' codes are drawn from.
verdite::TitleMenu &menu() {
    if (!title.menu)
        title.menu.emplace(random_seed(), online_may_host() ? verdite::TitleHosting::Offered : verdite::TitleHosting::Refused);
    return *title.menu;
}

verdite::TitleSaveModChange save_mod_change(verdite::SavedModChange change) {
    switch (change) {
    case verdite::SavedModChange::Missing: return verdite::TitleSaveModChange::Missing;
    case verdite::SavedModChange::Added: return verdite::TitleSaveModChange::Added;
    case verdite::SavedModChange::OtherVersion: return verdite::TitleSaveModChange::OtherVersion;
    }
    return verdite::TitleSaveModChange::Missing;
}

verdite::TitleSave read_save(const KfSaveSlotSummary &slot, SaveSlot number) {
    const bool used = slot.state == KfSaveSlotState::Ready;
    if (!used)
        return {false, {}, 0, {}};
    SaveModInfo info = mods_save_info(number);
    std::vector<verdite::TitleSaveMod> changes;
    changes.reserve(info.differences.size());
    for (const verdite::SavedModDifference &difference : info.differences)
        changes.push_back({difference.mod.name, save_mod_change(difference.change)});
    return {true, {}, info.mods, std::move(changes)};
}

std::vector<verdite::TitleSave> read_saves() {
    std::vector<verdite::TitleSave> saves;
    title.slots = {};
    if (save_system_read_catalog(title.slots) != KF_SAVE_RESULT_OK)
        title.slots = {};
    saves.reserve(title.slots.size());
    u8 number = static_cast<u8>(SaveSlot::First);
    for (const KfSaveSlotSummary &slot : title.slots)
        saves.push_back(read_save(slot, static_cast<SaveSlot>(number++)));
    return saves;
}

// The player's profile as the menu shows it: the colours' and helmets' counts are coop's.
verdite::TitleProfile read_profile() {
    const CoopProfile profile = coop_own_profile();
    return {profile.name, profile.colour, coop_colour_count, profile.helmet, online_player_helmet_count};
}

verdite::TitleModState mod_state(const verdite::ModInfo &mod) {
    if (mod.state == verdite::ModState::Failed)
        return verdite::TitleModState::Failed;
    if (mod.held)
        return verdite::TitleModState::Held;
    return mod.enabled ? verdite::TitleModState::On : verdite::TitleModState::Off;
}

std::vector<verdite::TitleModSetting> read_mod_settings(const verdite::ModInfo &mod, Language language) {
    std::vector<verdite::TitleModSetting> settings;
    settings.reserve(mod.menu.size());
    for (const verdite::ModMenuRow &row : mod.menu)
        settings.push_back({mod_menu_label(row, language), mod_menu_value(mod.id, row, language), mod_menu_rows_enabled()});
    return settings;
}

std::vector<verdite::TitleMod> read_mods() {
    std::vector<verdite::TitleMod> mods;
    for (const verdite::ModInfo &mod : mods_list())
        mods.push_back({mod.id, mod.name, mod_state(mod), read_mod_settings(mod, game_language())});
    return mods;
}

// What the menu's mods and the saves' warnings depend on: a mod switched or reloaded, on this page or
// in F1's Mods window, or a file saved in the mods folder.
std::string mods_seen_now() {
    std::string seen;
    for (const verdite::ModInfo &mod : mods_list())
        seen += mod.id + '\t' + mod.name + '\t' + mod.version + '\t' + std::to_string(static_cast<int>(mod.state)) +
                (mod.enabled ? "\ton\n" : "\toff\n");
    return seen;
}

void keep_mods_current() {
    std::string now = mods_seen_now();
    const verdite::TitlePage page = menu().view().page;
    const bool changed = now != title.mods_seen;
    if (changed) {
        title.mods_seen = std::move(now);
        menu().set_saves(read_saves());
    }
    if (changed || page == verdite::TitlePage::Mods || page == verdite::TitlePage::ModSettings)
        menu().set_mods(read_mods());
    if (menu().shows_coop_settings())
        menu().set_coop_settings(title_coop_settings(TitleCoopRows::Own, game_language()),
                                 title_coop_settings(TitleCoopRows::Room, game_language()));
}

void step_mod_setting(const verdite::TitleChoice &choice) {
    for (const verdite::ModInfo &mod : mods_list())
        if (mod.id == choice.code && choice.slot < mod.menu.size())
            mod_menu_step(mod.id, mod.menu[choice.slot], choice.delta);
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
    case OnlineFailure::NeedsEnglish: return "needs_english";
    case OnlineFailure::RoomClosed: return "room_closed";
    case OnlineFailure::SentAway: return "sent_away";
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
    case TitleAction::EnableMod:
        mods_set_enabled(choice.code, true);
        break;
    case TitleAction::DisableMod:
        mods_set_enabled(choice.code, false);
        break;
    case TitleAction::StepModSetting:
        step_mod_setting(choice);
        break;
    case TitleAction::StepCoopSetting:
        title_coop_setting_step(choice.listed ? TitleCoopRows::Room : TitleCoopRows::Own, choice.slot, choice.delta);
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

constexpr Words settings_word{"SETTINGS", "せつてい"}, room_settings_word{"ROOM SETTINGS", "ルームのせつてい"};
constexpr Words continue_word{"CONTINUE", "つづきから"}, new_game_word{"NEW GAME", "はじめから"},
    coop_word{"COOP", "マルチプレイ"}, coop_short_word{"COOP", "マルチ"}, load_word{"LOAD", "ロード"}, host_word{"HOST", "ホスト"},
    join_code_word{"JOIN BY CODE", "コードでさんか"}, room_list_word{"PUBLIC ROOMS", "こうかいルーム"},
    new_world_word{"NEW WORLD", "あたらしいワールド"}, private_word{"PRIVATE", "ひこうかい"},
    public_word{"PUBLIC", "こうかい"}, public_world_word{"PUBLIC WORLD", "こうかいワールド"},
    room_code_word{"ROOM CODE", "ルームコード"}, start_word{"START", "はじめる"}, delete_word{"DELETE", "けす"},
    join_word{"JOIN", "さんか"}, connecting_word{"CONNECTING", "つないでいます"},
    no_rooms_word{"NO PUBLIC ROOMS", "ルームはありません"}, no_list_word{"THE LIST CANNOT BE REACHED", "いちらんがよめません"},
    empty_word{"EMPTY", "データなし"}, damaged_word{"DAMAGED", "こわれています"},
    experience_word{"EXP", "けいけんち"}, share_word{"TELL YOUR FRIENDS THIS CODE", "このコードをともだちに"},
    profile_word{"PROFILE", "プレイヤー"}, name_word{"NAME", "なまえ"}, colour_word{"COLOUR", "いろ"},
    helmet_word{"HELMET", "かぶと"}, done_word{"DONE", "おわり"}, mods_word{"MODS", "かいぞう"},
    on_word{"ON", "オン"}, off_word{"OFF", "オフ"}, error_word{"ERROR", "エラー"}, held_word{"HELD", "ていし"},
    no_mods_word{"NO MODS YET", "かいぞうはありません"}, held_note_word{"OFF IN CLASSIC MODE", "クラシックではていし"},
    warning_word{"WARNING", "ちゅうい"}, load_anyway_word{"LOAD ANYWAY", "それでもロード"}, back_word{"BACK", "もどる"},
    enter_word{"ENTER", "はいる"}, enabled_word{"SWITCH", "スイッチ"}, off_now_word{"OFF NOW", "いまはオフ"},
    new_word{"NEW", "あたらしい"}, changed_word{"CHANGED", "はんがちがう"},
    other_mods_word{"THIS SAVE WAS MADE WITH OTHER MODS", "べつのかいぞうのセーブです"},
    coop_mods_word{"YOUR MODS STAY OFF WHILE IN COOP", "マルチプレイではかいぞうはオフです"};

struct Failure {
    std::string_view id;
    Words words;
};
constexpr std::array<Failure, 13> failures = {{
    {"no_host", {"NO ONE IS HOSTING THAT ROOM", "そのルームにホストがいません"}},
    {"in_use", {"THAT ROOM IS ALREADY IN USE", "そのルームはつかわれています"}},
    {"full", {"THAT ROOM IS FULL", "そのルームはまんいんです"}},
    {"other_build", {"THAT ROOM HAS ANOTHER VERSION", "ゲームのはんがちがいます"}},
    {"unreachable", {"THE SERVER CANNOT BE REACHED", "サーバーにつながりません"}},
    {"no_online", {"THIS BUILD HAS NO ONLINE PLAY", "オンラインはつかえません"}},
    {"no_answer", {"THE HOST DID NOT ANSWER", "ホストのへんじがありません"}},
    {"needs_english", {"COOP NEEDS THE ENGLISH FILES", "マルチプレイには えいごばんが いります"}},
    {"adding_english", {"ADDING THE ENGLISH FILES", "えいごばんを ついかしています"}},
    {"english_ready", {"THE ENGLISH FILES ARE READY", "えいごばんの じゅんびが できました"}},
    {"room_closed", {"THE ROOM IS CLOSED", "ルームは しめきりです"}},
    {"sent_away", {"THE HOST SENT YOU AWAY", "ホストに おいだされました"}},
    {"other", {"COULD NOT JOIN THE ROOM", "さんかできませんでした"}},
}};

std::string failure_text(std::string_view id, Language language) {
    const auto found = std::ranges::find(failures, id, &Failure::id);
    return std::string((found != failures.end() ? found->words : failures.back().words).in(language));
}

std::string floor_text(std::uint32_t floor, Language language) {
    return language == Language::English ? "FLOOR " + std::to_string(floor) : std::to_string(floor) + "かい";
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

constexpr std::size_t mod_name_most = 16;   // reaches the value's box at the menu font's width

// A mod's name in the menu font, which has capitals but no lower case, as much as fits.
std::string mod_name_text(std::string_view name) {
    std::string text;
    std::size_t letters = 0;
    for (const char c : name) {
        const bool continuation = (static_cast<unsigned char>(c) & 0xc0) == 0x80;
        if (!continuation && letters == mod_name_most)
            break;
        letters += continuation ? 0 : 1;
        text += c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
    }
    return text;
}

std::string_view mod_state_word(verdite::TitleModState state, Language language) {
    switch (state) {
    case verdite::TitleModState::Off: return off_word.in(language);
    case verdite::TitleModState::On: return on_word.in(language);
    case verdite::TitleModState::Failed: return error_word.in(language);
    case verdite::TitleModState::Held: return held_word.in(language);
    }
    return {};
}

std::string_view save_mod_word(verdite::TitleSaveModChange change, Language language) {
    switch (change) {
    case verdite::TitleSaveModChange::Missing: return off_now_word.in(language);
    case verdite::TitleSaveModChange::Added: return new_word.in(language);
    case verdite::TitleSaveModChange::OtherVersion: return changed_word.in(language);
    }
    return {};
}

std::string save_mods_text(std::size_t mods, Language language) {
    if (mods == 0)
        return {};
    return std::to_string(mods) + " " + std::string(mods_word.in(language));
}

std::string row_text(const verdite::TitleRowView &row, std::size_t index, Language language) {
    using verdite::TitleRowKind;
    switch (row.kind) {
    case TitleRowKind::Continue: return std::string(continue_word.in(language));
    case TitleRowKind::NewGame: return std::string(new_game_word.in(language));
    case TitleRowKind::Coop: return std::string(coop_short_word.in(language));   // the top page's line is tight
    case TitleRowKind::Mods: return std::string(mods_word.in(language));
    case TitleRowKind::Mod: return mod_name_text(row.mod->name);
    case TitleRowKind::ModSwitch: return std::string(enabled_word.in(language));
    case TitleRowKind::ModSetting:
    case TitleRowKind::CoopSetting: return row.setting->label;
    case TitleRowKind::CoopSettings: return std::string(settings_word.in(language));
    case TitleRowKind::RoomSettings: return std::string(room_settings_word.in(language));
    case TitleRowKind::LoadAnyway: return std::string(load_anyway_word.in(language));
    case TitleRowKind::SaveMod: return mod_name_text(row.save_mod->name);
    case TitleRowKind::EnterCoop: return std::string(enter_word.in(language));
    case TitleRowKind::GoBack: return std::string(back_word.in(language));
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
    case TitleRowKind::Room: return row.room->host;
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
    case TitleRowKind::ProfileName: return profile.name;
    case TitleRowKind::ProfileColour:
        return profile.colour < coop_colour_count ? profile_words(coop_colours()[profile.colour].name, language) : std::string();
    case TitleRowKind::ProfileHelmet:
        return profile.helmet < online_player_helmet_count ? profile_words(coop_helmet_names()[profile.helmet], language)
                                                           : std::string();
    case TitleRowKind::NewPrivateWorld: return std::string(private_word.in(language));
    case TitleRowKind::NewPublicWorld: return std::string(public_word.in(language));
    case TitleRowKind::KeptWorld: return floor_text(row.world->highest_floor, language);
    case TitleRowKind::Mod:
    case TitleRowKind::ModSwitch: return std::string(mod_state_word(row.mod->state, language));
    case TitleRowKind::ModSetting:
    case TitleRowKind::CoopSetting: return row.setting->value;
    case TitleRowKind::SaveMod: return std::string(save_mod_word(row.save_mod->change, language));
    case TitleRowKind::Slot: return save_mods_text(row.save->mods, language);
    case TitleRowKind::Room:
        return floor_text(row.room->floor, language) + "  " + std::to_string(row.room->players) + "/" +
               std::to_string(row.room->most);
    default: return {};
    }
}

std::string heading(const verdite::TitleView &view, Language language) {
    using verdite::TitlePage;
    switch (view.page) {
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
    case TitlePage::Mods: return std::string(mods_word.in(language));
    case TitlePage::ModSettings: return view.rows.empty() ? std::string() : mod_name_text(view.rows.front().mod->name);
    case TitlePage::LoadWarning:
    case TitlePage::CoopWarning: return std::string(warning_word.in(language));
    case TitlePage::CoopSettings: return std::string(settings_word.in(language));
    case TitlePage::RoomSettings: return std::string(room_settings_word.in(language));
    }
    return {};
}

// While coop waits on the English game files: being added, ready to try again, or failed.
std::string_view english_note() {
    switch (language_task_state(nullptr)) {
    case LanguageTask::Working: return "adding_english";
    case LanguageTask::Done: return "english_ready";
    case LanguageTask::Idle:
    case LanguageTask::Failed: break;
    }
    return "needs_english";
}

std::string note(const verdite::TitleView &view, Language language) {
    using verdite::TitlePage;
    if (view.failure == "needs_english")
        return failure_text(english_note(), language);
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
    case TitlePage::Mods: {
        if (view.rows.empty())
            return std::string(no_mods_word.in(language));
        const auto on = std::ranges::find(view.rows, true, &verdite::TitleRowView::selected);
        const bool held = on != view.rows.end() && on->mod->state == verdite::TitleModState::Held;
        return held ? std::string(held_note_word.in(language)) : std::string();
    }
    case TitlePage::LoadWarning: return std::string(other_mods_word.in(language));
    case TitlePage::CoopWarning: return std::string(coop_mods_word.in(language));
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
    title.mods_seen = ext::mods_seen_now();
    ext::menu().open(ext::read_saves(), ext::read_worlds(), ext::read_profile(), ext::read_mods());
    title.open = true;
    title.returning = false;
    title.waiting = false;
    title.continue_slot.reset();
}

std::optional<TitleStart> title_update(u32 pressed) {
    if (!title.open)
        return std::nullopt;
    ext::keep_room_list();
    ext::keep_mods_current();
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
    text.heading = ext::heading(view, language);
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
        text.code = view.code;
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
