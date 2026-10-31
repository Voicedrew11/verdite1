#include "record.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <memory>
#include <stdexcept>
#include <unistd.h>
#include <utility>

namespace vo {
namespace {

constexpr std::uint8_t record_version = 1;
// Where each field sits in the body (online/protocol/PROTOCOL.md, "Bytes").
constexpr std::size_t body_area = 1;
constexpr std::size_t body_place = 5;
constexpr std::size_t body_payload_size = 9;
constexpr std::size_t body_payload = 10;
constexpr std::size_t body_author = 18;
constexpr std::size_t body_serial = 50;
static_assert(body_payload + record_payload_bytes == body_author);
static_assert(body_author + record_key_bytes == body_serial);
static_assert(body_serial + 4 == record_body_bytes);

constexpr std::size_t sha256_bytes = 32;
constexpr std::size_t rating_body_bytes = 8 + record_key_bytes;   // id, rater
static_assert(rating_body_bytes + record_signature_bytes == 104);

using Seed = std::array<std::uint8_t, record_key_bytes>;
using Signature = std::array<std::uint8_t, record_signature_bytes>;
using Body = std::array<std::uint8_t, record_body_bytes>;
using Digest = std::array<std::uint8_t, sha256_bytes>;
using PkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using MdCtxPtr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

// Little-endian: out.size() bytes of value, from its low byte.
void put_le(std::span<std::uint8_t> out, std::uint64_t value) {
    for (auto &byte : out) {
        byte = static_cast<std::uint8_t>(value & 0xff);
        value >>= 8;
    }
}

// Little-endian: the bytes as one number, at most 8 of them.
std::uint64_t get_le(std::span<const std::uint8_t> in) {
    std::uint64_t value = 0;
    for (std::size_t i = in.size(); i-- > 0;)
        value = (value << 8) | in[i];
    return value;
}

void append_text(std::vector<std::uint8_t> &out, std::string_view text) {
    for (const char c : text)
        out.push_back(static_cast<std::uint8_t>(c));
}

// The domain (prefix, then game), one zero byte, then bytes: what a signature covers.
std::vector<std::uint8_t> signed_message(std::string_view prefix, std::string_view game,
                                         std::span<const std::uint8_t> bytes) {
    std::vector<std::uint8_t> message;
    message.reserve(prefix.size() + game.size() + 1 + bytes.size());
    append_text(message, prefix);
    append_text(message, game);
    message.push_back(0);
    message.insert(message.end(), bytes.begin(), bytes.end());
    return message;
}

// The 54 bytes of a record's body. A payload over 8 bytes is cut to 8 here; callers that take
// records from outside reject that size before they reach this.
Body record_body(const Record &record) {
    Body body{};
    std::span<std::uint8_t> out(body);
    out[0] = record_version;
    put_le(out.subspan(body_area, 4), record.area);
    put_le(out.subspan(body_place, 4), record.place);
    out[body_payload_size] = record.payload_size;
    const std::size_t payload_len = std::min<std::size_t>(record.payload_size, record_payload_bytes);
    std::copy_n(record.payload.begin(), payload_len, out.begin() + body_payload);
    std::copy(record.author.begin(), record.author.end(), out.begin() + body_author);
    put_le(out.subspan(body_serial, 4), record.serial);
    return body;
}

PkeyPtr private_key_object(const Seed &seed) {
    return PkeyPtr(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed.data(), seed.size()),
                   EVP_PKEY_free);
}

PkeyPtr public_key_object(const RecordKey &key) {
    return PkeyPtr(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, key.data(), key.size()),
                   EVP_PKEY_free);
}

std::optional<RecordKey> derive_public_key(const Seed &seed) {
    const PkeyPtr key = private_key_object(seed);
    if (!key)
        return std::nullopt;
    RecordKey out{};
    std::size_t length = out.size();
    if (EVP_PKEY_get_raw_public_key(key.get(), out.data(), &length) != 1 || length != out.size())
        return std::nullopt;
    return out;
}

// Ed25519 over message in one shot (no digest).
std::optional<Signature> ed25519_sign(const Seed &seed, std::span<const std::uint8_t> message) {
    const PkeyPtr key = private_key_object(seed);
    const MdCtxPtr ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!key || !ctx)
        return std::nullopt;
    if (EVP_DigestSignInit(ctx.get(), nullptr, nullptr, nullptr, key.get()) != 1)
        return std::nullopt;
    Signature signature{};
    std::size_t length = signature.size();
    if (EVP_DigestSign(ctx.get(), signature.data(), &length, message.data(), message.size()) != 1 ||
        length != signature.size())
        return std::nullopt;
    return signature;
}

