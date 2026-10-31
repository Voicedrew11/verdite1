#pragma once
// A floor's frames and the floor mesh's packets (online/protocol/PROTOCOL.md, "Records"), read and
// written. Each one is checked once, here, as it enters: sizes, ids, and every record's bytes. The
// board behind them trusts what comes out. Knows no network.

#include "record.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vo {

// Standard base64 with padding (RFC 4648). Decoding is strict: the length, the alphabet, padding
// only at the end, and zero bits past the data are all checked.
std::string base64_encode(std::span<const std::uint8_t> bytes);
std::optional<std::vector<std::uint8_t>> base64_decode(std::string_view text);

// A record as the service sends it, with its good count.
struct CountedRecord {
    Record record;
    std::uint32_t good = 0;
};

constexpr std::size_t sample_most = 24;               // a records frame's most
constexpr std::size_t mine_most = records_per_author; // an author's records out

// What the client sends on a floor's records socket.
std::string post_frame(const Record &record);
std::string rate_frame(const Rating &rating);
std::string mine_frame(const RecordKey &author);

struct FloorFrame {
    enum class Kind { Records, Posted, Rated, Mine, Error };
    Kind kind = Kind::Error;
    std::vector<CountedRecord> records; // Records, Mine
    std::uint64_t id = 0;               // Posted, Rated: a record id, never 0
    std::uint32_t good = 0;             // Posted, Rated
    std::string code;                   // Error
    std::string message;                // Error
};

// Nothing when the text is not one of the service's floor frames, or one of its records is not a
// record (record_read: the bytes, not the signature, which the pool checks).
std::optional<FloorFrame> parse_floor_frame(std::string_view text);

// The floor mesh's packets, sent as CONTROL packets (PROTOCOL.md, "The floor mesh").
constexpr std::size_t shown_most = 255;   // records in a shown packet
constexpr std::uint8_t new_hops_most = 3; // hops a new packet starts with

// What a player shows, sent once when a link opens. Only the first shown_most of records are sent.
std::vector<std::uint8_t> shown_packet(std::span<const Record> records);
// A new post, with the hops it has left (0 to new_hops_most).
std::vector<std::uint8_t> new_packet(const Record &record, std::uint8_t hops);

struct GossipPacket {
    enum class Kind { Shown, New };
    Kind kind = Kind::New;
    std::vector<Record> records; // Shown: any number; New: exactly one
    std::uint8_t hops = 0;       // New: the hops left after this one
};

// Nothing when the bytes are not a packet of the mesh, or a record in it is not a record.
std::optional<GossipPacket> parse_gossip(std::span<const std::uint8_t> bytes);

} // namespace vo
