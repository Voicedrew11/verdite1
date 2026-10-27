// The coop world (docs/COOP_PLAN.md), played in a room: whoever is in it first hosts the world kept
// under its code, and everyone who joins plays in it with their own character. The host keeps the
// party (one pack and one purse, every character, what each wears) and writes the world's file;
// a guest's game changes the pack at once and the host settles the difference. Joining sets the
// player's own game aside and leaving puts it back, both between updates.
#include "coop_floor_play.h"
#include "coop_hits.h"
#include "coop_message.h"
#include "coop_party.h"
#include "internal.hpp"

#include <kf/game/player.h>
#include <kf/game/save.h>
#include <kf/lib/item.h>
#include <kf/lib/map.h>
#include <kf/platform/automap.h>
#include <kf/platform/coop.h>
#include <kf/platform/host.h>
#include <kf/platform/saves.h>
#include <verdite/pickup_feed.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <optional>
#include <random>
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
    std::map<CoopPlayerId, std::uint32_t> applied_through, worn_through;
    std::vector<std::pair<std::uint64_t, CoopHello>> waiting;   // hellos to answer between updates
    bool pack_changed = false;
    bool file_changed = false;
    std::uint64_t written_ms = 0;
};

struct GuestSide {
    std::uint64_t host = 0;
    std::optional<CoopWelcome> welcome;   // until it is in the game
    std::optional<CoopPack> pack;         // the newest, until it is in the game
    std::vector<CoopPack::Player> players;
    int highest_floor = 1;
    std::uint32_t character_number = 0;
    std::uint32_t wear_number = 0;
    CoopRoster roster;   // as the host last sent it
};

