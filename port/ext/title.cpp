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
#include <ctime>
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

// The player's profile as the menu shows it: the colours', helmets' and symbols' counts are coop's.
verdite::TitleProfile read_profile() {
    const CoopProfile profile = coop_own_profile();
    return {profile.name, profile.colour, coop_colour_count, profile.helmet, online_player_helmet_count,
            profile.symbol, online_player_symbol_count, coop_own_profile_is_set_up()};
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

verdite::TitleWorld read_world(const std::string &room, const CoopWorldFile &file) {
    const CoopParty &party = file.live.party;
    verdite::TitleWorld world{room, static_cast<std::uint32_t>(party.highest_floor()), {},
        file.save_point ? static_cast<std::uint32_t>(file.save_point->floor) : 0u, party.gold(),
        file.played_ms / 1000, file.last_played_s};
    const CoopPlayerId own = coop_own_id();
    for (const CoopCharacter &character : party.characters())
        if (character.id != own)
            world.others.push_back(character.name);
    return world;
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
        worlds.push_back(read_world(room, *file));
    }
    return worlds;
}

void delete_world(std::string_view code) {
    if (coop_world_remove(code) != SaveFileResult::Ok)
        menu().failed_to_delete_world("delete_failed");
    menu().set_worlds(read_worlds());
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
            static_cast<std::uint8_t>(choice.profile.helmet), static_cast<std::uint8_t>(choice.profile.symbol)});
        break;
    case TitleAction::ProfileSetUp:
        coop_set_up_own_profile({choice.profile.name, static_cast<std::uint8_t>(choice.profile.colour),
            static_cast<std::uint8_t>(choice.profile.helmet), static_cast<std::uint8_t>(choice.profile.symbol)});
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
    case TitleAction::DeleteWorld:
        delete_world(choice.code);
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
    new_world_word{"NEW WORLD", "あたらしいワールド"}, options_word{"OPTIONS", "そのた"},
    private_word{"PRIVATE", "ひこうかい"}, public_word{"PUBLIC", "こうかい"},
    delete_note_word{"ITS CHARACTERS GO TOO", "なかまのデータもきえます"},
    solo_word{"SOLO", "ソロ"}, and_word{" AND ", " ほか "}, saved_word{"SAVED ", "セーブ "}, with_word{"WITH ", "なかま "},
    played_word{"PLAYED ", "プレイ "}, just_now_word{"JUST NOW", "いま"}, ago_word{" AGO", "まえ"},
    played_minutes_word{"M", "ふん"}, played_hours_word{"H", "じかん"},
    room_code_word{"ROOM CODE", "ルームコード"}, start_word{"START", "はじめる"}, delete_word{"DELETE", "けす"},
    join_word{"JOIN", "さんか"}, connecting_word{"CONNECTING", "つないでいます"},
    no_rooms_word{"NO PUBLIC ROOMS", "ルームはありません"}, no_list_word{"THE LIST CANNOT BE REACHED", "いちらんがよめません"},
    empty_word{"EMPTY", "データなし"}, damaged_word{"DAMAGED", "こわれています"},
    experience_word{"EXP", "けいけんち"}, share_word{"TELL YOUR FRIENDS THIS CODE", "このコードをともだちに"},
    profile_word{"PROFILE", "プレイヤー"}, name_word{"NAME", "なまえ"}, colour_word{"COLOUR", "いろ"},
    helmet_word{"HELMET", "かぶと"}, symbol_word{"SYMBOL", "しるし"}, done_word{"DONE", "おわり"}, mods_word{"MODS", "かいぞう"},
    on_word{"ON", "オン"}, off_word{"OFF", "オフ"}, error_word{"ERROR", "エラー"}, held_word{"HELD", "ていし"},
    no_mods_word{"NO MODS YET", "かいぞうはありません"}, held_note_word{"OFF IN CLASSIC MODE", "クラシックではていし"},
    warning_word{"WARNING", "ちゅうい"}, load_anyway_word{"LOAD ANYWAY", "それでもロード"}, back_word{"BACK", "もどる"},
    enter_word{"ENTER", "はいる"}, enabled_word{"SWITCH", "スイッチ"}, off_now_word{"OFF NOW", "いまはオフ"},
    new_word{"NEW", "あたらしい"}, changed_word{"CHANGED", "はんがちがう"},
    other_mods_word{"THIS SAVE WAS MADE WITH OTHER MODS", "べつのかいぞうのセーブです"},
    coop_mods_word{"YOUR MODS STAY OFF WHILE IN COOP", "マルチプレイではかいぞうはオフです"},
    next_word{"NEXT", "つぎへ"};

// A player's first time in coop. The menu font has no apostrophe, so it says the game rather than the name.
constexpr std::array<Words, 6> coop_intro_lines = {{
    {"COOP IS DIFFERENT FROM", "マルチプレイは ふつうの"},
    {"THE STANDARD GAME", "ゲームとは ちがいます"},
    {"", ""},
    {"IT IS STREAMLINED TO BE MORE", "オンラインで あそびやすいように"},
    {"CASUAL AND APPROACHABLE", "かんたんで きがるな"},
    {"FOR ONLINE PLAY", "つくりに なつています"},
}};

