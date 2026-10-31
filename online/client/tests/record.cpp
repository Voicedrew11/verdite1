#include "record.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace vo;

namespace {

constexpr std::string_view game = "kf1";

// Lowercase or uppercase hex, two digits a byte; the length must be 2 * N.
template <std::size_t N>
std::array<std::uint8_t, N> from_hex(std::string_view text) {
    const auto nibble = [](char c) -> std::uint8_t {
        if (c >= '0' && c <= '9')
            return static_cast<std::uint8_t>(c - '0');
        if (c >= 'a' && c <= 'f')
            return static_cast<std::uint8_t>(c - 'a' + 10);
        return static_cast<std::uint8_t>(c - 'A' + 10);
    };
    std::array<std::uint8_t, N> out{};
    REQUIRE(text.size() == 2 * N);
    for (std::size_t i = 0; i < N; ++i)
        out[i] = static_cast<std::uint8_t>(nibble(text[2 * i]) << 4 | nibble(text[2 * i + 1]));
    return out;
}

// RFC 8032 section 7.1, test 1.
constexpr std::string_view rfc_seed_hex = "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
constexpr std::string_view rfc_public_hex = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";

RecordSigner signer_for(std::uint8_t fill) {
    std::array<std::uint8_t, 32> seed{};
    seed.fill(fill);
    return RecordSigner::from_seed(seed);
}

Record record_at(std::uint32_t area, std::uint32_t place, std::uint32_t serial) {
    Record record;
    record.area = area;
    record.place = place;
    record.payload_size = 5;
    record.payload = {1, 2, 3, 4, 5, 0, 0, 0};
    record.serial = serial;
    return record;
}

Record signed_record(const RecordSigner &signer, std::uint32_t area, std::uint32_t place,
                     std::uint32_t serial) {
    return signer.sign(record_at(area, place, serial), game);
}

// Bytes of a record as they come off the wire, changed by the caller.
std::array<std::uint8_t, record_bytes> wire(const Record &record) {
    return record_write(record);
}

// A folder under the system temp dir, removed when the test ends.
struct TempDir {
    std::filesystem::path path;

