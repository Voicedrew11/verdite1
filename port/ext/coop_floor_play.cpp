#include "coop_floor_play.h"

#include "coop_floor.h"
#include "coop_floor_frames.h"
#include "internal.hpp"

#include <kf/game/player.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace kf::ext {
namespace {

constexpr std::uint32_t frame_delay_updates = 3;   // as the knights' playback, to ride out late frames
constexpr std::uint64_t record_period_ms = 1000;    // an owner's record goes to the host at most this often

// What this machine does with the floor its player is on.
enum class Part {
    Waiting,    // for the host's order or a snapshot: the actors stand still
    Running,    // owns it: runs the actors, sends them and its record
    Watching,   // another player owns it: draws that player's actors
};

struct Held {
    CoopPlayerId from = 0;
    CoopFloorSnapshot snapshot;
};

struct Floors {
    bool on = false;
    CoopPlayerId self = 0;
    CoopPlayerId host = 0;
    std::optional<CoopFloorOwners> table;              // the host's only
    std::map<std::uint64_t, CoopPlayerId> players;     // by peer
    std::map<CoopPlayerId, std::uint64_t> peers;       // by player
    std::array<CoopFloorOwnership, coop_floor_count> rows{};
    int floor = 0;                                     // the floor this machine said it is on
    CoopPlayerId left_floor_with = 0;
    Part part = Part::Waiting;
    CoopPlayerId watching = 0;
    std::optional<Held> held;                          // a snapshot not yet put in
    std::map<int, std::vector<std::uint8_t>> host_records;   // from the host, for a run from the record
    std::vector<std::pair<CoopPlayerId, CoopMessage>> inbox;
    CoopFramePlayback playback{frame_delay_updates};
    std::uint32_t update = 0;
    bool record_due = false;
    std::uint64_t record_sent_ms = 0;
    bool world_changed = false;
    bool reloading = false;
};
Floors floors;

std::uint64_t now_ms() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

int loaded_floor() {
    return kf_enum_encode<int>(player_state.progress_state.current_floor);
}

bool is_host() {
    return floors.self == floors.host;
}

bool alone() {
    return floors.peers.size() <= 1;
}

CoopFloorOwnership &row(int floor) {
    return floors.rows[static_cast<std::size_t>(floor - 1)];
}

void send_to(CoopPlayerId player, const CoopMessage &message) {
    if (player == floors.self) {
        floors.inbox.emplace_back(player, message);
        return;
    }
    const auto peer = floors.peers.find(player);
    const auto bytes = coop_message_encode(message);
    if (peer == floors.peers.end() || !bytes) {
        std::printf("[coop] a floor message to a player not in the room was dropped\n");
        return;
    }
    online_send_control(peer->second, *bytes);
}

void tell_everyone(const CoopMessage &message) {
    if (const auto bytes = coop_message_encode(message))
        online_broadcast_control(*bytes);
    floors.inbox.emplace_back(floors.self, message);
}

void watch(CoopPlayerId owner) {
    floors.part = Part::Watching;
    floors.watching = owner;
    floors.playback = CoopFramePlayback(frame_delay_updates);
}

void run() {
    floors.part = Part::Running;
    floors.watching = 0;
    floors.record_due = true;
}

void wait() {
    floors.part = Part::Waiting;
    floors.watching = 0;
}

// The floor reloaded from a record and a live floor, without the unload hook handing it on.
bool reload(std::span<const std::uint8_t> record, std::span<const std::uint8_t> live) {
    floors.reloading = true;
    const bool done = coop_floor_reload(record, live);
    floors.reloading = false;
    if (!done)
        std::printf("[coop] floor %d's state from another player was refused\n", floors.floor);
    return done;
}

CoopFloorSnapshot snapshot_for(CoopSnapshotFor purpose) {
    return {floors.floor, purpose, coop_floor_record(floors.floor), coop_floor_capture()};
}

// ---- the host's side ----

void host_orders(const std::vector<CoopFloorOrder> &orders) {
    if (orders.empty())
        return;
    for (const CoopFloorOrder &order : orders)
        if (const auto *run_order = std::get_if<CoopRunFromRecord>(&order); run_order && run_order->to != floors.self)
            send_to(run_order->to, CoopFloorRecord{run_order->floor, coop_floor_record(run_order->floor)});
    tell_everyone(CoopFloorOrders{orders});
}

void host_takes(CoopPlayerId from, const CoopMessage &message) {
    if (!floors.table)
        return;
    CoopFloorOwners &table = *floors.table;
    if (const auto *at = std::get_if<CoopFloorAt>(&message)) {
        host_orders(table.arrived(from, at->floor, at->left_floor_with));
    } else if (const auto *taken = std::get_if<CoopFloorTaken>(&message)) {
        table.taken(from, taken->floor);
    } else if (const auto *record = std::get_if<CoopFloorRecord>(&message)) {
        if (from == floors.self || !table.records_kept_from(from, record->floor)
            || coop_floor_record_floor(record->record) != record->floor)
            return;
        if (coop_floor_keep_record(record->record))
            floors.world_changed = true;
    }
}

// ---- every machine's side ----

void act_on(const CoopFloorOwnership &ownership) {
    row(ownership.floor) = ownership;
    if (ownership.floor != floors.floor)
        return;
    if (ownership.owner == floors.self) {
        if (floors.part == Part::Watching)
            wait();   // the owner walked off: its snapshot comes, or a run from the record
        return;
    }
    if (floors.part == Part::Watching && ownership.owner != 0 && ownership.owner != floors.watching)
        watch(ownership.owner);
    if (floors.held && floors.held->snapshot.purpose == CoopSnapshotFor::Handover && ownership.owner != 0
        && ownership.owner != floors.held->from)
        floors.held.reset();   // the floor went to another
}

void act_on(const CoopRunFromRecord &order) {
    if (order.to != floors.self || order.floor != floors.floor)
        return;
    floors.held.reset();
    if (!is_host()) {
        const auto record = floors.host_records.find(order.floor);
        const auto bytes = record != floors.host_records.end() ? record->second : coop_floor_record(order.floor);
        reload(bytes, {});
    }
    run();
    std::printf("[coop] running floor %d from its record\n", order.floor);
}

void act_on(const CoopTakeFloor &order) {
    if (order.floor != floors.floor)
        return;
    if (order.from == floors.self && floors.part == Part::Running) {
        send_to(order.to, snapshot_for(CoopSnapshotFor::Take));
        watch(order.to);
        std::printf("[coop] gave floor %d to a more senior player\n", order.floor);
    } else if (order.to == floors.self) {
        wait();
    }
}

void act_on(const CoopShowFloor &order) {
    if (order.floor != floors.floor)
        return;
    if (order.owner == floors.self && floors.part == Part::Running)
        send_to(order.to, snapshot_for(CoopSnapshotFor::Show));
    else if (order.to == floors.self)
        wait();
}

// A snapshot held until this machine is on its floor and, for a handover, named its owner.
void put_in_held() {
    if (!floors.held || floors.held->snapshot.floor != floors.floor)
        return;
    Held held = std::move(*floors.held);
    floors.held.reset();
    const CoopFloorSnapshot &snapshot = held.snapshot;
    switch (snapshot.purpose) {
    case CoopSnapshotFor::Handover:
        if (row(snapshot.floor).owner != floors.self) {
            floors.held = std::move(held);   // the host has not named this machine yet
            return;
        }
        [[fallthrough]];
    case CoopSnapshotFor::Take:
        if (reload(snapshot.record, snapshot.live)) {
            run();
            send_to(floors.host, CoopFloorTaken{snapshot.floor});
            std::printf("[coop] took floor %d live\n", snapshot.floor);
        }
        return;
    case CoopSnapshotFor::Show:
        if (reload(snapshot.record, snapshot.live))
            watch(held.from);
        return;
    }
}

void act_on_message(CoopPlayerId from, const CoopMessage &message) {
    if (is_host() && (std::holds_alternative<CoopFloorAt>(message) || std::holds_alternative<CoopFloorTaken>(message)
                      || std::holds_alternative<CoopFloorRecord>(message))) {
        host_takes(from, message);
        return;
    }
    if (const auto *orders = std::get_if<CoopFloorOrders>(&message)) {
        if (from != floors.host)
            return;
        for (const CoopFloorOrder &order : orders->orders)
            std::visit([](const auto &kind) { act_on(kind); }, order);
    } else if (const auto *record = std::get_if<CoopFloorRecord>(&message)) {
        if (from == floors.host)
            floors.host_records[record->floor] = record->record;
    } else if (const auto *snapshot = std::get_if<CoopFloorSnapshot>(&message)) {
        floors.held = Held{from, *snapshot};
    } else if (const auto *writes = std::get_if<CoopFloorWrites>(&message)) {
        if (from == floors.self || writes->floor != floors.floor)
            return;
        if (!coop_floor_apply_writes(writes->writes))
            std::printf("[coop] writes to floor %d that are not writes were dropped\n", writes->floor);
        else if (floors.part == Part::Running)
            floors.record_due = true;
    }
}

void arrive() {
    const int floor = loaded_floor();
    if (floor == floors.floor)
        return;
    floors.floor = floor;
    floors.held.reset();
    wait();
    tell_everyone(CoopFloorAt{floor, floors.left_floor_with});
    floors.left_floor_with = 0;
}

void send_writes() {
    auto writes = coop_floor_writes();
    if (writes.empty() || alone())
        return;
    if (floors.part == Part::Running)
        floors.record_due = true;
    if (const auto bytes = coop_message_encode(CoopFloorWrites{floors.floor, std::move(writes)}))
        online_broadcast_control(*bytes);
}

void send_record() {
    if (is_host() || floors.part != Part::Running || !floors.record_due)
        return;
    const std::uint64_t now = now_ms();
    if (now - floors.record_sent_ms < record_period_ms)
        return;
    send_to(floors.host, CoopFloorRecord{floors.floor, coop_floor_record(floors.floor)});
    floors.record_due = false;
    floors.record_sent_ms = now;
}

} // namespace