struct Failure {
    std::string_view id;
    Words words;
};
constexpr std::array<Failure, 14> failures = {{
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
    {"delete_failed", {"THE WORLD COULD NOT BE DELETED", "ワールドをけせませんでした"}},
    {"other", {"COULD NOT JOIN THE ROOM", "さんかできませんでした"}},
}};

std::string failure_text(std::string_view id, Language language) {
    const auto found = std::ranges::find(failures, id, &Failure::id);
    return std::string((found != failures.end() ? found->words : failures.back().words).in(language));
}

std::string floor_text(std::uint32_t floor, Language language) {
    return language == Language::English ? "FLOOR " + std::to_string(floor) : std::to_string(floor) + "かい";
}

// A kept world's name in a list: who else has played in it, as the world has no other. A long first
// name stands alone, as "AND 3" after it would run into the value's box.
std::string world_label(const verdite::TitleWorld &world, Language language) {
    constexpr std::size_t name_most_before_and = 7;
    if (world.others.empty())
        return std::string(solo_word.in(language));
    const std::string &first = world.others.front();
    if (world.others.size() == 1 || first.size() > name_most_before_and)
        return first;
    return first + std::string(and_word.in(language)) + std::to_string(world.others.size() - 1);
}

std::string room_kind_line(const verdite::TitleWorld &world, Language language) {
    if (verdite::room_code_is_public(world.code))
        return std::string(public_word.in(language));
    return std::string(private_word.in(language)) + " " + world.code;
}

std::string others_line(const verdite::TitleWorld &world, Language language) {
    constexpr std::size_t names_most_letters = 22;   // what fits beside WITH and AND
    const std::size_t fit = verdite::title_names_that_fit(world.others, names_most_letters);
    std::string line(with_word.in(language));
    for (std::size_t i = 0; i < fit; ++i)
        line += (i == 0 ? "" : " ") + world.others[i];
    if (fit < world.others.size())
        line += std::string(and_word.in(language)) + std::to_string(world.others.size() - fit);
    return line;
}

std::string played_text(std::uint64_t played_s, Language language) {
    const verdite::TitleTime played = verdite::title_time_played(played_s);
    const Words &unit = played.unit == verdite::TitleTimeUnit::Minutes ? played_minutes_word : played_hours_word;
    return std::string(played_word.in(language)) + std::to_string(played.count) + std::string(unit.in(language));
}

std::string ago_text(std::uint64_t elapsed_s, Language language) {
    const verdite::TitleTime ago = verdite::title_time_ago(elapsed_s);
    if (ago.unit == verdite::TitleTimeUnit::Minutes && ago.count == 0)
        return std::string(just_now_word.in(language));
    const std::string count = std::to_string(ago.count);
    if (language != Language::English) {
        const std::string_view unit = ago.unit == verdite::TitleTimeUnit::Minutes ? "ふん" : ago.unit == verdite::TitleTimeUnit::Hours ? "じかん" : "にち";
        return count + std::string(unit) + std::string(ago_word.in(language));
    }
    const std::string_view unit = ago.unit == verdite::TitleTimeUnit::Minutes ? " MINUTE" : ago.unit == verdite::TitleTimeUnit::Hours ? " HOUR" : " DAY";
    return count + std::string(unit) + (ago.count == 1 ? "" : "S") + std::string(ago_word.in(language));
}

std::string times_line(const verdite::TitleWorld &world, Language language) {
    const auto now = static_cast<std::uint64_t>(std::time(nullptr));
    const std::uint64_t elapsed_s = now > world.last_played_s ? now - world.last_played_s : 0;
    return played_text(world.played_s, language) + "  " + ago_text(elapsed_s, language);
}

// Only what the world's file knows and its row does not already say.
std::vector<std::string> world_details(const verdite::TitleWorld &world, Language language) {
    std::vector<std::string> lines{room_kind_line(world, language)};
    if (world.saved_floor != 0)
        lines.push_back(std::string(saved_word.in(language)) + floor_text(world.saved_floor, language));
    if (world.others.size() > 1)
        lines.push_back(others_line(world, language));
    if (world.last_played_s != 0)
        lines.push_back(times_line(world, language));
    return lines;
}

std::vector<std::string> room_settings_details(Language language) {
    std::vector<std::string> lines;
    for (const verdite::TitleModSetting &setting : title_coop_settings(TitleCoopRows::Room, language))
        if (lines.size() < title_details_most)
            lines.push_back(setting.label + "  " + setting.value);
    return lines;
}

template <std::size_t N>
std::vector<std::string> spelled(const std::array<Words, N> &words, Language language) {
    std::vector<std::string> lines;
    for (const Words &line : words)
        lines.emplace_back(line.in(language));
    return lines;
}

