// The coop world (docs/COOP_PLAN.md), played in a room: whoever is in it first hosts the world kept
// under its code, and everyone who joins plays in it with their own character. The host keeps the
// party (one pack and one purse, every character, what each wears) and writes the world's file;
// a guest's game changes the pack at once and the host settles the difference. Joining sets the
// player's own game aside and leaving puts it back, both between updates. When every player is down,
// the host sends the party back to its last save (coop_death_play.h).
#include "coop_death_play.h"
#include "coop_floor_play.h"
#include "coop_hits.h"
#include "coop_message.h"
#include "coop_party.h"
#include "coop_profile.h"
#include "internal.hpp"
#include "online_sounds.h"

#include <kf/game/player.h>
#include <kf/game/save.h>
#include <kf/lib/item.h>
#include <kf/lib/map.h>
#include <kf/platform/automap.h>
#include <kf/platform/coop.h>
#include <kf/platform/host.h>
#include <kf/platform/input.h>
#include <kf/platform/saves.h>
#include <verdite/pickup_feed.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace kf::ext {
namespace {

constexpr std::uint64_t character_period_ms = 1000;   // a character's state goes to the host at most this often
constexpr std::uint64_t file_period_ms = 5000;        // the host writes the world's file at most this often

enum class Role { None, Host, Guest };

// What the game is to be replaced with at the next point between updates.
struct Entering {
    std::vector<std::uint8_t> character;   // empty: a new game's character
    std::vector<std::uint8_t> world;       // empty: a new world
    CoopItems pack{};
    std::uint32_t gold = 0;
    CoopWorn worn = coop_nothing_worn;
    std::optional<std::array<std::uint8_t, 3>> beside;   // floor, cell x, cell z: arrive there
};

struct HostSide {
    CoopWorldFile file;
    std::map<std::uint64_t, CoopPlayerId> guests;   // peers that said hello
    std::map<CoopPlayerId, CoopProfile> profiles;   // as each said it, kept after they leave
    std::map<CoopPlayerId, std::uint32_t> applied_through, worn_through;
    std::vector<std::pair<std::uint64_t, CoopHello>> waiting;   // hellos to answer between updates
    std::array<std::uint64_t, coop_guest_slots> slots{};        // a guest's peer, kept while it stays
    std::set<CoopPlayerId> sent_away;                           // refused for the rest of this session
    bool pack_changed = false;
    bool file_changed = false;
    std::uint64_t written_ms = 0;
    std::uint64_t played_until_ms = 0;   // how far file.played_ms has counted
};

struct GuestSide {
    std::uint64_t room_host = 0;   // the room's first peer: the only one asked for a welcome, and heard
    std::uint64_t host = 0;
    std::optional<CoopWelcome> welcome;   // until it is in the game
    std::optional<CoopWipe> wipe;         // until it is in the game
    std::optional<CoopPack> pack;         // the newest, until it is in the game
    std::vector<CoopPack::Player> players;
    int highest_floor = 1;
    std::uint32_t character_number = 0;
    std::uint32_t wear_number = 0;
    CoopRoster roster;   // as the host last sent it
    std::optional<CoopRules> rules;   // as the host last sent them
};

struct Coop {
    Role role = Role::None;
    std::string room;
    CoopPlayerId self = 0;
    CoopProfile profile;
    bool in_play = false;    // the coop world is the game in play
    bool from_title = false; // no game was set aside: leaving goes back to the title
    bool entering = false;   // to set this game aside and put the coop world in
    bool leaving = false;    // to put this game back
    Entering enter;
    CoopPackMirror mirror;   // the game's pack and purse against the party's
    PackCause cause = PackCause::Found;
    std::vector<std::uint8_t> character_sent;
    std::uint64_t character_sent_ms = 0;
    CoopWorn worn_sent = coop_nothing_worn;
    HostSide host;
    GuestSide guest;
    verdite::PickupFeed feed{verdite::PickupFeedShape{.most_lines = 3, .shown_ms = 3000}};
    std::vector<CoopPlayerId> feed_players;   // a feed line's player is an index into this
    bool looked_for_aside = false;            // once a run, for a game a quit left set aside
    std::uint32_t life = 0;                   // how many times the party went back to its last save
    std::map<CoopPlayerId, std::uint32_t> lives;   // each other player's, once a wipe makes it matter
};
Coop coop;

std::uint64_t now_ms() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// An install's id, kept in the user folder, so a player is the same character in every session.
CoopPlayerId install_id() {
    const auto path = user_dir() / "coop.id";
    unsigned long long id = 0;
    if (std::ifstream in(path); in && (in >> std::hex >> id) && id != 0)
        return id;
    std::random_device random;
    while (id == 0)
        id = (static_cast<unsigned long long>(random()) << 32) | random();
    std::ofstream out(path);
    out << std::hex << id << '\n';
    if (!out)
        std::printf("[coop] cannot keep this player's id in %s\n", path.string().c_str());
    return id;
}

// This player's profile, chosen on the title, beside the install's id.
std::filesystem::path profile_path() {
    return user_dir() / "coop.profile";
}

void keep_own_profile() {
    std::ofstream out(profile_path(), std::ios::binary);
    out << coop_profile_text(coop.profile);
    if (!out)
        std::printf("[coop] cannot keep this player's profile in %s\n", profile_path().string().c_str());
}

CoopProfile read_own_profile(CoopPlayerId id) {
    std::ifstream in(profile_path(), std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return coop_profile_read(text, coop_profile_default(id));
}

// Anyone's in the room: as they said it, else the first an install has.
CoopProfile profile_of(CoopPlayerId id) {
    if (id == coop.self)
        return coop.profile;
    if (const auto said = coop.host.profiles.find(id); said != coop.host.profiles.end())
        return said->second;
    for (const CoopRoster::Player &player : coop.guest.roster.players)
        if (player.id == id)
            return player.profile;
    return coop_profile_default(id);
}

CoopSavePoint standing_here() {
    const auto &cell = player_state.motion_state.map_cell;
    return {kf_enum_encode<int>(player_state.progress_state.current_floor), static_cast<std::uint8_t>(cell.x),
        static_cast<std::uint8_t>(cell.z)};
}

void know_self() {
    if (coop.self != 0)
        return;
    coop.self = install_id();
    coop.profile = read_own_profile(coop.self);
}

void send(std::uint64_t peer, const CoopMessage &message) {
    if (const auto bytes = coop_message_encode(message))
        online_send_control(peer, *bytes);
    else
        std::printf("[coop] a message too big to send was dropped\n");
}

void broadcast(const CoopMessage &message) {
    if (const auto bytes = coop_message_encode(message))
        online_broadcast_control(*bytes);
}

// Who is in the room and their peers, the host first, to everyone: a snapshot goes straight from
// one player to another.
void host_roster() {
    CoopRoster roster{{{coop.self, online_self_peer(), coop.profile}}};
    for (const auto &[peer, id] : coop.host.guests)
        roster.players.push_back({id, peer, profile_of(id)});
    broadcast(roster);
    coop_floors_roster(roster);
}

// ---- the game's side ----

// The disc's items only: mods are off in coop, and a mod's items stay where they lie in the stock.
CoopItems game_pack() {
    const auto &stock = item_stock[kf_enum_encode<u8>(KF_ITEM_STOCK_PLAYER)];
    CoopItems pack{};
    std::copy_n(stock.begin(), pack.size(), pack.begin());
    return pack;
}

void put_pack_in_game(const CoopItems &pack, std::uint32_t gold) {
    std::ranges::copy(pack, item_stock[kf_enum_encode<u8>(KF_ITEM_STOCK_PLAYER)].begin());
    player_state.gold = gold;
}

CoopWorn game_worn() {
    const auto &p = player_state;
    return {kf_enum_encode<u8>(p.equipped_weapon_id), kf_enum_encode<u8>(p.equipped_head_armor_id),
        kf_enum_encode<u8>(p.equipped_body_armor_id), kf_enum_encode<u8>(p.equipped_shield_id),
        kf_enum_encode<u8>(p.equipped_arm_armor_id), kf_enum_encode<u8>(p.equipped_leg_armor_id),
        kf_enum_encode<u8>(p.equipped_accessory_id)};
}

// The worn slots after the weapon, in CoopWorn's order.
constexpr std::array<KfEquipmentSlot, coop_worn_slots - 1> armor_slots = {KF_EQUIPMENT_SLOT_HEAD,
    KF_EQUIPMENT_SLOT_BODY, KF_EQUIPMENT_SLOT_SHIELD, KF_EQUIPMENT_SLOT_ARM, KF_EQUIPMENT_SLOT_LEG,
    KF_EQUIPMENT_SLOT_ACCESSORY};

void put_on(const CoopWorn &worn) {
    const CoopWorn now = game_worn();
    if (worn[0] != now[0])
        player_equip_weapon(kf_enum_decode<KfObjectId>(worn[0]));
    for (std::size_t slot = 1; slot < coop_worn_slots; ++slot)
        if (worn[slot] != now[slot])
            player_set_equipment_slot(kf_enum_decode<KfObjectId>(worn[slot]), armor_slots[slot - 1]);
}

void stand_beside(const std::array<std::uint8_t, 3> &beside) {
    const auto floor = kf_enum_decode<KfFloorId>(beside[0]);
    if (player_state.progress_state.current_floor != floor)
        player_state.map_variant = KF_MAP_VARIANT_DEFAULT;
    player_state.progress_state.current_floor = floor;
    player_state.camera_position.vx = beside[1] * KF_MAP_TILE_SIZE + KF_MAP_TILE_CENTER;
    player_state.camera_position.vz = beside[2] * KF_MAP_TILE_SIZE + KF_MAP_TILE_CENTER;
}

// player_replace_game's load: the coop world, as coop.enter holds it.
bool load_coop_world() {
    const Entering &enter = coop.enter;
    if (enter.character.empty() || enter.world.empty())
        game_state_start_new();
    else
        automap_reset();   // the coop world's map is not this game's
    const auto character = enter.character.empty() ? save_system_coop_character() : enter.character;
    const auto world = enter.world.empty() ? save_system_coop_world() : enter.world;
    if (!save_system_coop_apply(character, world, enter.pack, enter.gold))
        return false;
    // A character kept dying or down comes back whole and standing, as it would beside another
    // player: its state was taken at any moment, mid-fade or mid-hit too.
    if (player_state.vitals.current_hp == 0) {
        player_state.vitals.current_hp = player_state.vitals.maximum_hp;
        player_state.vitals.current_mp = player_state.vitals.maximum_mp;
    }
    player_state.update_state = KF_PLAYER_UPDATE_NORMAL;
    player_state.death_visual_blend = 0;
    if (enter.beside)
        stand_beside(*enter.beside);
    put_on(enter.worn);
    return true;
}

// ---- joining and leaving, between updates ----

void reset_session() {
    const CoopPlayerId self = coop.self;
    const CoopProfile profile = coop.profile;
    const bool looked_for_aside = coop.looked_for_aside;
    coop = Coop{};
    coop.self = self;
    coop.profile = profile;
    coop.looked_for_aside = looked_for_aside;
    apply_control_settings();   // a guest's tick rate is its own again
}

// A game set aside by a run that quit or crashed in a room comes back in the next, before any
// room is entered, which would set this one aside over it.
void put_back_leftover_game() {
    std::array<u8, save_card_bytes> card{};
    if (save_card_read(SaveCard::Aside, card) != SaveFileResult::Ok)
        return;
    if (!player_replace_game(save_system_aside_read)) {
        std::printf("[coop] a game set aside in a room could not be put back; it is still in its file\n");
        return;
    }
    save_card_remove(SaveCard::Aside);
    host_show_message("Your game from before the coop session is back.");
    std::printf("[coop] put back the game set aside in a room\n");
}

void fail(const char *why) {
    std::printf("[coop] %s\n", why);
    host_show_message(why);
    online_leave_room(why);
    if (coop.from_title)
        title_return();
}

std::optional<CoopWorldFile> read_world_file() {
    std::vector<u8> bytes;
    const auto result = coop_world_read(coop.room, bytes);
    if (result == SaveFileResult::Missing)
        return CoopWorldFile{};
    if (result != SaveFileResult::Ok)
        return std::nullopt;
    return coop_world_decode(bytes);
}

void keep_live_world() {
    map_world_state_persist();
    coop.host.file.live.world = save_system_coop_world();
}

void note_time_played(HostSide &host) {
    using namespace std::chrono;
    const std::uint64_t now = now_ms();
    host.file.played_ms += now - host.played_until_ms;
    host.played_until_ms = now;
    host.file.last_played_s = static_cast<std::uint64_t>(duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
}

void write_world_file() {
    HostSide &host = coop.host;
    keep_live_world();
    note_time_played(host);
    const auto bytes = coop_world_encode(host.file);
    const auto result = coop_world_write(coop.room, bytes);
    if (result != SaveFileResult::Ok)
        std::printf("[coop] cannot write the world of room %s (%d)\n", coop.room.c_str(), static_cast<int>(result));
    host.file_changed = false;
    host.written_ms = now_ms();
}

bool set_game_aside() {
    map_world_state_persist();
    return save_system_aside_write();
}

// The coop world, just put in the game: who runs which floor, deaths, and the pack start afresh.
void start_playing() {
    if (coop.role == Role::Host) {
        coop_floors_start(coop.self, coop.self);
        host_roster();
    } else {
        coop_floors_start(coop.self, 0);
        coop_floors_roster(coop.guest.roster);
    }
    coop_death_start(coop.self);
    coop.mirror = CoopPackMirror(coop.enter.pack, coop.enter.gold);
    coop.worn_sent = game_worn();
    coop.character_sent.clear();
    coop.enter = {};
}

void stop_playing() {
    coop_floors_stop();
    coop_hits_stop();
    coop_death_stop();
}

void enter_world() {
    if (!coop.from_title && !set_game_aside()) {
        fail("This game could not be set aside, so the room was left.");
        return;
    }
    if (!player_replace_game(load_coop_world)) {
        if (!coop.from_title)
            player_replace_game(save_system_aside_read);
        fail("The coop world could not be loaded, so the room was left.");
        return;
    }
    coop.in_play = true;
    start_playing();
    std::printf("[coop] playing the world of room %s\n", coop.room.c_str());
}

// The coop world put in again over the one in play, with nothing set aside: the party back at its
// last save, as coop.enter holds it.
void enter_again() {
    stop_playing();
    if (!player_replace_game(load_coop_world)) {
        fail("The party's last save could not be loaded, so the room was left.");
        return;
    }
    start_playing();
}

// What each other player in the room says counts again only once it says it is back at the save
// too (CoopWiped): until then, what it said before is still on its way.
void others_not_yet_back() {
    const auto room = coop_floors_room();
    const CoopPlayerId host = room.empty() ? 0 : room.front().id;
    for (const CoopRoster::Player &player : room)
        if (player.id != coop.self && player.id != host)
            coop.lives.try_emplace(player.id, coop.life);
}

void enter_as_host() {
    auto file = read_world_file();
    if (!file) {
        fail("This room's coop world cannot be read; it was left as it is.");
        return;
    }
    HostSide &host = coop.host;
    host.file = std::move(*file);
    host.played_until_ms = now_ms();
    const CoopCharacter character = host.file.live.party.join(coop.self, coop.profile.name);
    CoopParty &party = host.file.live.party;
    coop.enter = {character.state, host.file.live.world, party.pack(), party.gold(), character.wore, std::nullopt};
    enter_world();
    if (!coop.in_play)
        return;
    // A wipe always has a save to go back to: a world never saved goes back to how it was opened,
    // and one kept before save points were stands the party where it was opened.
    if (!host.file.last_save) {
        party.store(coop.self, save_system_coop_character(), kf_enum_encode<u8>(player_state.progress_state.highest_floor));
        keep_live_world();
        host.file.last_save = host.file.live;
    }
    if (!host.file.save_point)
        host.file.save_point = standing_here();
    host.pack_changed = true;
    write_world_file();
}

void enter_as_guest(const CoopWelcome &welcome) {
    coop.guest.highest_floor = welcome.highest_floor;
    coop.enter = {welcome.state, welcome.world, welcome.pack, welcome.gold, welcome.worn,
        std::array<std::uint8_t, 3>{static_cast<std::uint8_t>(welcome.floor), welcome.cell_x, welcome.cell_z}};
    enter_world();
}

void leave_world() {
    stop_playing();
    // Out of the world before the aside game is read, so the mods take back its save tables.
    coop.in_play = false;
    std::printf("[coop] left the world of room %s\n", coop.room.c_str());
    if (coop.from_title) {
        reset_session();
        title_return();
        return;
    }
    if (player_replace_game(save_system_aside_read))
        save_card_remove(SaveCard::Aside);
    else
        host_show_message("Your own game could not be put back; it is still in its file.");
    reset_session();
}

// ---- the pack, the character and the file, between updates ----

PackCause cause_of(const CoopPackChange &change) {
    if (coop.cause != PackCause::Found)
        return coop.cause;
    return change.count < 0 ? PackCause::Used : PackCause::Found;
}

// The others' changes only: this player's own the game already shows, or this player made in a menu.
void feed(CoopPlayerId by, CoopCause cause, const CoopPackChange &change) {
    if (by == coop.self)
        return;
    auto known = std::ranges::find(coop.feed_players, by);
    if (known == coop.feed_players.end())
        known = coop.feed_players.insert(known, by);
    const auto player = static_cast<std::uint32_t>(known - coop.feed_players.begin());
    const std::uint16_t item = change.item == coop_no_item ? 0xffff : change.item;
    const auto count = static_cast<std::uint32_t>(change.count < 0 ? -std::int64_t{change.count} : change.count);
    coop.feed.add({player, static_cast<std::uint8_t>(cause), item, count}, now_ms());
}

void host_takes(CoopPlayerId by, std::uint32_t number, CoopPackChange change, CoopCause cause) {
    HostSide &host = coop.host;
    const std::int32_t applied = host.file.live.party.apply(by, change);
    if (by != coop.self)
        host.applied_through[by] = number;
    host.pack_changed = true;
    host.file_changed = true;
    if (applied == 0)
        return;
    const CoopFeedLine line{by, cause, {change.item, applied}};
    feed(line.by, line.cause, line.change);
    broadcast(line);
}

// The game's own changes to the pack since the last look: to the party on the host, to the host
// from a guest.
void take_game_changes() {
    const auto changes = coop.mirror.take_changes(game_pack(), player_state.gold);
    for (const auto &sent : changes) {
        const auto cause = static_cast<CoopCause>(cause_of(sent.change));
        if (coop.role == Role::Host)
            host_takes(coop.self, sent.number, sent.change, cause);
        else
            send(coop.guest.host, CoopPackRequest{sent.number, sent.change, cause});
    }
    if (!changes.empty())
        coop.cause = PackCause::Found;
}

CoopPack host_pack_message() {
    const CoopParty &party = coop.host.file.live.party;
    CoopPack message{party.pack(), party.gold(), party.highest_floor(), {}};
    for (const CoopPlayerId id : party.in_room()) {
        const auto character = std::ranges::find(party.characters(), id, &CoopCharacter::id);
        message.players.push_back({id, character->wore, coop.host.applied_through[id], coop.host.worn_through[id]});
    }
    return message;
}

void host_wear(CoopPlayerId id, std::uint32_t number, const CoopWorn &worn) {
    coop.host.file.live.party.wear(id, worn);
    coop.host.worn_through[id] = number;
    coop.host.pack_changed = true;
}

CoopRules host_rules() {
    return {effective_settings().tick_rate, settings().revive_wait_s};
}

void keep_guest_slot(std::uint64_t peer) {
    auto &slots = coop.host.slots;
    if (const auto free = std::ranges::find(slots, std::uint64_t{0}); free != slots.end())
        *free = peer;
}

void host_welcome(std::uint64_t peer, const CoopHello &hello) {
    HostSide &host = coop.host;
    map_world_state_persist();
    const CoopCharacter character = host.file.live.party.join(hello.id, hello.profile.name);
    host.guests[peer] = hello.id;
    host.profiles[hello.id] = hello.profile;
    const CoopParty &party = host.file.live.party;
    const auto &cell = player_state.motion_state.map_cell;
    send(peer, CoopWelcome{character.state, character.wore, party.pack(), party.gold(), party.highest_floor(),
        save_system_coop_world(), kf_enum_encode<u8>(player_state.progress_state.current_floor), cell.x, cell.z});
    send(peer, host_rules());
    keep_guest_slot(peer);
    host.pack_changed = true;
    host.file_changed = true;
    host_roster();
    std::printf("[coop] %s joined the world\n", hello.profile.name.c_str());
}

CoopWelcome start_at_save(const CoopCharacter &character) {
    const CoopParty &party = coop.host.file.live.party;
    const CoopSavePoint point = coop.host.file.save_point.value_or(standing_here());
    return {character.state, character.wore, party.pack(), party.gold(), party.highest_floor(),
        coop.host.file.live.world, point.floor, point.cell_x, point.cell_z};
}

// Every player is down: the live world becomes the last save, every character as saved (one who
// joined since keeps what it has), everyone in the room wearing what of their gear is free, and
// each player's game is put back at the save point.
void host_wipe() {
    HostSide &host = coop.host;
    const CoopWorldState saved = host.file.last_save.value_or(host.file.live);
    const CoopParty &before = host.file.live.party;
    std::vector<CoopCharacter> characters(saved.party.characters().begin(), saved.party.characters().end());
    for (const CoopCharacter &character : before.characters())
        if (std::ranges::find(characters, character.id, &CoopCharacter::id) == characters.end())
            characters.push_back(character);
    const std::vector<CoopPlayerId> in_room(before.in_room().begin(), before.in_room().end());
    CoopParty party(characters, saved.party.pack(), saved.party.gold());
    for (const CoopPlayerId id : in_room)
        party.join(id, std::ranges::find(characters, id, &CoopCharacter::id)->name);
    host.file.live = {std::move(party), saved.world};
    host.applied_through.clear();
    host.worn_through.clear();
    others_not_yet_back();
    ++coop.life;
    const CoopParty &now = host.file.live.party;
    for (const auto &[peer, id] : host.guests)
        send(peer, CoopWipe{coop.life, start_at_save(*std::ranges::find(now.characters(), id, &CoopCharacter::id))});
    const CoopWelcome own = start_at_save(*std::ranges::find(now.characters(), coop.self, &CoopCharacter::id));
    coop.enter = {own.state, own.world, own.pack, own.gold, own.worn,
        std::array<std::uint8_t, 3>{static_cast<std::uint8_t>(own.floor), own.cell_x, own.cell_z}};
    enter_again();
    if (!coop.in_play)
        return;
    host.pack_changed = true;
    write_world_file();
    host_show_message("Everyone fell: the party is back at its last save.");
    std::printf("[coop] the party went back to its last save (%u)\n", coop.life);
}

// Whether every player is down, each since the last wipe.
bool party_down() {
    std::vector<CoopPlayerId> not_back;
    for (const auto &[id, life] : coop.lives)
        if (life < coop.life)
            not_back.push_back(id);
    return coop_death_party_down(not_back);
}

void guest_wipe(const CoopWipe &wipe) {
    others_not_yet_back();
    coop.life = wipe.life;
    coop.guest.highest_floor = wipe.start.highest_floor;
    coop.guest.pack.reset();
    coop.enter = {wipe.start.state, wipe.start.world, wipe.start.pack, wipe.start.gold, wipe.start.worn,
        std::array<std::uint8_t, 3>{static_cast<std::uint8_t>(wipe.start.floor), wipe.start.cell_x, wipe.start.cell_z}};
    enter_again();
    if (!coop.in_play)
        return;
    broadcast(CoopWiped{coop.life});
    host_show_message("Everyone fell: the party is back at its last save.");
    std::printf("[coop] back at the party's last save (%u)\n", coop.life);
}

void between_updates_as_host() {
    if (party_down()) {
        host_wipe();
        return;
    }
    HostSide &host = coop.host;
    for (const auto &[peer, hello] : host.waiting)
        host_welcome(peer, hello);
    host.waiting.clear();
    take_game_changes();
    const CoopWorn worn = game_worn();
    if (worn != coop.worn_sent) {
        host_wear(coop.self, 0, worn);
        coop.worn_sent = worn;
    }
    CoopParty &party = host.file.live.party;
    coop.mirror.receive(party.pack(), party.gold(), UINT32_MAX);
    put_pack_in_game(coop.mirror.pack(), coop.mirror.gold());
    const auto self = std::ranges::find(party.characters(), coop.self, &CoopCharacter::id);
    if (self != party.characters().end() && self->wore != worn)
        put_on(self->wore);   // a guest took it first
    coop.worn_sent = game_worn();
    if (host.pack_changed) {
        broadcast(host_pack_message());
        host.pack_changed = false;
    }
    const std::uint64_t now = now_ms();
    if (now - coop.character_sent_ms >= character_period_ms) {
        party.store(coop.self, save_system_coop_character(), kf_enum_encode<u8>(player_state.progress_state.highest_floor));
        coop.character_sent_ms = now;
        host.file_changed = true;
    }
    if (host.file_changed && now - host.written_ms >= file_period_ms)
        write_world_file();
}

void between_updates_as_guest() {
    GuestSide &guest = coop.guest;
    take_game_changes();
    if (guest.pack) {
        const CoopPack pack = std::move(*guest.pack);
        guest.pack.reset();
        guest.highest_floor = std::max(guest.highest_floor, pack.highest_floor);
        guest.players = pack.players;
        const auto self = std::ranges::find(pack.players, coop.self, &CoopPack::Player::id);
        const std::uint32_t through = self == pack.players.end() ? 0 : self->applied_through;
        coop.mirror.receive(pack.pack, pack.gold, through);
        put_pack_in_game(coop.mirror.pack(), coop.mirror.gold());
        // The host's answer to the last thing we asked to wear: anything not free comes off.
        if (self != pack.players.end() && self->worn_through == guest.wear_number && self->worn != game_worn()) {
            put_on(self->worn);
            coop.worn_sent = game_worn();
        }
    }
    const CoopWorn worn = game_worn();
    if (worn != coop.worn_sent) {
        send(guest.host, CoopWearRequest{++guest.wear_number, worn});
        coop.worn_sent = worn;
    }
    const std::uint64_t now = now_ms();
    if (now - coop.character_sent_ms < character_period_ms)
        return;
    auto character = save_system_coop_character();
    if (character != coop.character_sent) {
        send(guest.host, CoopCharacterUpdate{++guest.character_number, character,
            kf_enum_encode<u8>(player_state.progress_state.highest_floor)});
        coop.character_sent = std::move(character);
    }
    coop.character_sent_ms = now;
}

// A save point used by any player, at `at`: the whole live state becomes the last save.
void host_save(const CoopSavePoint &at) {
    HostSide &host = coop.host;
    host.file.live.party.store(coop.self, save_system_coop_character(),
        kf_enum_encode<u8>(player_state.progress_state.highest_floor));
    keep_live_world();
    host.file.last_save = host.file.live;
    host.file.save_point = at;
    write_world_file();
    host_show_message("The party is saved.");
}

// ---- messages ----

// Why a player saying hello is not let in, if it is not.
std::optional<CoopTurnedAwayWhy> refusal_of(CoopPlayerId id) {
    if (coop.host.sent_away.contains(id))
        return CoopTurnedAwayWhy::SentAway;
    const auto characters = coop.host.file.live.party.characters();
    const bool known = std::ranges::find(characters, id, &CoopCharacter::id) != characters.end();
    return settings().room_open || known ? std::nullopt : std::optional(CoopTurnedAwayWhy::RoomClosed);
}

void host_receives(std::uint64_t peer, const CoopMessage &message) {
    HostSide &host = coop.host;
    if (const auto *hello = std::get_if<CoopHello>(&message)) {
        if (hello->id == coop.self || host.guests.contains(peer))
            return;
        if (const auto refused = refusal_of(hello->id))
            send(peer, CoopTurnedAway{*refused});
        else
            host.waiting.emplace_back(peer, *hello);
        return;
    }
    const auto guest = host.guests.find(peer);
    if (guest == host.guests.end())
        return;
    const CoopPlayerId id = guest->second;
    if (const auto *update = std::get_if<CoopCharacterUpdate>(&message)) {
        host.file.live.party.store(id, update->state, update->highest_floor);
        host.file_changed = true;
        host.pack_changed = true;   // the highest floor may have risen
        send(peer, CoopAcknowledged{update->number});
    } else if (const auto *request = std::get_if<CoopPackRequest>(&message)) {
        host_takes(id, request->number, request->change, request->cause);
    } else if (const auto *wear = std::get_if<CoopWearRequest>(&message)) {
        host_wear(id, wear->number, wear->worn);
    } else if (const auto *save = std::get_if<CoopSaveRequest>(&message)) {
        host_save(save->at);
    }
}

void guest_turned_away(CoopTurnedAwayWhy why) {
    const bool sent_away = why == CoopTurnedAwayWhy::SentAway;
    const char *text = sent_away ? "The host sent you away." : "The host is not letting new players in.";
    std::printf("[coop] %s\n", text);
    host_show_message(text);
    online_turned_away(text, sent_away ? OnlineFailure::SentAway : OnlineFailure::RoomClosed);
}

void guest_receives(std::uint64_t peer, const CoopMessage &message) {
    GuestSide &guest = coop.guest;
    if (const auto *away = std::get_if<CoopTurnedAway>(&message)) {
        if (peer == guest.room_host)
            guest_turned_away(away->why);
        return;
    }
    if (const auto *welcome = std::get_if<CoopWelcome>(&message)) {
        if (guest.host != 0 || coop.in_play || peer != guest.room_host)
            return;
        guest.host = peer;
        guest.welcome = *welcome;
        coop.entering = true;
        return;
    }
    if (peer != guest.host)
        return;
    if (const auto *wipe = std::get_if<CoopWipe>(&message)) {
        if (wipe->life > coop.life)
            guest.wipe = *wipe;
        return;
    }
    if (const auto *rules = std::get_if<CoopRules>(&message)) {
        guest.rules = *rules;
        apply_control_settings();
    } else if (const auto *roster = std::get_if<CoopRoster>(&message)) {
        guest.roster = *roster;
        coop_floors_roster(*roster);
    } else if (const auto *pack = std::get_if<CoopPack>(&message)) {
        guest.pack = *pack;
    }
    else if (const auto *line = std::get_if<CoopFeedLine>(&message))
        feed(line->by, line->cause, line->change);
}

// Whether a guest's message was said before it came back to the party's last save: the wipe made
// it of no account. The host's are never, since its wipe comes after everything it said before.
bool from_before_wipe(CoopPlayerId from) {
    const auto room = coop_floors_room();
    if (from == 0 || room.empty() || from == room.front().id)
        return false;
    const auto life = coop.lives.find(from);
    return life != coop.lives.end() && life->second < coop.life;
}

// Whether a message is about who runs which floor (coop_floor_play.h), from any player.
bool floor_message(const CoopMessage &message) {
    return std::holds_alternative<CoopFloorAt>(message) || std::holds_alternative<CoopFloorOrders>(message)
        || std::holds_alternative<CoopFloorRecord>(message) || std::holds_alternative<CoopFloorSnapshot>(message)
        || std::holds_alternative<CoopFloorTaken>(message) || std::holds_alternative<CoopFloorWrites>(message);
}

// What the others in the room wear: the host's party, or a guest's last word from the host.
std::vector<CoopWorn> others_worn() {
    std::vector<CoopWorn> worn;
    if (coop.role == Role::Host) {
        const CoopParty &party = coop.host.file.live.party;
        for (const CoopCharacter &character : party.characters())
            if (character.id != coop.self && std::ranges::find(party.in_room(), character.id) != party.in_room().end())
                worn.push_back(character.wore);
        return worn;
    }
    for (const CoopPack::Player &player : coop.guest.players)
        if (player.id != coop.self)
            worn.push_back(player.worn);
    return worn;
}

std::string worn_json(const CoopWorn &worn) {
    std::string text;
    for (const std::uint8_t item : worn)
        text += (text.empty() ? "" : ",") + std::to_string(item);
    return "[" + text + "]";
}

} // namespace

void coop_room_joined(std::string_view room, std::uint64_t host_peer) {
    const bool first = host_peer == 0;
    know_self();
    reset_session();
    coop.room = room;
    coop.role = first ? Role::Host : Role::Guest;
    coop.guest.room_host = host_peer;
    coop.entering = first;
    std::printf("[coop] in room %s as %s (%s)\n", coop.room.c_str(), coop.profile.name.c_str(), first ? "host" : "guest");
}

bool coop_ready_from_title() {
    return coop.role == Role::Host || (coop.role == Role::Guest && coop.guest.welcome);
}

void coop_start_from_title() {
    coop.from_title = true;
    coop.entering = true;
    // A game a crashed run left set aside stays in its file: putting it back now would only have
    // the coop world replace it.
    coop.looked_for_aside = true;
}

void coop_keep_leftover_aside() {
    coop.looked_for_aside = true;
}

CoopPlayerId coop_own_id() {
    return install_id();
}

CoopProfile coop_own_profile() {
    know_self();
    return coop.profile;
}

void coop_set_own_profile(const CoopProfile &profile) {
    know_self();
    if (!coop_profile_valid(profile) || profile == coop.profile)
        return;
    coop.profile = profile;
    keep_own_profile();
}

bool coop_own_profile_is_set_up() {
    std::error_code error;
    return std::filesystem::exists(profile_path(), error);
}

void coop_set_up_own_profile(const CoopProfile &profile) {
    know_self();
    if (coop_profile_valid(profile))
        coop.profile = profile;
    keep_own_profile();
}

std::uint64_t coop_room_host_peer() {
    return coop.role == Role::Guest ? coop.guest.room_host : 0;
}

CoopProfile coop_profile_of_peer(std::uint64_t peer) {
    for (const CoopRoster::Player &player : coop_floors_room())
        if (player.peer == peer)
            return player.profile;
    return coop_profile_default(peer);
}

void coop_peer_open(std::uint64_t peer) {
    if (coop.role == Role::Guest && peer == coop.guest.room_host && coop.guest.host == 0)
        send(peer, CoopHello{coop.self, coop.profile});
}

void coop_peer_closed(std::uint64_t peer) {
    if (coop.role == Role::Host) {
        const auto guest = coop.host.guests.find(peer);
        if (guest == coop.host.guests.end())
            return;
        const CoopPlayerId id = guest->second;
        coop.host.file.live.party.leave(id);
        coop.host.guests.erase(guest);
        if (const auto slot = std::ranges::find(coop.host.slots, peer); slot != coop.host.slots.end())
            *slot = 0;
        coop.host.pack_changed = true;
        coop.host.file_changed = true;
        host_roster();
        coop_floors_gone(id);
        return;
    }
    if (coop.role == Role::Guest && peer == coop.guest.host)
        online_leave_room("The host left, so the session ended.");
}

void coop_control_packet(std::uint64_t peer, std::span<const std::uint8_t> bytes) {
    const auto message = coop_message_decode(bytes);
    if (!message) {
        std::printf("[coop] a message that is not one was dropped\n");
        return;
    }
    const CoopPlayerId from = coop_floors_player(peer);
    if (const auto *wiped = std::get_if<CoopWiped>(&*message)) {
        if (from != 0)
            coop.lives[from] = std::max(coop.lives[from], wiped->life);
        return;
    }
    if (from_before_wipe(from))
        return;
    if (floor_message(*message))
        coop_floors_receive(peer, *message);
    else if (coop_hits_message(*message))
        coop_hits_receive(coop_floors_player(peer), *message);
    else if (coop.role == Role::Host)
        host_receives(peer, *message);
    else if (coop.role == Role::Guest)
        guest_receives(peer, *message);
}

void coop_room_left() {
    if (!coop.in_play) {
        reset_session();
        return;
    }
    // Written now: leaving may be the game quitting, with no update to come.
    if (coop.role == Role::Host) {
        coop.host.file.live.party.store(coop.self, save_system_coop_character(),
            kf_enum_encode<u8>(player_state.progress_state.highest_floor));
        write_world_file();
    }
    coop.leaving = true;
}

std::vector<verdite::PickupShown> coop_feed_lines() {
    return coop.feed.shown(now_ms());
}

std::string coop_player_name(std::uint32_t player) {
    return player < coop.feed_players.size() ? profile_of(coop.feed_players[player]).name : std::string();
}

std::string coop_player_name_of(std::uint64_t player) {
    return profile_of(player).name;
}

std::string coop_json() {
    const char *role = coop.role == Role::Host ? "host" : coop.role == Role::Guest ? "guest" : "none";
    std::string pack;
    const CoopItems items = game_pack();
    for (std::size_t item = 0; item < items.size(); ++item)
        if (items[item] != 0)
            pack += (pack.empty() ? "\"" : ",\"") + std::to_string(item) + "\":" + std::to_string(items[item]);
    std::string others;
    for (const CoopWorn &worn : others_worn())
        others += (others.empty() ? "" : ",") + worn_json(worn);
    std::string feed_rows;
    for (const CoopFeedRow &row : kf::coop_feed_view(Language::English))
        feed_rows += (feed_rows.empty() ? "[\"" : ",[\"") + row.before + "\"," + std::to_string(row.item) + ",\"" + row.after + "\"]";
    std::string made, heard;
    for (const std::uint8_t count : online_sounds_made())
        made += (made.empty() ? "" : ",") + std::to_string(count);
    for (const std::uint32_t count : online_sounds_heard())
        heard += (heard.empty() ? "" : ",") + std::to_string(count);
    std::string profiles;
    for (const CoopRoster::Player &player : coop_floors_room())
        profiles += (profiles.empty() ? "[\"" : ",[\"") + player.profile.name + "\"," + std::to_string(player.profile.colour) +
                    "," + std::to_string(player.profile.helmet) + "," + std::to_string(player.profile.symbol) + "]";
    return "\"role\":\"" + std::string(role) + "\",\"inPlay\":" + (coop.in_play ? "true" : "false") + ",\"name\":\"" +
           coop.profile.name + "\",\"colour\":" + std::to_string(coop.profile.colour) + ",\"helmet\":" +
           std::to_string(coop.profile.helmet) + ",\"symbol\":" + std::to_string(coop.profile.symbol) + ",\"roster\":[" + profiles + "],\"pack\":{" + pack + "},\"gold\":" + std::to_string(player_state.gold) + ",\"worn\":" +
           worn_json(game_worn()) + ",\"othersWear\":[" + others + "],\"feed\":[" + feed_rows + "],\"highestFloor\":" +
           std::to_string(kf::coop_highest_floor(kf_enum_encode<u8>(player_state.progress_state.highest_floor))) +
           ",\"floor\":" + std::to_string(kf_enum_encode<int>(player_state.progress_state.current_floor)) + ",\"cell\":[" +
           std::to_string(player_state.motion_state.map_cell.x) + "," + std::to_string(player_state.motion_state.map_cell.z) +
           "],\"floorPart\":\"" + std::string(coop_floors_part()) + "\"," + coop_hits_json() + "," +
           coop_death_json() + ",\"tickVblanks\":" + std::to_string(world_update_vblanks) + ",\"reviveWaitS\":" + std::to_string(coop_revive_wait_s()) +
           ",\"messageStone\":" + (messages_stone_offered() ? "true" : "false") + ",\"nameBarsDrawn\":" + std::to_string(online_name_bars_drawn()) + ",\"crownsDrawn\":" + std::to_string(online_crowns_drawn()) + ",\"compassDotsDrawn\":" + std::to_string(online_compass_dots_drawn()) + ",\"life\":" + std::to_string(coop.life) + ",\"soundsMade\":[" + made + "],\"soundsHeard\":[" + heard + "]";
}

bool coop_hosting() {
    return coop.role == Role::Host;
}

bool coop_follows_host() {
    return coop.role == Role::Guest;
}

void coop_room_settings_changed() {
    if (coop.role == Role::Host && coop.in_play)
        broadcast(host_rules());
}

std::optional<int> coop_room_tick_rate() {
    if (coop.role == Role::Guest && coop.guest.rules)
        return coop.guest.rules->tick_rate;
    return std::nullopt;
}

int coop_revive_wait_s() {
    if (coop.role == Role::Guest)
        return coop.guest.rules ? coop.guest.rules->revive_wait_s : CoopRules{}.revive_wait_s;
    return settings().revive_wait_s;
}

std::string coop_guest_name(std::size_t slot) {
    if (coop.role != Role::Host || slot >= coop.host.slots.size())
        return {};
    const auto guest = coop.host.guests.find(coop.host.slots[slot]);
    return guest == coop.host.guests.end() ? std::string() : profile_of(guest->second).name;
}

void coop_send_away(std::size_t slot) {
    if (coop.role != Role::Host || slot >= coop.host.slots.size())
        return;
    const std::uint64_t peer = coop.host.slots[slot];
    const auto guest = coop.host.guests.find(peer);
    if (guest == coop.host.guests.end())
        return;
    coop.host.sent_away.insert(guest->second);
    send(peer, CoopTurnedAway{CoopTurnedAwayWhy::SentAway});
    std::printf("[coop] sent %s away\n", profile_of(guest->second).name.c_str());
    coop_peer_closed(peer);   // at once: from here the party goes on without them
}

std::string coop_panel_line() {
    switch (coop.role) {
    case Role::None:
        return {};
    case Role::Host:
        return coop.in_play ? "Your coop world (" + coop.profile.name + ")" : "Opening your coop world...";
    case Role::Guest:
        return coop.in_play ? "Playing the host's coop world as " + coop.profile.name : "Asking the host to join...";
    }
    return {};
}

}

namespace kf {

bool coop_world_in_play() {
    return ext::coop.in_play;
}

int coop_worn_by_others(u8 item) {
    using namespace ext;
    if (!coop.in_play)
        return 0;
    int worn = 0;
    for (const CoopWorn &other : others_worn())
        worn += static_cast<int>(std::ranges::count(other, item));
    return worn;
}

namespace {

// A line's words in the menu font: English capitals, or Japanese kana without small kana.
struct FeedWords {
    std::array<const char *, 4> verbs;   // found, bought, sold, used: CoopCause's order
    const char *gold;
};
constexpr FeedWords english_words = {{"FOUND", "BOUGHT", "SOLD", "USED"}, "GOLD"};
constexpr FeedWords japanese_words = {{"てにいれました", "かいました", "うりました", "つかいました"}, "ゴールド"};

CoopFeedRow feed_row(const verdite::PickupShown &shown, Language language) {
    const verdite::PickupLine &line = shown.line;
    const bool english = language == Language::English;
    const FeedWords &words = english ? english_words : japanese_words;
    const std::string name = ext::coop_player_name(line.player);
    const std::string verb = words.verbs[std::min<std::size_t>(line.action, words.verbs.size() - 1)];
    const std::string count = std::to_string(line.count);
    const bool gold = line.item > 0xff;
    CoopFeedRow row{{}, gold ? u8{0xff} : static_cast<u8>(line.item), {}, shown.opacity};
    if (gold) {
        row.before = english ? name + " " + verb + " " + count + " " + words.gold
                             : name + " " + count + " " + words.gold + " " + verb;
    } else if (english) {
        row.before = name + " " + verb + " " + (line.count > 1 ? count + " " : "");
    } else {
        row.before = name + " ";
        row.after = (line.count > 1 ? " " + count : "") + " " + verb;
    }
    return row;
}

} // namespace

std::vector<CoopFeedRow> coop_feed_view(Language language) {
    std::vector<CoopFeedRow> rows;
    if (!ext::settings().pickup_feed)
        return rows;
    for (const verdite::PickupShown &shown : ext::coop_feed_lines())
        rows.push_back(feed_row(shown, language));
    return rows;
}

u8 coop_highest_floor(u8 own) {
    using namespace ext;
    if (!coop.in_play)
        return own;
    const int world = coop.role == Role::Host ? coop.host.file.live.party.highest_floor() : coop.guest.highest_floor;
    return static_cast<u8>(std::max<int>(own, world));
}

bool coop_backs_out_of_menu() {
    return ext::coop.in_play && player_state.vitals.current_hp == 0;
}

bool coop_player_died() {
    if (!ext::coop.in_play)
        return false;
    ext::coop_death_went_down();
    return true;
}

bool coop_player_down() {
    return ext::coop.in_play && ext::coop_death_down();
}

void coop_down_update(bool next) {
    if (ext::coop.in_play)
        ext::coop_death_down_update(next);
}

void coop_player_fought() {
    if (ext::coop.in_play)
        ext::coop_death_fought();
}

std::string coop_watching_text(Language language) {
    const std::string name = ext::coop_death_eyes_name();
    if (name.empty())
        return {};
    return language == Language::English ? "WATCHING " + name : name + " を みています";
}

bool coop_runs_floor() {
    return !ext::coop.in_play || ext::coop_floors_run_here();
}

void coop_floor_unloading() {
    if (ext::coop.in_play)
        ext::coop_floors_unloading();
}

bool coop_save_point() {
    using namespace ext;
    if (!coop.in_play)
        return false;
    if (coop.role == Role::Guest) {
        send(coop.guest.host, CoopCharacterUpdate{++coop.guest.character_number, save_system_coop_character(),
            kf_enum_encode<u8>(player_state.progress_state.highest_floor)});
        send(coop.guest.host, CoopSaveRequest{standing_here()});
        host_show_message("The party is saved.");
        return true;
    }
    host_save(standing_here());
    return true;
}

void coop_pack_cause(PackCause cause) {
    ext::coop.cause = cause;
}

void coop_update() {
    using namespace ext;
    if (!coop.in_play)
        return;
    // Sent at once, even from a menu; the host's answer comes into the game between updates.
    if (coop.role == Role::Guest)
        take_game_changes();
    coop_hits_world_update();
    coop_floors_world_update();
    coop_death_world_update();
}

void coop_between_updates() {
    using namespace ext;
    if (player_state.update_state == KF_PLAYER_UPDATE_RECOVERY_FADE)
        return;
    if (!coop.looked_for_aside) {
        coop.looked_for_aside = true;
        put_back_leftover_game();
    }
    if (coop.leaving) {
        leave_world();
        return;
    }
    if (coop.in_play && coop.guest.wipe) {
        const CoopWipe wipe = std::move(*coop.guest.wipe);
        coop.guest.wipe.reset();
        guest_wipe(wipe);
        return;
    }
    if (coop.entering) {
        coop.entering = false;
        if (coop.role == Role::Host) {
            enter_as_host();
        } else if (coop.guest.welcome) {
            const CoopWelcome welcome = std::move(*coop.guest.welcome);
            coop.guest.welcome.reset();
            enter_as_guest(welcome);
        }
        return;
    }
    if (!coop.in_play)
        return;
    if (coop.role == Role::Host)
        between_updates_as_host();
    else
        between_updates_as_guest();
    coop_floors_between_updates();
    if (coop.role == Role::Host && coop_floors_world_changed())
        coop.host.file_changed = true;
}

}