bool ed25519_verify(const RecordKey &public_key, const Signature &signature,
                    std::span<const std::uint8_t> message) {
    const PkeyPtr key = public_key_object(public_key);
    const MdCtxPtr ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!key || !ctx)
        return false;
    if (EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, key.get()) != 1)
        return false;
    return EVP_DigestVerify(ctx.get(), signature.data(), signature.size(), message.data(),
                            message.size()) == 1;
}

std::optional<Digest> sha256(std::span<const std::uint8_t> bytes) {
    std::array<std::uint8_t, EVP_MAX_MD_SIZE> buffer{};
    unsigned int length = 0;
    if (EVP_Digest(bytes.data(), bytes.size(), buffer.data(), &length, EVP_sha256(), nullptr) != 1 ||
        length != sha256_bytes)
        return std::nullopt;
    Digest digest{};
    std::copy_n(buffer.begin(), digest.size(), digest.begin());
    return digest;
}

bool write_all(int fd, std::span<const std::uint8_t> bytes) {
    std::size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t written = ::write(fd, bytes.data() + done, bytes.size() - done);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return false;
        done += static_cast<std::size_t>(written);
    }
    return true;
}

// The seed in an open file, which must hold exactly 32 bytes. Closes fd.
std::optional<Seed> read_seed(int fd) {
    std::array<std::uint8_t, record_key_bytes + 1> buffer{}; // one more: longer is not a key
    std::size_t total = 0;
    bool read_ok = true;
    while (total < buffer.size()) {
        const ssize_t got = ::read(fd, buffer.data() + total, buffer.size() - total);
        if (got == 0)
            break;
        if (got < 0) {
            if (errno == EINTR)
                continue;
            read_ok = false;
            break;
        }
        total += static_cast<std::size_t>(got);
    }
    const bool closed_ok = ::close(fd) == 0;
    if (!read_ok || !closed_ok || total != record_key_bytes)
        return std::nullopt;
    Seed seed{};
    std::copy_n(buffer.begin(), seed.size(), seed.begin());
    return seed;
}

// A new random seed written to a new owner-only file. Nothing is left behind on failure.
std::optional<Seed> create_seed(const std::string &path) {
    Seed seed{};
    if (RAND_bytes(seed.data(), static_cast<int>(seed.size())) != 1)
        return std::nullopt;
    // O_EXCL: an existing file is never overwritten, even if it appeared since the open
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
        return std::nullopt;
    const bool written = write_all(fd, seed) && ::fsync(fd) == 0;
    const bool closed = ::close(fd) == 0;
    if (written && closed)
        return seed;
    // A part-written key would only be refused next time; remove it if we can.
    if (::unlink(path.c_str()) != 0) {
        // the partial file stays; load() still reports none
    }
    return std::nullopt;
}

// The key at path: read when it exists, made there when it does not (ENOENT only).
std::optional<Seed> seed_at(const std::string &path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd >= 0)
        return read_seed(fd);
    if (errno != ENOENT)
        return std::nullopt;
    return create_seed(path);
}

// The bytes a rating signs after its domain: the record's id, then the rater's key.
std::array<std::uint8_t, rating_body_bytes> rating_body(std::uint64_t record, const RecordKey &rater) {
    std::array<std::uint8_t, rating_body_bytes> body{};
    put_le(std::span<std::uint8_t>(body).first(8), record);
    std::copy(rater.begin(), rater.end(), body.begin() + 8);
    return body;
}

} // namespace

std::optional<RecordSigner> RecordSigner::load(const std::string &path) {
    const std::optional<Seed> seed = seed_at(path);
    if (!seed || !derive_public_key(*seed))
        return std::nullopt;
    return from_seed(*seed);
}

RecordSigner RecordSigner::from_seed(std::span<const std::uint8_t, 32> seed) {
    RecordSigner signer;
    std::copy(seed.begin(), seed.end(), signer.seed_.begin());
    const std::optional<RecordKey> key = derive_public_key(signer.seed_);
    if (!key)
        throw std::runtime_error("Ed25519 key derivation failed");
    signer.public_key_ = *key;
    return signer;
}

Record RecordSigner::sign(Record record, std::string_view game) const {
    if (record.payload_size > record_payload_bytes)
        throw std::invalid_argument("a record's payload is at most 8 bytes");
    record.author = public_key_;
    const Body body = record_body(record);
    const std::optional<Signature> signature =
        ed25519_sign(seed_, signed_message("verdite-record/", game, body));
    if (!signature)
        throw std::runtime_error("Ed25519 signing failed");
    record.signature = *signature;
    return record;
}