constexpr std::array<Words, 1> new_world_details = {{{"PRIVATE OR PUBLIC", "ひこうかい こうかい"}}};
constexpr std::array<Words, 1> host_options_details = {{{"SETTINGS AND DELETE", "せつていと けす"}}};
constexpr std::array<Words, 1> new_private_details = {{{"FRIENDS JOIN BY CODE", "コードでさんかします"}}};
constexpr std::array<Words, 2> new_public_details = {{
    {"ANYONE CAN JOIN", "だれでもさんかできます"},
    {"STARTS AT ONCE", "すぐにはじまります"},
}};
constexpr std::array<Words, 1> delete_worlds_details = {{{"REMOVE A WORLD", "ワールドをけします"}}};

bool page_has_details(verdite::TitlePage page) {
    using verdite::TitlePage;
    return page == TitlePage::Host || page == TitlePage::NewWorld || page == TitlePage::HostOptions ||
           page == TitlePage::DeleteWorld || page == TitlePage::DeleteWarning;
}

// What the panel under the rows says about the selected row; nothing on a page without one.
std::vector<std::string> details(const verdite::TitleView &view, Language language) {
    using verdite::TitleRowKind;
    const auto on = std::ranges::find(view.rows, true, &verdite::TitleRowView::selected);
    if (!page_has_details(view.page) || on == view.rows.end())
        return {};
    if (view.page == verdite::TitlePage::DeleteWarning)
        return view.rows.front().world ? world_details(*view.rows.front().world, language) : std::vector<std::string>{};
    switch (on->kind) {
    case TitleRowKind::KeptWorld:
    case TitleRowKind::WorldToDelete: return world_details(*on->world, language);
    case TitleRowKind::NewWorld: return spelled(new_world_details, language);
    case TitleRowKind::HostOptions: return spelled(host_options_details, language);
    case TitleRowKind::NewPrivateWorld: return spelled(new_private_details, language);
    case TitleRowKind::NewPublicWorld: return spelled(new_public_details, language);
    case TitleRowKind::RoomSettings: return room_settings_details(language);
    case TitleRowKind::DeleteWorlds: return spelled(delete_worlds_details, language);
    default: return {};
    }
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
    case TitleRowKind::NewWorld: return std::string(new_world_word.in(language));
    case TitleRowKind::HostOptions: return std::string(options_word.in(language));
    case TitleRowKind::NewPrivateWorld: return std::string(private_word.in(language));
    case TitleRowKind::NewPublicWorld: return std::string(public_word.in(language));
    case TitleRowKind::KeptWorld:
    case TitleRowKind::WorldToDelete: return world_label(*row.world, language);
    case TitleRowKind::DeleteWorlds:
    case TitleRowKind::DeleteAnyway: return std::string(delete_word.in(language));
    case TitleRowKind::Start: return std::string(start_word.in(language));
    case TitleRowKind::Letter: return std::string(1, row.letter);
    case TitleRowKind::Delete: return std::string(delete_word.in(language));
    case TitleRowKind::Join: return std::string(join_word.in(language));
    case TitleRowKind::Room: return row.room->host;
    case TitleRowKind::Profile: return std::string(profile_word.in(language));
    case TitleRowKind::ProfileName: return std::string(name_word.in(language));
    case TitleRowKind::ProfileColour: return std::string(colour_word.in(language));
    case TitleRowKind::ProfileHelmet: return std::string(helmet_word.in(language));
    case TitleRowKind::ProfileSymbol: return std::string(symbol_word.in(language));
    case TitleRowKind::Done:
    case TitleRowKind::ProfileDone: return std::string(done_word.in(language));
    case TitleRowKind::CoopIntroNext: return std::string(next_word.in(language));
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
    case TitleRowKind::ProfileSymbol:
        return profile.symbol < online_player_symbol_count ? profile_words(coop_symbol_names()[profile.symbol], language)
                                                           : std::string();
    case TitleRowKind::KeptWorld:
    case TitleRowKind::WorldToDelete: return floor_text(row.world->highest_floor, language);
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
    case TitlePage::Coop:
    case TitlePage::CoopIntro: return std::string(coop_word.in(language));
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
    case TitlePage::CoopWarning:
    case TitlePage::DeleteWarning: return std::string(warning_word.in(language));
    case TitlePage::DeleteWorld: return std::string(delete_word.in(language));
    case TitlePage::NewWorld: return std::string(new_world_word.in(language));
    case TitlePage::HostOptions: return std::string(options_word.in(language));
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
    case TitlePage::DeleteWarning: return std::string(delete_note_word.in(language));
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
    text.pinned_rows = static_cast<u32>(view.pinned_rows);
    text.details = ext::details(view, language);
    text.note = ext::note(view, language);
    if (view.page == verdite::TitlePage::CoopIntro)
        for (const ext::Words &line : ext::coop_intro_lines)
            text.lines.emplace_back(line.in(language));
    if (view.page == verdite::TitlePage::HostCode || view.page == verdite::TitlePage::JoinCode)
        text.code = view.code;
    if (view.page == verdite::TitlePage::NameEntry)
        text.code = view.code;
    if (view.page == verdite::TitlePage::Profile)
        text.helmet = TitleHelmet{static_cast<u8>(view.profile.helmet), static_cast<u8>(view.profile.symbol), static_cast<u8>(view.profile.colour)};
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