struct Coop {
    Role role = Role::None;
    std::string room;
    CoopPlayerId self = 0;
    std::string name;
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
    verdite::PickupFeed feed{verdite::PickupFeedShape{}};
    std::vector<CoopPlayerId> feed_players;   // a feed line's player is an index into this
    bool looked_for_aside = false;            // once a run, for a game a quit left set aside
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

// Names are an install's until players can choose one.
std::string name_of(CoopPlayerId id) {
    char name[coop_name_most + 1];
    std::snprintf(name, sizeof name, "PLAYER%04u", static_cast<unsigned>(id % 10000));
    return name;
}

void know_self() {
    if (coop.self != 0)
        return;
    coop.self = install_id();
    coop.name = name_of(coop.self);
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
    CoopRoster roster{{{coop.self, online_self_peer()}}};
    for (const auto &[peer, id] : coop.host.guests)
        roster.players.push_back({id, peer});
    broadcast(roster);
    coop_floors_roster(roster);
}

// ---- the game's side ----

CoopItems game_pack() {
    return item_stock[kf_enum_encode<u8>(KF_ITEM_STOCK_PLAYER)];
}

void put_pack_in_game(const CoopItems &pack, std::uint32_t gold) {
    item_stock[kf_enum_encode<u8>(KF_ITEM_STOCK_PLAYER)] = pack;
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
    if (enter.beside)
        stand_beside(*enter.beside);
    put_on(enter.worn);
    return true;
}

// ---- joining and leaving, between updates ----

void reset_session() {
    const CoopPlayerId self = coop.self;
    const std::string name = coop.name;
    const bool looked_for_aside = coop.looked_for_aside;
    coop = Coop{};
    coop.self = self;
    coop.name = name;
    coop.looked_for_aside = looked_for_aside;
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

void write_world_file() {
    HostSide &host = coop.host;
    keep_live_world();
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
    if (coop.role == Role::Host) {
        coop_floors_start(coop.self, coop.self);
        host_roster();
    } else {
        coop_floors_start(coop.self, 0);
        coop_floors_roster(coop.guest.roster);
    }
    coop.mirror = CoopPackMirror(coop.enter.pack, coop.enter.gold);
    coop.worn_sent = game_worn();
    coop.enter = {};
    std::printf("[coop] playing the world of room %s\n", coop.room.c_str());
}

void enter_as_host() {
    auto file = read_world_file();
    if (!file) {
        fail("This room's coop world cannot be read; it was left as it is.");
        return;
    }
    HostSide &host = coop.host;
    host.file = std::move(*file);
    const CoopCharacter character = host.file.live.party.join(coop.self, coop.name);
    CoopParty &party = host.file.live.party;
    coop.enter = {character.state, host.file.live.world, party.pack(), party.gold(), character.wore, std::nullopt};
    enter_world();
    if (!coop.in_play)
        return;
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
    coop_floors_stop();
    coop_hits_stop();
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

void feed(CoopPlayerId by, CoopCause cause, const CoopPackChange &change) {
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

void host_welcome(std::uint64_t peer, const CoopHello &hello) {
    HostSide &host = coop.host;
    map_world_state_persist();
    const CoopCharacter character = host.file.live.party.join(hello.id, hello.name);
    host.guests[peer] = hello.id;
    const CoopParty &party = host.file.live.party;
    const auto &cell = player_state.motion_state.map_cell;
    send(peer, CoopWelcome{character.state, character.wore, party.pack(), party.gold(), party.highest_floor(),
        save_system_coop_world(), kf_enum_encode<u8>(player_state.progress_state.current_floor), cell.x, cell.z});
    host.pack_changed = true;
    host.file_changed = true;
    host_roster();
    std::printf("[coop] %s joined the world\n", hello.name.c_str());
}

void between_updates_as_host() {
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

// ---- messages ----

void host_receives(std::uint64_t peer, const CoopMessage &message) {
    HostSide &host = coop.host;
    if (const auto *hello = std::get_if<CoopHello>(&message)) {
        if (hello->id != coop.self && !host.guests.contains(peer))
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
    } else if (std::holds_alternative<CoopSaveRequest>(message)) {
        coop_save_point();
    }
}

void guest_receives(std::uint64_t peer, const CoopMessage &message) {
    GuestSide &guest = coop.guest;
    if (const auto *welcome = std::get_if<CoopWelcome>(&message)) {
        if (guest.host != 0 || coop.in_play)
            return;
        guest.host = peer;
        guest.welcome = *welcome;
        coop.entering = true;
        return;
    }
    if (peer != guest.host)
        return;
    if (const auto *roster = std::get_if<CoopRoster>(&message)) {
        guest.roster = *roster;
        coop_floors_roster(*roster);
    } else if (const auto *pack = std::get_if<CoopPack>(&message)) {
        guest.pack = *pack;
    }
    else if (const auto *line = std::get_if<CoopFeedLine>(&message))
        feed(line->by, line->cause, line->change);
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

void coop_room_joined(std::string_view room, bool first) {
    know_self();
    reset_session();
    coop.room = room;
    coop.role = first ? Role::Host : Role::Guest;
    coop.entering = first;
    std::printf("[coop] in room %s as %s (%s)\n", coop.room.c_str(), coop.name.c_str(), first ? "host" : "guest");
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

std::string coop_own_name() {
    know_self();
    return coop.name;
}

void coop_peer_open(std::uint64_t peer, bool older) {
    if (coop.role == Role::Guest && older && coop.guest.host == 0)
        send(peer, CoopHello{coop.self, coop.name});
}

void coop_peer_closed(std::uint64_t peer) {
    if (coop.role == Role::Host) {
        const auto guest = coop.host.guests.find(peer);
        if (guest == coop.host.guests.end())
            return;
        const CoopPlayerId id = guest->second;
        coop.host.file.live.party.leave(id);
        coop.host.guests.erase(guest);
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
    return player < coop.feed_players.size() ? name_of(coop.feed_players[player]) : std::string();
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
    return "\"role\":\"" + std::string(role) + "\",\"inPlay\":" + (coop.in_play ? "true" : "false") + ",\"name\":\"" +
           coop.name + "\",\"pack\":{" + pack + "},\"gold\":" + std::to_string(player_state.gold) + ",\"worn\":" +
           worn_json(game_worn()) + ",\"othersWear\":[" + others + "],\"feed\":[" + feed_rows + "],\"highestFloor\":" +
           std::to_string(kf::coop_highest_floor(kf_enum_encode<u8>(player_state.progress_state.highest_floor))) +
           ",\"floor\":" + std::to_string(kf_enum_encode<int>(player_state.progress_state.current_floor)) + ",\"cell\":[" +
           std::to_string(player_state.motion_state.map_cell.x) + "," + std::to_string(player_state.motion_state.map_cell.z) +
           "],\"floorPart\":\"" + std::string(coop_floors_part()) + "\"";
}

std::string coop_panel_line() {
    switch (coop.role) {
    case Role::None:
        return {};
    case Role::Host:
        return coop.in_play ? "Your coop world (" + coop.name + ")" : "Opening your coop world...";
    case Role::Guest:
        return coop.in_play ? "Playing the host's coop world as " + coop.name : "Asking the host to join...";
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
        send(coop.guest.host, CoopSaveRequest{});
        host_show_message("The party is saved.");
        return true;
    }
    HostSide &host = coop.host;
    host.file.live.party.store(coop.self, save_system_coop_character(),
        kf_enum_encode<u8>(player_state.progress_state.highest_floor));
    keep_live_world();
    host.file.last_save = host.file.live;
    write_world_file();
    host_show_message("The party is saved.");
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
