#pragma once
// The service's frames (online/protocol/PROTOCOL.md), read and written. Nothing here touches the
// network: what arrives is checked once, here, so the session behind it can trust it.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vo {

using PeerId = std::uint64_t; // 0 is no peer

constexpr int protocol_version = 1;
constexpr std::size_t frame_bytes = std::size_t{64} * 1024;

// A peer id is 16 lowercase hex digits on the wire.
std::optional<PeerId> parse_peer(std::string_view hex);
std::string peer_hex(PeerId peer);

// The WebSocket address of a room, or nothing when a name does not match its form or the
// service is not an http:// or https:// address. A listed_as names the host of a listed room,
// which must have a public code; empty, the room is not listed.
std::optional<std::string> room_url(std::string_view service_url, std::string_view game,
                                    std::string_view room, std::string_view build,
                                    std::string_view listed_as = {});
// The WebSocket address of the list of public rooms, checked as room_url is.
std::optional<std::string> lobby_url(std::string_view service_url, std::string_view game, std::string_view build);
// The WebSocket addresses of a floor's records (its sample, drawn from seed) and of its mesh, which
// takes a build's name but checks none (PROTOCOL.md, "A floor's records" and "The floor mesh").
std::optional<std::string> floor_records_url(std::string_view service_url, std::string_view game,
                                             std::uint32_t area, std::uint32_t seed);
std::optional<std::string> floor_mesh_url(std::string_view service_url, std::string_view game,
                                          std::uint32_t area, std::string_view build);

constexpr std::size_t listed_rooms_most = 50;
constexpr std::uint32_t floor_most = 99;

// A row of the list of public rooms.
struct ListedRoom {
    std::string room;
    std::string host;
    std::uint32_t floor = 0;
    std::uint32_t players = 0;
    std::uint32_t most = 0;
};

// One URL of an RTCIceServer, with its credential.
struct IceServer {
    std::string url;
    std::string username;
    std::string credential;
};

// What peers pass each other through the service, opaque to it. Each belongs to a generation of
// the pair's connection: 0 first, one more each time it moves to a new TURN credential.
struct Signal {
    enum class Kind { Offer, Answer, Candidate, Restart };
    Kind kind = Kind::Offer;
    std::uint32_t generation = 0; // Restart: the generation asked for
    std::string sdp;       // Offer, Answer
    std::string candidate; // Candidate
    std::string mid;       // Candidate
};

struct ServiceFrame {
    // Link: a floor mesh's newer peer is told to offer to peer, as to one named in welcome.
    enum class Kind { Welcome, Join, Leave, Signal, Turn, Rooms, Error, Link };
    Kind kind = Kind::Error;
    int protocol = 0;                   // Welcome
    PeerId peer = 0;                    // Welcome: you; Join, Leave, Link: them; Signal: the sender
    std::vector<PeerId> peers;          // Welcome: already in the room
    Signal signal;                      // Signal
    std::vector<IceServer> ice_servers; // Turn: one per URL
    int seconds = 0;                    // Turn: how long the credential lasts
    std::vector<ListedRoom> rooms;      // Rooms: newest first
    std::string code;                   // Error
    std::string message;                // Error
};

// Nothing when the text is not a frame of the protocol, or a signal's data is not one the
// clients agree on.
std::optional<ServiceFrame> parse_service_frame(std::string_view text);

std::string signal_frame(PeerId to, const Signal &signal);
std::string turn_frame();
std::string describe_frame(std::uint32_t floor);

} // namespace vo
