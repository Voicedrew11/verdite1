#include "floor_frames.h"
#include "signaling.h"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

using namespace vo;

namespace {

constexpr std::string_view game = "kf1";

RecordSigner signer_for(std::uint8_t fill) {
    std::array<std::uint8_t, 32> seed{};
    seed.fill(fill);
    return RecordSigner::from_seed(seed);
}

Record signed_record(const RecordSigner &signer, std::uint32_t place, std::uint32_t serial) {
    Record record;
    record.area = 7;
    record.place = place;
    record.payload_size = 3;
    record.payload = {9, 8, 7, 0, 0, 0, 0, 0};
    record.serial = serial;
    return signer.sign(record, game);
}

// A records-socket frame with one record, as the service writes it.
std::string records_frame(const std::string &type, const Record &record, int good) {
    const auto bytes = record_write(record);
    return R"({"type":")" + type + R"(","records":[{"record":")" + base64_encode(bytes) +
           R"(","good":)" + std::to_string(good) + "}]}";
}

std::span<const std::uint8_t> bytes_of(const std::vector<std::uint8_t> &bytes) {
    return bytes;
}

// The frame, which must parse.
FloorFrame frame_of(std::string_view text) {
    const auto frame = parse_floor_frame(text);
    REQUIRE(frame.has_value());
    return frame.value_or(FloorFrame{});
}

// The packet, which must parse.
GossipPacket packet_of(const std::vector<std::uint8_t> &bytes) {
    const auto packet = parse_gossip(bytes_of(bytes));
    REQUIRE(packet.has_value());
    return packet.value_or(GossipPacket{});
}

// The text decoded, which must be valid base64.
std::vector<std::uint8_t> decoded(std::string_view text) {
    const auto bytes = base64_decode(text);
    REQUIRE(bytes.has_value());
    return bytes.value_or(std::vector<std::uint8_t>{});
}

} // namespace

TEST_CASE("base64 matches RFC 4648's test vectors, with padding") {
    const auto text = [](std::string_view s) {
        return std::vector<std::uint8_t>(s.begin(), s.end());
    };
    CHECK(base64_encode(text("")) == "");
    CHECK(base64_encode(text("f")) == "Zg==");
    CHECK(base64_encode(text("fo")) == "Zm8=");
    CHECK(base64_encode(text("foo")) == "Zm9v");
    CHECK(base64_encode(text("foob")) == "Zm9vYg==");
    CHECK(base64_encode(text("fooba")) == "Zm9vYmE=");
    CHECK(base64_encode(text("foobar")) == "Zm9vYmFy");
    CHECK(base64_decode("Zm9vYmFy") == text("foobar"));
    CHECK(base64_decode("Zm9vYmE=") == text("fooba"));
    CHECK(base64_decode("Zm8=") == text("fo"));
    CHECK(base64_decode("Zg==") == text("f"));
    CHECK(base64_decode("") == std::vector<std::uint8_t>{});
}

TEST_CASE("base64 decodes what it encodes, a record's 118 bytes included") {
    const RecordSigner signer = signer_for(3);
    const auto bytes = record_write(signed_record(signer, 4, 1));
    const std::string text = base64_encode(bytes);
    CHECK(text.size() == 160); // 118 bytes, padded to 120 quartet characters' worth
    CHECK(std::vector<std::uint8_t>(bytes.begin(), bytes.end()) == decoded(text));
}

TEST_CASE("base64 decoding is strict: length, alphabet, padding, and unused bits") {
    CHECK_FALSE(base64_decode("Zm9"));        // length not a multiple of 4
    CHECK_FALSE(base64_decode("Zm9v!mFy"));   // a character outside the alphabet
    CHECK_FALSE(base64_decode("Zm9v\nYmFy")); // whitespace
    CHECK_FALSE(base64_decode("Zg=="
                              "Zg==")); // padding before the end
    CHECK_FALSE(base64_decode("Z===")); // padding where data belongs
    CHECK_FALSE(base64_decode("Zg=A")); // a character after padding
    CHECK_FALSE(base64_decode("Zh==")); // the unused bits of "f" set: not canonical
    CHECK_FALSE(base64_decode("Zm9=")); // "fo" with its last bits set: not canonical
}

