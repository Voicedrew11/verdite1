#pragma once
// Records (online/protocol/PROTOCOL.md, "Records"): a signed body of 54 bytes and an Ed25519
// signature over the game's domain and that body, 118 bytes in all. Plain functions over bytes,
// so they are tested without a network; the board (board.cpp) keeps them.
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vo {

inline constexpr std::size_t record_body_bytes = 54;
inline constexpr std::size_t record_signature_bytes = 64;
inline constexpr std::size_t record_bytes = record_body_bytes + record_signature_bytes;
inline constexpr std::size_t record_payload_bytes = 8;
inline constexpr std::size_t record_key_bytes = 32;
inline constexpr std::uint32_t records_per_author = 10;

using RecordKey = std::array<std::uint8_t, record_key_bytes>;

struct Record {
    std::uint32_t area = 0;
    std::uint32_t place = 0;
    std::array<std::uint8_t, record_payload_bytes> payload{};   // zero past payload_size
    std::uint8_t payload_size = 0;
    RecordKey author{};
    std::uint32_t serial = 0;                                    // from 1
    std::array<std::uint8_t, record_signature_bytes> signature{};
    bool operator==(const Record &) const = default;
};

struct Rating;

// An install's Ed25519 key pair; the private half never leaves the process.
class RecordSigner {
public:
    // The key in path: read, or made and written there (owner-only) when the file is missing.
    // None when the file exists but is not a key, or cannot be read or written.
    static std::optional<RecordSigner> load(const std::string &path);
    // A key from its 32-byte seed, for tests.
    static RecordSigner from_seed(std::span<const std::uint8_t, 32> seed);

    const RecordKey &public_key() const { return public_key_; }
    // Fills author and signature; game is the domain ("kf1"), as the board was opened with.
    Record sign(Record record, std::string_view game) const;

private:
    friend Rating rating_sign(const RecordSigner &signer, std::uint64_t record, std::string_view game);
    std::array<std::uint8_t, 32> seed_{};
    RecordKey public_key_{};
};

// The 118 bytes on the wire.
std::array<std::uint8_t, record_bytes> record_write(const Record &record);
// None when the size is not record_bytes, the version is not 1, payload_size is over 8, the
// payload is not zero past it, or serial is 0. Does not check the signature.
std::optional<Record> record_read(std::span<const std::uint8_t> bytes);
// Whether the signature is the author's over game's domain and the body.
bool record_verified(const Record &record, std::string_view game);
// The first 8 bytes of the SHA-256 of the signed body, little-endian; never 0 (a 0 is made 1).
std::uint64_t record_id(const Record &record);

// A rating: the rater's key and a record id, signed as a record is, under game's domain with
// "/rating" after it. 104 bytes on the wire: id (8), rater (32), signature (64).
struct Rating {
    std::uint64_t record = 0;
    RecordKey rater{};
    std::array<std::uint8_t, record_signature_bytes> signature{};
};
Rating rating_sign(const RecordSigner &signer, std::uint64_t record, std::string_view game);
std::vector<std::uint8_t> rating_write(const Rating &rating);

// The records one player holds for one area: added only when verified, retired by serial (an
// author's records at or below their newest serial less records_per_author are dropped, and
// never added again), at most one per id.
class RecordPool {
public:
    explicit RecordPool(std::string game);

    enum class Added : std::uint8_t { New, Known, Retired, Forged };
    // Checks and keeps a record; New only when it was not held and is now.
    Added add(const Record &record);
    // A record this install signed: kept without checking the signature again.
    void add_own(const Record &record);
    void set_good(std::uint64_t id, std::uint32_t good);
    void clear();

    struct Held {
        std::uint64_t id;
        Record record;
        std::uint32_t good;
    };
    // Held records, oldest added first.
    std::span<const Held> held() const { return held_; }

private:
    Added keep(const Record &record);
    void retire_below(const RecordKey &author, std::uint32_t newest_serial);
    std::string game_;
    std::vector<Held> held_;
    struct Newest { RecordKey author; std::uint32_t serial; };
    std::vector<Newest> newest_;   // per author seen
};

} // namespace vo