void coop_floors_start(CoopPlayerId self, CoopPlayerId host) {
    floors = Floors{};
    floors.on = true;
    floors.self = self;
    floors.host = host;
    if (self == host)
        floors.table.emplace(host);
}

void coop_floors_stop() {
    floors = Floors{};
}

void coop_floors_roster(const CoopRoster &roster) {
    floors.players.clear();
    floors.peers.clear();
    for (const CoopRoster::Player &player : roster.players) {
        floors.players[player.peer] = player.id;
        floors.peers[player.id] = player.peer;
    }
    if (!floors.table && !roster.players.empty())
        floors.host = roster.players.front().id;
}

void coop_floors_gone(CoopPlayerId player) {
    if (!floors.on || !floors.table)
        return;
    host_orders(floors.table->gone(player));
}

void coop_floors_receive(std::uint64_t peer, const CoopMessage &message) {
    if (!floors.on)
        return;
    const auto player = floors.players.find(peer);
    if (player != floors.players.end())
        floors.inbox.emplace_back(player->second, message);
}

void coop_floors_state_packet(std::uint64_t peer, std::span<const std::uint8_t> bytes) {
    const auto player = floors.players.find(peer);
    if (!floors.on || floors.part != Part::Watching || player == floors.players.end() || player->second != floors.watching)
        return;
    if (auto part = coop_frame_decode(bytes); part && part->floor == floors.floor)
        floors.playback.receive(*part);
}