TEST_CASE("a post, rating and mine request carry base64 of their bytes") {
    const RecordSigner signer = signer_for(5);
    const Record record = signed_record(signer, 1, 2);
    const std::string post = post_frame(record);
    CHECK(post.starts_with(R"({"type":"post","record":")"));
    const std::string encoded = post.substr(std::string(R"({"type":"post","record":")").size());
    CHECK(decoded(encoded.substr(0, encoded.size() - 2)).size() == record_bytes);

    const Rating rating = rating_sign(signer, record_id(record), game);
    const std::string rate = rate_frame(rating);
    CHECK(rate.starts_with(R"({"type":"rate","rating":")"));
    const std::string author = mine_frame(signer.public_key());
    CHECK(author == R"({"type":"mine","author":")" + base64_encode(signer.public_key()) + R"("})");
}

TEST_CASE("a records frame gives each record with its good count") {
    const RecordSigner signer = signer_for(5);
    const Record record = signed_record(signer, 1, 2);
    const FloorFrame frame = frame_of(records_frame("records", record, 3));
    CHECK(frame.kind == FloorFrame::Kind::Records);
    REQUIRE(frame.records.size() == 1);
    CHECK(frame.records[0].record == record);
    CHECK(frame.records[0].good == 3);
}

TEST_CASE("a records frame over the sample's size is refused") {
    const RecordSigner signer = signer_for(5);
    const Record record = signed_record(signer, 1, 2);
    std::string frame = R"({"type":"records","records":[)";
    for (std::size_t i = 0; i <= sample_most; ++i)
        frame += std::string(i ? "," : "") + R"({"record":")" +
                 base64_encode(record_write(record)) + R"(","good":0})";
    frame += "]}";
    CHECK_FALSE(parse_floor_frame(frame));
}

TEST_CASE("a malformed record in a frame refuses the whole frame") {
    const RecordSigner signer = signer_for(5);
    auto bytes = record_write(signed_record(signer, 1, 2));
    std::string short_text = R"({"type":"records","records":[{"record":")" +
                             base64_encode(std::span<const std::uint8_t>(bytes).first(117)) +
                             R"(","good":0}]})";
    CHECK_FALSE(parse_floor_frame(short_text));

    bytes[0] = 2; // a version the client does not read
    const std::string wrong_version =
        R"({"type":"records","records":[{"record":")" + base64_encode(bytes) + R"(","good":0}]})";
    CHECK_FALSE(parse_floor_frame(wrong_version));

    const std::string bad_good = R"({"type":"records","records":[{"record":")" +
                                 base64_encode(record_write(signed_record(signer, 1, 2))) +
                                 R"(","good":-1}]})";
    CHECK_FALSE(parse_floor_frame(bad_good));
}

TEST_CASE("posted and rated carry an id of 16 lowercase hex digits, never zero, and a good count") {
    const FloorFrame posted = frame_of(R"({"type":"posted","id":"00000000000000ff","good":0})");
    CHECK(posted.kind == FloorFrame::Kind::Posted);
    CHECK(posted.id == 0xff);

    const FloorFrame rated = frame_of(R"({"type":"rated","id":"0123456789abcdef","good":4})");
    CHECK(rated.kind == FloorFrame::Kind::Rated);
    CHECK(rated.id == 0x0123456789abcdefu);
    CHECK(rated.good == 4);

    CHECK_FALSE(parse_floor_frame(R"({"type":"posted","id":"00000000000000FF","good":0})"));
    CHECK_FALSE(parse_floor_frame(R"({"type":"posted","id":"0000000000000000","good":0})"));
    CHECK_FALSE(parse_floor_frame(R"({"type":"posted","id":"ff","good":0})"));
    CHECK_FALSE(parse_floor_frame(R"({"type":"rated","id":"00000000000000ff","good":-2})"));
}

TEST_CASE("mine carries the author's records, at most ten") {
    const RecordSigner signer = signer_for(5);
    const FloorFrame frame = frame_of(records_frame("mine", signed_record(signer, 2, 3), 0));
    CHECK(frame.kind == FloorFrame::Kind::Mine);
    CHECK(frame.records.size() == 1);

    std::string many = R"({"type":"mine","records":[)";
    for (std::size_t i = 0; i <= mine_most; ++i)
        many += std::string(i ? "," : "") + R"({"record":")" +
                base64_encode(record_write(signed_record(signer, 2, 3))) + R"(","good":0})";
    CHECK_FALSE(parse_floor_frame(many + "]}"));
}

