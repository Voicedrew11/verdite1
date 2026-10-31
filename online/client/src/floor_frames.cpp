#include "floor_frames.h"

#include "json.h"
#include "signaling.h"

#include <algorithm>
#include <array>

namespace vo {
namespace {

constexpr std::string_view base64_alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr std::uint8_t gossip_version = 1;
constexpr std::uint8_t gossip_shown = 1;
constexpr std::uint8_t gossip_new = 2;
constexpr std::size_t gossip_head_bytes = 3; // version, kind, then a count or the hops
constexpr std::uint32_t good_most = 1 << 30;

// The 6-bit value of a base64 character, or -1.
int base64_digit(char c) {
    const std::size_t at = base64_alphabet.find(c);
    return at == std::string_view::npos ? -1 : static_cast<int>(at);
}

// Padding at the end of a base64 text: 0, 1 or 2 '=' characters.
std::size_t padding_of(std::string_view text) {
    std::size_t pad = 0;
    while (pad < 2 && pad < text.size() && text[text.size() - 1 - pad] == '=')
        ++pad;
    return pad;
}

// The four characters of one quartet as 24 bits, or nothing when one is not in place: padding is
// only the last quartet's last one or two characters, and the bits it leaves unused are zero.
std::optional<std::uint32_t> quartet_value(std::string_view quartet, std::size_t pad) {
    std::uint32_t value = 0;
    for (std::size_t k = 0; k < quartet.size(); ++k) {
        if (k >= quartet.size() - pad) {
            if (quartet[k] != '=')
                return std::nullopt;
            value <<= 6;
            continue;
        }
        const int digit = base64_digit(quartet[k]);
        if (digit < 0)
            return std::nullopt;
        value = value << 6 | static_cast<std::uint32_t>(digit);
    }
    const std::uint32_t unused = (std::uint32_t{1} << (8 * pad)) - 1;
    if (value & unused)
        return std::nullopt;
    return value;
}

std::optional<std::uint64_t> record_id_of(std::string_view hex) {
    if (hex.size() != 16)
        return std::nullopt;
    std::uint64_t id = 0;
    for (const char c : hex) {
        int digit = 0;
        if (c >= '0' && c <= '9')
            digit = c - '0';
        else if (c >= 'a' && c <= 'f')
            digit = c - 'a' + 10;
        else
            return std::nullopt;
        id = id << 4 | static_cast<std::uint64_t>(digit);
    }
    if (id == 0)
        return std::nullopt;
    return id;
}

std::optional<Record> record_of(const cJSON *item) {
    const auto encoded = json::string_field(item, "record");
    if (!encoded)
        return std::nullopt;
    const auto bytes = base64_decode(*encoded);
    if (!bytes || bytes->size() != record_bytes)
        return std::nullopt;
    return record_read(*bytes);
}

// A records or mine frame: each {record, good}, at most most of them.
std::optional<FloorFrame> parse_records(const cJSON *json, FloorFrame::Kind kind,
                                        std::size_t most) {
    const cJSON *array = cJSON_GetObjectItemCaseSensitive(json, "records");
    if (!cJSON_IsArray(array) || static_cast<std::size_t>(cJSON_GetArraySize(array)) > most)
        return std::nullopt;
    FloorFrame frame;
    frame.kind = kind;
    const cJSON *item = nullptr;
    cJSON_ArrayForEach(item, array) {
        auto record = record_of(item);
        const auto good = json::int_field(item, "good", 0, static_cast<int>(good_most));
        if (!record || !good)
            return std::nullopt;
        frame.records.push_back(CountedRecord{*record, static_cast<std::uint32_t>(*good)});
    }
    return frame;
}

// A posted or rated frame: a record id and its good count.
std::optional<FloorFrame> parse_receipt(const cJSON *json, FloorFrame::Kind kind) {
    const auto hex = json::string_field(json, "id");
    const auto good = json::int_field(json, "good", 0, static_cast<int>(good_most));
    const auto id = hex ? record_id_of(*hex) : std::nullopt;
    if (!id || !good)
        return std::nullopt;
    FloorFrame frame;
    frame.kind = kind;
    frame.id = *id;
    frame.good = static_cast<std::uint32_t>(*good);
    return frame;
}

std::optional<FloorFrame> parse_error(const cJSON *json) {
    auto code = json::string_field(json, "code");
    if (!code)
        return std::nullopt;
    FloorFrame frame;
    frame.kind = FloorFrame::Kind::Error;
    frame.code = std::move(*code);
    frame.message = json::string_field(json, "message").value_or("");
    return frame;
}

// The record's 118 bytes appended to out.
void append_record(std::vector<std::uint8_t> &out, const Record &record) {
    const auto bytes = record_write(record);
    out.insert(out.end(), bytes.begin(), bytes.end());
}

std::optional<GossipPacket> parse_shown(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < gossip_head_bytes)
        return std::nullopt;
    const std::size_t count = bytes[2];
    const auto body = bytes.subspan(gossip_head_bytes);
    if (body.size() != count * record_bytes)
        return std::nullopt;
    GossipPacket packet;
    packet.kind = GossipPacket::Kind::Shown;
    for (std::size_t i = 0; i < count; ++i) {
        auto record = record_read(body.subspan(i * record_bytes, record_bytes));
        if (!record)
            return std::nullopt;
        packet.records.push_back(*record);
    }
    return packet;
}

std::optional<GossipPacket> parse_new(std::span<const std::uint8_t> bytes) {
    if (bytes.size() != gossip_head_bytes + record_bytes)
        return std::nullopt;
    const std::uint8_t hops = bytes[2];
    if (hops > new_hops_most)
        return std::nullopt;
    auto record = record_read(bytes.subspan(gossip_head_bytes));
    if (!record)
        return std::nullopt;
    GossipPacket packet;
    packet.kind = GossipPacket::Kind::New;
    packet.records.push_back(*record);
    packet.hops = hops;
    return packet;
}

} // namespace