void coop_floors_between_updates() {
    if (!floors.on)
        return;
    arrive();
    send_writes();
    // Acting on one message can bring others (the host's own orders to itself).
    while (!floors.inbox.empty()) {
        auto inbox = std::move(floors.inbox);
        floors.inbox.clear();
        for (const auto &[from, message] : inbox) {
            act_on_message(from, message);
            put_in_held();
        }
    }
    put_in_held();   // one that came before this machine was on its floor
    send_record();
}

void coop_floors_world_update() {
    if (!floors.on || loaded_floor() != floors.floor)
        return;
    ++floors.update;
    if (floors.part == Part::Running && !alone()) {
        const auto actors = coop_floor_actors();
        for (const auto &packet : coop_frame_encode(floors.floor, floors.update, actors, coop_floor_actor_bytes()))
            online_broadcast_state(packet);
    } else if (floors.part == Part::Watching) {
        if (const auto frame = floors.playback.step(); frame && !coop_floor_show_actors(*frame))
            std::printf("[coop] actors from floor %d's owner that are not actors were dropped\n", floors.floor);
    }
}

void coop_floors_unloading() {
    if (!floors.on || floors.reloading)
        return;
    coop_floor_forget();
    if (floors.part != Part::Running) {
        wait();
        return;
    }
    if (!is_host())
        send_to(floors.host, CoopFloorRecord{floors.floor, coop_floor_record(floors.floor)});
    const CoopPlayerId heir = row(floors.floor).heir;
    if (heir != 0 && heir != floors.self) {
        send_to(heir, snapshot_for(CoopSnapshotFor::Handover));
        floors.left_floor_with = heir;
        std::printf("[coop] handed floor %d on live\n", floors.floor);
    }
    wait();
}

bool coop_floors_run_here() {
    return !floors.on || floors.part == Part::Running;
}

std::string_view coop_floors_part() {
    if (!floors.on)
        return "";
    switch (floors.part) {
    case Part::Running:
        return "running";
    case Part::Watching:
        return "watching";
    case Part::Waiting:
        return "waiting";
    }
    return "";
}

bool coop_floors_world_changed() {
    return std::exchange(floors.world_changed, false);
}

}