TEST_CASE(
    "error carries a code and a message; other frames and text that is not JSON are refused") {
    const FloorFrame error = frame_of(R"({"type":"error","code":"refused","message":"serial 2"})");
    CHECK(error.kind == FloorFrame::Kind::Error);
    CHECK(error.code == "refused");
    CHECK(error.message == "serial 2");
    CHECK_FALSE(parse_floor_frame(R"({"type":"error"})"));
    CHECK_FALSE(parse_floor_frame(
        R"({"type":"welcome","protocol":1,"peer":"0000000000000001","peers":[]})"));
    CHECK_FALSE(parse_floor_frame(""));
    CHECK_FALSE(parse_floor_frame(R"({"type":"posted")"));
    CHECK_FALSE(parse_floor_frame(std::string(frame_bytes + 1, ' ')));
}

TEST_CASE("a shown packet carries its count, then the records, and round-trips") {
    const RecordSigner signer = signer_for(6);
    const std::vector<Record> records = {signed_record(signer, 1, 1), signed_record(signer, 2, 2)};
    const auto bytes = shown_packet(records);
    CHECK(bytes.size() == 3 + 2 * record_bytes);
    CHECK(bytes[0] == 1);
    CHECK(bytes[1] == 1);
    CHECK(bytes[2] == 2);
    const GossipPacket parsed = packet_of(bytes);
    CHECK(parsed.kind == GossipPacket::Kind::Shown);
    CHECK(parsed.records == records);
}

TEST_CASE(
    "a shown packet with nothing to show is still a packet, and at most 255 records go in it") {
    const std::vector<std::uint8_t> empty = shown_packet({});
    CHECK(empty.size() == 3);
    CHECK(parse_gossip(bytes_of(empty)).has_value());

    const RecordSigner signer = signer_for(6);
    const std::vector<Record> many(shown_most + 5, signed_record(signer, 1, 1));
    const auto bytes = shown_packet(many);
    CHECK(bytes.size() == 3 + shown_most * record_bytes);
    CHECK(packet_of(bytes).records.size() == shown_most);
}

TEST_CASE("a new packet carries its hops and one record, and round-trips") {
    const RecordSigner signer = signer_for(6);
    const Record record = signed_record(signer, 3, 4);
    const auto bytes = new_packet(record, new_hops_most);
    CHECK(bytes.size() == 3 + record_bytes);
    const GossipPacket parsed = packet_of(bytes);
    CHECK(parsed.kind == GossipPacket::Kind::New);
    CHECK(parsed.hops == new_hops_most);
    REQUIRE(parsed.records.size() == 1);
    CHECK(parsed.records[0] == record);
}

TEST_CASE("a gossip packet with hops over 3, a bad kind or a bad size is refused") {
    const RecordSigner signer = signer_for(6);
    auto hops_four = new_packet(signed_record(signer, 3, 4), 0);
    hops_four[2] = 4;
    CHECK_FALSE(parse_gossip(bytes_of(hops_four)));

    auto kind_three = new_packet(signed_record(signer, 3, 4), 1);
    kind_three[1] = 3;
    CHECK_FALSE(parse_gossip(bytes_of(kind_three)));

    auto version_two = new_packet(signed_record(signer, 3, 4), 1);
    version_two[0] = 2;
    CHECK_FALSE(parse_gossip(bytes_of(version_two)));

    auto short_packet = new_packet(signed_record(signer, 3, 4), 1);
    short_packet.pop_back();
    CHECK_FALSE(parse_gossip(bytes_of(short_packet)));

    auto long_packet = new_packet(signed_record(signer, 3, 4), 1);
    long_packet.push_back(0);
    CHECK_FALSE(parse_gossip(bytes_of(long_packet)));

    CHECK_FALSE(parse_gossip(bytes_of(std::vector<std::uint8_t>{1, 1})));
}

TEST_CASE("a shown packet whose count and size disagree is refused") {
    const RecordSigner signer = signer_for(6);
    auto packet = shown_packet(std::vector<Record>{signed_record(signer, 1, 1)});
    packet[2] = 2; // claims two, carries one
    CHECK_FALSE(parse_gossip(bytes_of(packet)));
}