std::array<std::uint8_t, record_bytes> record_write(const Record &record) {
    std::array<std::uint8_t, record_bytes> out{};
    const Body body = record_body(record);
    std::copy(body.begin(), body.end(), out.begin());
    std::copy(record.signature.begin(), record.signature.end(), out.begin() + record_body_bytes);
    return out;
}

std::optional<Record> record_read(std::span<const std::uint8_t> bytes) {
    if (bytes.size() != record_bytes || bytes[0] != record_version)
        return std::nullopt;
    Record record;
    record.area = static_cast<std::uint32_t>(get_le(bytes.subspan(body_area, 4)));
    record.place = static_cast<std::uint32_t>(get_le(bytes.subspan(body_place, 4)));
    record.payload_size = bytes[body_payload_size];
    if (record.payload_size > record_payload_bytes)
        return std::nullopt;
    const auto payload = bytes.subspan(body_payload, record_payload_bytes);
    const auto past_size = payload.subspan(record.payload_size);
    if (!std::all_of(past_size.begin(), past_size.end(), [](std::uint8_t byte) { return byte == 0; }))
        return std::nullopt;
    std::copy_n(payload.begin(), record.payload_size, record.payload.begin());
    std::copy_n(bytes.begin() + body_author, record_key_bytes, record.author.begin());
    record.serial = static_cast<std::uint32_t>(get_le(bytes.subspan(body_serial, 4)));
    if (record.serial == 0)
        return std::nullopt;
    std::copy_n(bytes.begin() + record_body_bytes, record_signature_bytes, record.signature.begin());
    return record;
}

bool record_verified(const Record &record, std::string_view game) {
    if (record.payload_size > record_payload_bytes)
        return false;
    const Body body = record_body(record);
    return ed25519_verify(record.author, record.signature,
                          signed_message("verdite-record/", game, body));
}

std::uint64_t record_id(const Record &record) {
    const Body body = record_body(record);
    const std::optional<Digest> digest = sha256(body);
    if (!digest)
        throw std::runtime_error("SHA-256 failed");
    const std::uint64_t id = get_le(std::span<const std::uint8_t>(*digest).first(8));
    return id == 0 ? 1 : id;
}

Rating rating_sign(const RecordSigner &signer, std::uint64_t record, std::string_view game) {
    Rating rating;
    rating.record = record;
    rating.rater = signer.public_key();
    const auto body = rating_body(record, rating.rater);
    const std::optional<Signature> signature =
        ed25519_sign(signer.seed_, signed_message("verdite-rating/", game, body));
    if (!signature)
        throw std::runtime_error("Ed25519 signing failed");
    rating.signature = *signature;
    return rating;
}

std::vector<std::uint8_t> rating_write(const Rating &rating) {
    const auto body = rating_body(rating.record, rating.rater);
    std::vector<std::uint8_t> out(body.begin(), body.end());
    out.insert(out.end(), rating.signature.begin(), rating.signature.end());
    return out;
}

RecordPool::RecordPool(std::string game) : game_(std::move(game)) {}

RecordPool::Added RecordPool::add(const Record &record) {
    if (!record_verified(record, game_))
        return Added::Forged;
    return keep(record);
}

void RecordPool::add_own(const Record &record) {
    keep(record);
}

void RecordPool::set_good(std::uint64_t id, std::uint32_t good) {
    const auto found = std::find_if(held_.begin(), held_.end(),
                                    [id](const Held &each) { return each.id == id; });
    if (found != held_.end())
        found->good = good;
}

void RecordPool::clear() {
    // newest_ stays: a record retired before the clear is still never taken again
    held_.clear();
}

RecordPool::Added RecordPool::keep(const Record &record) {
    const auto newest = std::find_if(newest_.begin(), newest_.end(),
                                     [&record](const Newest &each) { return each.author == record.author; });
    if (newest != newest_.end() &&
        static_cast<std::uint64_t>(record.serial) + records_per_author <= newest->serial)
        return Added::Retired;
    const std::uint64_t id = record_id(record);
    if (std::any_of(held_.begin(), held_.end(), [id](const Held &each) { return each.id == id; }))
        return Added::Known;
    held_.push_back(Held{id, record, 0});
    const bool is_newest = newest == newest_.end() || record.serial > newest->serial;
    if (!is_newest)
        return Added::New;
    if (newest == newest_.end())
        newest_.push_back(Newest{record.author, record.serial});
    else
        newest->serial = record.serial;
    retire_below(record.author, record.serial);
    return Added::New;
}

void RecordPool::retire_below(const RecordKey &author, std::uint32_t newest_serial) {
    std::erase_if(held_, [&author, newest_serial](const Held &each) {
        return each.record.author == author &&
               static_cast<std::uint64_t>(each.record.serial) + records_per_author <= newest_serial;
    });
}

} // namespace vo
