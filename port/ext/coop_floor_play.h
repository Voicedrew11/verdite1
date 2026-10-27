#pragma once
// This machine's part in who runs each floor of the coop world, in a room (docs/COOP_PLAN.md,
// "Floor ownership"). It tells everyone which floor its player is on, acts on the host's orders
// that name it, and sends a floor's live state straight to the player an order or the floor's
// heir names. On a floor another player runs it draws that player's actors as puppets, and sends
// what its own player changes there as writes; on a floor it runs it sends its actors and its
// writes, and its record to the host. On the host it also keeps the table of owners
// (CoopFloorOwners) and the records their owners send. Until a player's machine has its floor's
// order its actors stand still. Outside a room this machine runs every floor and nothing here acts.
#include "coop_message.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace kf::ext {

// The coop world is in play in a room, with this player `self` and the host `host` (0 on a guest,
// which learns it from the roster): from now on this machine runs no floor until the host says so
// (the host says so to itself at once).
void coop_floors_start(CoopPlayerId self, CoopPlayerId host);
// The world left play, or the room ended.
void coop_floors_stop();

// Who is in the room and their peers, the host first: the host's own, or as it last sent it.
void coop_floors_roster(const CoopRoster &roster);
// Who is in the room and their peers, the host first, as the roster last said.
std::span<const CoopRoster::Player> coop_floors_room();
// Host only: a guest left the room, dropped or was sent away.
void coop_floors_gone(CoopPlayerId player);
// A floor message (CoopFloorAt to CoopFloorWrites) from a player in the roster.
void coop_floors_receive(std::uint64_t peer, const CoopMessage &message);
// A STATE packet that is not a pose: a floor owner's actors.
void coop_floors_state_packet(std::uint64_t peer, std::span<const std::uint8_t> bytes);

// Between two world updates: this player's floor said when it changes, orders and snapshots acted
// on, the others' writes put in and this player's sent, and on an owner the record to the host.
void coop_floors_between_updates();
// Once a world update, before the actors': an owner sends its actors, another machine puts the
// owner's in place.
void coop_floors_world_update();
// The loaded floor is about to go (map_unload_floor): an owner sends it live to its heir, and its
// record to the host.
void coop_floors_unloading();
// The player whose machine runs this player's floor: this one while it runs it, the owner whose
// actors it draws while it watches, 0 while it waits.
CoopPlayerId coop_floors_runner();
// The floor this machine said it is on, 0 before it has said.
int coop_floors_floor();
// This player, 0 outside a room.
CoopPlayerId coop_floors_self();
// The player a peer is, 0 for one not in the roster.
CoopPlayerId coop_floors_player(std::uint64_t peer);
// The player the online players' list names by `online_id` (OnlinePlayer::id), 0 for none here.
CoopPlayerId coop_floors_player_online(std::uint32_t online_id);
// A message straight to one player in the room.
void coop_floors_send(CoopPlayerId player, const CoopMessage &message);
// Whether this machine runs the loaded floor's actors.
bool coop_floors_run_here();
// What this machine does with its floor, for the agent: "running", "watching", "waiting", or ""
// outside a room.
std::string_view coop_floors_part();
// Host only: whether an owner's record went into the world since the last call, so its file is
// written.
bool coop_floors_world_changed();

}