std::string base64_encode(std::span<const std::uint8_t> bytes) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t at = 0; at < bytes.size(); at += 3) {
        const std::size_t chunk = std::min<std::size_t>(3, bytes.size() - at);
        std::uint32_t value = 0;
        for (std::size_t k = 0; k < 3; ++k)
            value = value << 8 | (k < chunk ? bytes[at + k] : 0u);
        for (std::size_t k = 0; k < 4; ++k)
            out += k <= chunk ? base64_alphabet[(value >> (18 - 6 * k)) & 63] : '=';
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> base64_decode(std::string_view text) {
    if (text.size() % 4 != 0)
        return std::nullopt;
    std::vector<std::uint8_t> out;
    out.reserve(text.size() / 4 * 3);
    for (std::size_t at = 0; at < text.size(); at += 4) {
        const bool last = at + 4 == text.size();
        const std::size_t pad = last ? padding_of(text) : 0;
        const auto value = quartet_value(text.substr(at, 4), pad);
        if (!value)
            return std::nullopt;
        const std::array<std::uint8_t, 3> bytes = {
            static_cast<std::uint8_t>(*value >> 16),
            static_cast<std::uint8_t>(*value >> 8),
            static_cast<std::uint8_t>(*value),
        };
        out.insert(out.end(), bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(3 - pad));
    }
    return out;
}

std::string post_frame(const Record &record) {
    const auto bytes = record_write(record);
    return R"({"type":"post","record":")" + base64_encode(bytes) + R"("})";
}

std::string rate_frame(const Rating &rating) {
    const auto bytes = rating_write(rating);
    return R"({"type":"rate","rating":")" + base64_encode(bytes) + R"("})";
}

std::string mine_frame(const RecordKey &author) {
    return R"({"type":"mine","author":")" + base64_encode(author) + R"("})";
}

std::optional<FloorFrame> parse_floor_frame(std::string_view text) {
    if (text.size() > frame_bytes)
        return std::nullopt;
    const json::Object json(cJSON_ParseWithLength(text.data(), text.size()));
    if (!cJSON_IsObject(json.get()))
        return std::nullopt;
    const auto type = json::string_field(json.get(), "type");
    if (!type)
        return std::nullopt;
    if (*type == "records")
        return parse_records(json.get(), FloorFrame::Kind::Records, sample_most);
    if (*type == "mine")
        return parse_records(json.get(), FloorFrame::Kind::Mine, mine_most);
    if (*type == "posted")
        return parse_receipt(json.get(), FloorFrame::Kind::Posted);
    if (*type == "rated")
        return parse_receipt(json.get(), FloorFrame::Kind::Rated);
    if (*type == "error")
        return parse_error(json.get());
    return std::nullopt;
}

std::vector<std::uint8_t> shown_packet(std::span<const Record> records) {
    const std::size_t count = std::min(records.size(), shown_most);
    std::vector<std::uint8_t> out{gossip_version, gossip_shown, static_cast<std::uint8_t>(count)};
    out.reserve(gossip_head_bytes + count * record_bytes);
    for (const Record &record : records.first(count))
        append_record(out, record);
    return out;
}

std::vector<std::uint8_t> new_packet(const Record &record, std::uint8_t hops) {
    std::vector<std::uint8_t> out{gossip_version, gossip_new, hops};
    out.reserve(gossip_head_bytes + record_bytes);
    append_record(out, record);
    return out;
}

std::optional<GossipPacket> parse_gossip(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < gossip_head_bytes || bytes[0] != gossip_version)
        return std::nullopt;
    if (bytes[1] == gossip_shown)
        return parse_shown(bytes);
    if (bytes[1] == gossip_new)
        return parse_new(bytes);
    return std::nullopt;
}

} // namespace vo
