// libFuzzer: what the service and the floor mesh's strangers send the client: the service's frames
// (a room's and a floor's records socket's), base64, records and the mesh's gossip packets, which
// then go into a record pool as the board puts them. The first byte says which. Nothing may crash,
// and whatever a parser takes must be within the limits its header gives.
#include "floor_frames.h"
#include "record.h"
#include "signaling.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string_view>

namespace {

[[noreturn]] void fail(const char *what) {
    std::fprintf(stderr, "%s\n", what);
    std::abort();
}

std::string_view text_of(std::span<const std::uint8_t> bytes) {
    return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}

void service_frame(std::span<const std::uint8_t> bytes) {
    const auto frame = vo::parse_service_frame(text_of(bytes));
    if (frame && frame->rooms.size() > vo::listed_rooms_most)
        fail("a rooms frame over its most");
}

void floor_frame(std::span<const std::uint8_t> bytes) {
    const auto frame = vo::parse_floor_frame(text_of(bytes));
    if (frame && frame->records.size() > vo::sample_most)
        fail("a records frame over its most");
}

void base64(std::span<const std::uint8_t> bytes) {
    const auto decoded = vo::base64_decode(text_of(bytes));
    if (decoded && vo::base64_decode(vo::base64_encode(*decoded)) != decoded)
        fail("base64 does not encode back to what it decoded");
}

void gossip(std::span<const std::uint8_t> bytes) {
    const auto packet = vo::parse_gossip(bytes);
    if (!packet)
        return;
    if (packet->kind == vo::GossipPacket::Kind::New && packet->records.size() != 1)
        fail("a new packet without exactly one record");
    vo::RecordPool pool{"kf1"};
    for (const vo::Record &record : packet->records) {
        if (record.payload_size > vo::record_payload_bytes)
            fail("a record over its payload's most");
        (void)pool.add(record);
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    if (size == 0)
        return 0;
    const std::span<const std::uint8_t> bytes(data + 1, size - 1);
    switch (data[0] % 4) {
    case 0:
        service_frame(bytes);
        break;
    case 1:
        floor_frame(bytes);
        break;
    case 2:
        base64(bytes);
        break;
    default:
        gossip(bytes);
        break;
    }
    return 0;
}
