// libFuzzer: what another player in a room, or a stranger on the floor mesh, sends: a coop message
// (CONTROL), a pose and a floor frame's part (STATE), a message on the ground's payload and place
// (a record's), and the host's coop world file. The first byte says which. Each decoder must take
// any bytes without crashing, and a coop message it takes must encode and decode again.
#include "../../ext/coop_floor_frames.h"
#include "../../ext/coop_message.h"
#include "../../ext/coop_party.h"
#include "../../ext/message_reading.h"
#include "../../ext/messages.h"
#include "../../ext/online_pose.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>

namespace {

using namespace kf::ext;

[[noreturn]] void fail(const char *what) {
    std::fprintf(stderr, "%s\n", what);
    std::abort();
}

void coop_message(std::span<const std::uint8_t> bytes) {
    const auto message = coop_message_decode(bytes);
    if (!message)
        return;
    const auto encoded = coop_message_encode(*message);
    if (!encoded)
        fail("a coop message that decoded does not encode");
    if (!coop_message_decode(*encoded))
        fail("a coop message encoded from a decoded one does not decode");
}

void message_place(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 4)
        return;
    const auto place = static_cast<std::uint32_t>(bytes[0] | bytes[1] << 8 | bytes[2] << 16)
        | static_cast<std::uint32_t>(bytes[3]) << 24;
    (void)message_place_decode(place);
}

void world_file(std::span<const std::uint8_t> bytes) {
    const auto file = coop_world_decode(bytes);
    if (file && !coop_world_decode(coop_world_encode(*file)))
        fail("a coop world file encoded from a decoded one does not decode");
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    if (size == 0)
        return 0;
    const std::span<const std::uint8_t> bytes(data + 1, size - 1);
    switch (data[0] % 6) {
    case 0:
        coop_message(bytes);
        break;
    case 1:
        (void)online_pose_decode(bytes);
        break;
    case 2:
        (void)coop_frame_decode(bytes);
        break;
    case 3:
        if (const auto message = message_decode(bytes))
            (void)message_pieces(*message, kf::Language::English);
        break;
    case 4:
        message_place(bytes);
        break;
    default:
        world_file(bytes);
        break;
    }
    return 0;
}