    TempDir() {
        std::random_device random;
        path = std::filesystem::temp_directory_path() /
               ("verdite-record-test-" + std::to_string(random()) + std::to_string(random()));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;
};

} // namespace

TEST_CASE("the public key of RFC 8032 test 1 comes from its seed") {
    const auto seed = from_hex<32>(rfc_seed_hex);
    const RecordSigner signer = RecordSigner::from_seed(seed);
    CHECK(signer.public_key() == from_hex<32>(rfc_public_hex));
}

TEST_CASE("a signed record verifies, and is signed by its signer's key") {
    const RecordSigner signer = signer_for(7);
    const Record signed_once = signed_record(signer, 3, 7, 1);
    CHECK(signed_once.author == signer.public_key());
    CHECK(record_verified(signed_once, game));
}

TEST_CASE("a changed body byte, a wrong game or a changed signature byte fails verification") {
    const RecordSigner signer = signer_for(7);
    const Record signed_once = signed_record(signer, 3, 7, 1);

    Record changed_body = signed_once;
    changed_body.area = 4;
    CHECK_FALSE(record_verified(changed_body, game));

    CHECK_FALSE(record_verified(signed_once, "kf2"));

    Record changed_signature = signed_once;
    changed_signature.signature[10] ^= 0x01;
    CHECK_FALSE(record_verified(changed_signature, game));
}

TEST_CASE("a record written and read back is the same record") {
    const RecordSigner signer = signer_for(7);
    const Record signed_once = signed_record(signer, 3, 7, 1);
    const auto bytes = wire(signed_once);
    CHECK(bytes.size() == 118);
    const std::optional<Record> parsed = record_read(bytes);
    REQUIRE(parsed.has_value());
    const Record parsed_record = parsed.value_or(Record{});
    CHECK(parsed_record == signed_once);
    CHECK(record_verified(parsed_record, game));
}

TEST_CASE("record_read refuses each malformed record") {
    const RecordSigner signer = signer_for(7);
    const auto good = wire(signed_record(signer, 3, 7, 1));
    REQUIRE(record_read(good).has_value());

    std::vector<std::uint8_t> short_bytes(good.begin(), good.end() - 1);
    CHECK_FALSE(record_read(short_bytes).has_value());

    std::vector<std::uint8_t> long_bytes(good.begin(), good.end());
    long_bytes.push_back(0);
    CHECK_FALSE(record_read(long_bytes).has_value());

    auto version = good;
    version[0] = 2;
    CHECK_FALSE(record_read(version).has_value());

    auto payload_size = good;
    payload_size[9] = 9;
    CHECK_FALSE(record_read(payload_size).has_value());

    auto past_size = good;
    past_size[10 + 5] = 1; // payload_size is 5: byte 5 of the payload must be zero
    CHECK_FALSE(record_read(past_size).has_value());

    auto serial_zero = good;
    serial_zero[50] = serial_zero[51] = serial_zero[52] = serial_zero[53] = 0;
    CHECK_FALSE(record_read(serial_zero).has_value());
}

TEST_CASE("a record's id is stable and differs for a different body") {
    const RecordSigner signer = signer_for(7);
    const Record first = signed_record(signer, 3, 7, 1);
    const Record second = signed_record(signer, 3, 8, 1);
    CHECK(record_id(first) == record_id(first));
    CHECK(record_id(first) != record_id(second));
    CHECK(record_id(first) != 0);
}

TEST_CASE("a rating is 104 bytes, its record id first, little-endian") {
    const RecordSigner signer = signer_for(9);
    const Rating rating = rating_sign(signer, 0x0102030405060708u, game);
    const std::vector<std::uint8_t> bytes = rating_write(rating);
    CHECK(bytes.size() == 104);
    CHECK(bytes[0] == 0x08);
    CHECK(bytes[7] == 0x01);
    CHECK(std::equal(bytes.begin() + 8, bytes.begin() + 40, signer.public_key().begin()));
}

TEST_CASE("a pool keeps a record once, and then says it is known") {
    const RecordSigner signer = signer_for(7);
    RecordPool pool{std::string(game)};
    const Record first = signed_record(signer, 3, 7, 1);
    CHECK(pool.add(first) == RecordPool::Added::New);
    CHECK(pool.add(first) == RecordPool::Added::Known);
    CHECK(pool.held().size() == 1);
}

TEST_CASE("a pool refuses a forged record and keeps nothing of it") {
    const RecordSigner signer = signer_for(7);
    RecordPool pool{std::string(game)};
    Record forged = signed_record(signer, 3, 7, 1);
    forged.area = 4;
    CHECK(pool.add(forged) == RecordPool::Added::Forged);
    CHECK(pool.held().empty());
}

TEST_CASE("eleven records from one author leave ten, and the first is retired when offered again") {
    const RecordSigner signer = signer_for(7);
    RecordPool pool{std::string(game)};
    for (std::uint32_t serial = 1; serial <= 11; ++serial)
        CHECK(pool.add(signed_record(signer, 3, serial, serial)) == RecordPool::Added::New);
    REQUIRE(pool.held().size() == 10);
    CHECK(pool.held().front().record.serial == 2);
    CHECK(pool.add(signed_record(signer, 3, 1, 1)) == RecordPool::Added::Retired);
    CHECK(pool.held().size() == 10);
    CHECK(pool.add(signed_record(signer, 3, 2, 2)) == RecordPool::Added::Known);
}

TEST_CASE("a record older than the author's newest by ten or more is retired, one less is kept") {
    const RecordSigner signer = signer_for(7);
    RecordPool pool{std::string(game)};
    CHECK(pool.add(signed_record(signer, 3, 11, 11)) == RecordPool::Added::New);
    CHECK(pool.add(signed_record(signer, 3, 2, 2)) == RecordPool::Added::New);
    CHECK(pool.add(signed_record(signer, 3, 1, 1)) == RecordPool::Added::Retired);
}

TEST_CASE("two authors' serials do not retire each other's records") {
    const RecordSigner alice = signer_for(7);
    const RecordSigner bob = signer_for(8);
    RecordPool pool{std::string(game)};
    CHECK(pool.add(signed_record(alice, 3, 1, 1)) == RecordPool::Added::New);
    for (std::uint32_t serial = 1; serial <= 11; ++serial)
        CHECK(pool.add(signed_record(bob, 3, 100 + serial, serial)) == RecordPool::Added::New);
    CHECK(pool.add(signed_record(alice, 3, 1, 1)) == RecordPool::Added::Known);
    CHECK(pool.held().size() == 11);
}

TEST_CASE("a record the install signed is kept without a check, and set_good sets its rating") {
    const RecordSigner signer = signer_for(7);
    RecordPool pool{std::string(game)};
    const Record own = signed_record(signer, 3, 7, 1);
    pool.add_own(own);
    CHECK(pool.add(own) == RecordPool::Added::Known);
    pool.set_good(record_id(own), 5);
    REQUIRE(pool.held().size() == 1);
    CHECK(pool.held().front().good == 5);
}

TEST_CASE("clear empties the pool and keeps the retirements") {
    const RecordSigner signer = signer_for(7);
    RecordPool pool{std::string(game)};
    for (std::uint32_t serial = 1; serial <= 11; ++serial)
        pool.add(signed_record(signer, 3, serial, serial));
    pool.clear();
    CHECK(pool.held().empty());
    CHECK(pool.add(signed_record(signer, 3, 1, 1)) == RecordPool::Added::Retired);
    CHECK(pool.add(signed_record(signer, 3, 12, 12)) == RecordPool::Added::New);
}

TEST_CASE("RecordSigner::load makes an owner-only key file, then loads the same key") {
    TempDir dir;
    const std::string path = (dir.path / "key").string();
    const std::optional<RecordSigner> made = RecordSigner::load(path);
    REQUIRE(made.has_value());
    const RecordKey made_key = made.value_or(RecordSigner{}).public_key();
    CHECK(std::filesystem::file_size(path) == 32);
    const auto perms = std::filesystem::status(path).permissions();
    CHECK((perms & std::filesystem::perms::all) == (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write));

    const std::optional<RecordSigner> again = RecordSigner::load(path);
    REQUIRE(again.has_value());
    CHECK(again.value_or(RecordSigner{}).public_key() == made_key);
}

TEST_CASE("RecordSigner::load refuses a key file that is not 32 bytes") {
    TempDir dir;
    const std::string path = (dir.path / "key").string();
    {
        std::ofstream out(path, std::ios::binary);
        out << "abcde";
    }
    CHECK_FALSE(RecordSigner::load(path).has_value());
}
