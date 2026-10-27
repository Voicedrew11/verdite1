#include "coop_message.h"

#include "coop_bytes.h"

#include <kf/platform/automap.h>

#include <algorithm>
#include <array>
#include <type_traits>

namespace kf::ext {
namespace {

constexpr std::uint8_t message_version = 1;
constexpr std::uint8_t cause_count = 4;
constexpr std::size_t players_most = 64;

bool floor_valid(int floor) {
    return floor >= 1 && floor <= coop_floor_count;
}

bool change_valid(const CoopPackChange &change) {
    return change.item < coop_item_count || change.item == coop_no_item;
}

void write_worn(CoopByteWriter &out, const CoopWorn &worn) {
    out.raw(worn);
}

bool read_worn(CoopByteReader &in, CoopWorn &worn) {
    const auto bytes = in.raw(worn.size());
    if (!in.ok())
        return false;
    std::ranges::copy(bytes, worn.begin());
    return coop_worn_valid(worn);
}

bool read_pack(CoopByteReader &in, CoopItems &pack) {
    const auto bytes = in.raw(pack.size());
    if (!in.ok())
        return false;
    std::ranges::copy(bytes, pack.begin());
    return coop_pack_valid(pack);
}

void write_change(CoopByteWriter &out, const CoopPackChange &change) {
    out.u8(change.item);
    out.s32(change.count);
}

bool read_change(CoopByteReader &in, CoopPackChange &change) {
    change.item = in.u8();
    change.count = in.s32();
    return in.ok() && change_valid(change);
}

bool read_cause(CoopByteReader &in, CoopCause &cause) {
    const std::uint8_t value = in.u8();
    cause = static_cast<CoopCause>(value);
    return in.ok() && value < cause_count;
}

// One writer and one reader per message, in the variant's order.

void write(CoopByteWriter &out, const CoopHello &hello) {
    out.u64(hello.id);
    out.name(hello.name);
}

bool read(CoopByteReader &in, CoopHello &hello) {
    hello.id = in.u64();
    hello.name = in.name();
    return in.ok() && hello.id != 0 && coop_name_valid(hello.name);
}

void write(CoopByteWriter &out, const CoopWelcome &welcome) {
    out.blob(welcome.state);
    write_worn(out, welcome.worn);
    out.raw(welcome.pack);
    out.u32(welcome.gold);
    out.u8(static_cast<std::uint8_t>(welcome.highest_floor));
    out.blob(welcome.world);
    out.u8(static_cast<std::uint8_t>(welcome.floor));
    out.u8(welcome.cell_x);
    out.u8(welcome.cell_z);
}

bool read(CoopByteReader &in, CoopWelcome &welcome) {
    welcome.state = in.blob();
    if (!read_worn(in, welcome.worn) || !read_pack(in, welcome.pack))
        return false;
    welcome.gold = in.u32();
    welcome.highest_floor = in.u8();
    welcome.world = in.blob();
    welcome.floor = in.u8();
    welcome.cell_x = in.u8();
    welcome.cell_z = in.u8();
    return in.ok() && floor_valid(welcome.highest_floor) && floor_valid(welcome.floor) &&
           welcome.cell_x < automap_columns && welcome.cell_z < automap_rows;
}

void write(CoopByteWriter &out, const CoopCharacterUpdate &update) {
    out.u32(update.number);
    out.blob(update.state);
    out.u8(static_cast<std::uint8_t>(update.highest_floor));
}

bool read(CoopByteReader &in, CoopCharacterUpdate &update) {
    update.number = in.u32();
    update.state = in.blob();
    update.highest_floor = in.u8();
    return in.ok() && floor_valid(update.highest_floor);
}

void write(CoopByteWriter &out, const CoopAcknowledged &acknowledged) {
    out.u32(acknowledged.number);
}

bool read(CoopByteReader &in, CoopAcknowledged &acknowledged) {
    acknowledged.number = in.u32();
    return in.ok();
}

void write(CoopByteWriter &out, const CoopPackRequest &request) {
    out.u32(request.number);
    write_change(out, request.change);
    out.u8(static_cast<std::uint8_t>(request.cause));
}

bool read(CoopByteReader &in, CoopPackRequest &request) {
    request.number = in.u32();
    return read_change(in, request.change) && read_cause(in, request.cause);
}

void write(CoopByteWriter &out, const CoopWearRequest &request) {
    out.u32(request.number);
    write_worn(out, request.worn);
}

bool read(CoopByteReader &in, CoopWearRequest &request) {
    request.number = in.u32();
    return read_worn(in, request.worn);
}

void write(CoopByteWriter &out, const CoopPack &pack) {
    out.raw(pack.pack);
    out.u32(pack.gold);
    out.u8(static_cast<std::uint8_t>(pack.highest_floor));
    out.u8(static_cast<std::uint8_t>(pack.players.size()));
    for (const CoopPack::Player &player : pack.players) {
        out.u64(player.id);
        write_worn(out, player.worn);
        out.u32(player.applied_through);
        out.u32(player.worn_through);
    }
}

bool read(CoopByteReader &in, CoopPack &pack) {
    if (!read_pack(in, pack.pack))
        return false;
    pack.gold = in.u32();
    pack.highest_floor = in.u8();
    const std::uint8_t count = in.u8();
    if (!in.ok() || !floor_valid(pack.highest_floor) || count > players_most)
        return false;
    for (std::uint8_t i = 0; i < count; ++i) {
        CoopPack::Player player{in.u64(), coop_nothing_worn, 0, 0};
        if (!read_worn(in, player.worn))
            return false;
        player.applied_through = in.u32();
        player.worn_through = in.u32();
        if (!in.ok() || player.id == 0)
            return false;
        pack.players.push_back(player);
    }
    return true;
}

void write(CoopByteWriter &out, const CoopFeedLine &line) {
    out.u64(line.by);
    out.u8(static_cast<std::uint8_t>(line.cause));
    write_change(out, line.change);
}

bool read(CoopByteReader &in, CoopFeedLine &line) {
    line.by = in.u64();
    return read_cause(in, line.cause) && read_change(in, line.change);
}

void write(CoopByteWriter &, const CoopSaveRequest &) {}

bool read(CoopByteReader &, CoopSaveRequest &) {
    return true;
}

void write(CoopByteWriter &out, const CoopRoster &roster) {
    out.u8(static_cast<std::uint8_t>(roster.players.size()));
    for (const CoopRoster::Player &player : roster.players) {
        out.u64(player.id);
        out.u64(player.peer);
    }
}

bool read(CoopByteReader &in, CoopRoster &roster) {
    const std::uint8_t count = in.u8();
    if (!in.ok() || count > players_most)
        return false;
    for (std::uint8_t i = 0; i < count; ++i) {
        const CoopRoster::Player player{in.u64(), in.u64()};
        if (!in.ok() || player.id == 0 || player.peer == 0)
            return false;
        roster.players.push_back(player);
    }
    return true;
}

void write(CoopByteWriter &out, const CoopFloorAt &at) {
    out.u8(static_cast<std::uint8_t>(at.floor));
    out.u64(at.left_floor_with);
}

bool read(CoopByteReader &in, CoopFloorAt &at) {
    at.floor = in.u8();
    at.left_floor_with = in.u64();
    return in.ok() && floor_valid(at.floor);
}

// An order: its kind (CoopFloorOrder's index), the floor, then its two players.
void write_order(CoopByteWriter &out, const CoopFloorOrder &order) {
    out.u8(static_cast<std::uint8_t>(order.index()));
    std::visit(
        [&](const auto &kind) {
            using Kind = std::decay_t<decltype(kind)>;
            out.u8(static_cast<std::uint8_t>(kind.floor));
            if constexpr (std::is_same_v<Kind, CoopFloorOwnership>) {
                out.u64(kind.owner);
                out.u64(kind.heir);
            } else if constexpr (std::is_same_v<Kind, CoopRunFromRecord>) {
                out.u64(kind.to);
                out.u64(0);
            } else if constexpr (std::is_same_v<Kind, CoopTakeFloor>) {
                out.u64(kind.from);
                out.u64(kind.to);
            } else {
                out.u64(kind.owner);
                out.u64(kind.to);
            }
        },
        order);
}

std::optional<CoopFloorOrder> read_order(CoopByteReader &in) {
    const std::uint8_t kind = in.u8();
    const int floor = in.u8();
    const CoopPlayerId first = in.u64();
    const CoopPlayerId second = in.u64();
    if (!in.ok() || !floor_valid(floor))
        return std::nullopt;
    switch (kind) {
    case 0:
        return CoopFloorOwnership{floor, first, second};
    case 1:
        return CoopRunFromRecord{floor, first};
    case 2:
        return CoopTakeFloor{floor, first, second};
    case 3:
        return CoopShowFloor{floor, first, second};
    default:
        return std::nullopt;
    }
}

void write(CoopByteWriter &out, const CoopFloorOrders &orders) {
    out.u8(static_cast<std::uint8_t>(orders.orders.size()));
    for (const CoopFloorOrder &order : orders.orders)
        write_order(out, order);
}

bool read(CoopByteReader &in, CoopFloorOrders &orders) {
    const std::uint8_t count = in.u8();
    if (!in.ok())
        return false;
    for (std::uint8_t i = 0; i < count; ++i) {
        auto order = read_order(in);
        if (!order)
            return false;
        orders.orders.push_back(*order);
    }
    return true;
}

void write(CoopByteWriter &out, const CoopFloorRecord &record) {
    out.u8(static_cast<std::uint8_t>(record.floor));
    out.blob(record.record);
}

bool read(CoopByteReader &in, CoopFloorRecord &record) {
    record.floor = in.u8();
    record.record = in.blob();
    return in.ok() && floor_valid(record.floor);
}

void write(CoopByteWriter &out, const CoopFloorSnapshot &snapshot) {
    out.u8(static_cast<std::uint8_t>(snapshot.floor));
    out.u8(static_cast<std::uint8_t>(snapshot.purpose));
    out.blob(snapshot.record);
    out.blob(snapshot.live);
}

bool read(CoopByteReader &in, CoopFloorSnapshot &snapshot) {
    snapshot.floor = in.u8();
    const std::uint8_t purpose = in.u8();
    snapshot.purpose = static_cast<CoopSnapshotFor>(purpose);
    snapshot.record = in.blob();
    snapshot.live = in.blob();
    return in.ok() && floor_valid(snapshot.floor) && purpose <= static_cast<std::uint8_t>(CoopSnapshotFor::Show);
}

void write(CoopByteWriter &out, const CoopFloorTaken &taken) {
    out.u8(static_cast<std::uint8_t>(taken.floor));
}

bool read(CoopByteReader &in, CoopFloorTaken &taken) {
    taken.floor = in.u8();
    return in.ok() && floor_valid(taken.floor);
}

void write(CoopByteWriter &out, const CoopFloorWrites &writes) {
    out.u8(static_cast<std::uint8_t>(writes.floor));
    out.blob(writes.writes);
}

bool read(CoopByteReader &in, CoopFloorWrites &writes) {
    writes.floor = in.u8();
    writes.writes = in.blob();
    return in.ok() && floor_valid(writes.floor);
}

template <typename Message>
std::optional<CoopMessage> read_as(CoopByteReader &in) {
    Message message{};
    if (!read(in, message) || !in.done())
        return std::nullopt;
    return CoopMessage{std::move(message)};
}

using Reader = std::optional<CoopMessage> (*)(CoopByteReader &);
constexpr std::array<Reader, 16> readers = {&read_as<CoopHello>, &read_as<CoopWelcome>, &read_as<CoopCharacterUpdate>,
    &read_as<CoopAcknowledged>, &read_as<CoopPackRequest>, &read_as<CoopWearRequest>, &read_as<CoopPack>,
    &read_as<CoopFeedLine>, &read_as<CoopSaveRequest>, &read_as<CoopRoster>, &read_as<CoopFloorAt>,
    &read_as<CoopFloorOrders>, &read_as<CoopFloorRecord>, &read_as<CoopFloorSnapshot>, &read_as<CoopFloorTaken>,
    &read_as<CoopFloorWrites>};
static_assert(readers.size() == std::variant_size_v<CoopMessage>);

} // namespace

std::optional<std::vector<std::uint8_t>> coop_message_encode(const CoopMessage &message) {
    CoopByteWriter out;
    out.u8(message_version);
    out.u8(static_cast<std::uint8_t>(message.index()));
    std::visit([&](const auto &kind) { write(out, kind); }, message);
    if (out.bytes().size() > coop_message_most)
        return std::nullopt;
    return std::move(out.bytes());
}

std::optional<CoopMessage> coop_message_decode(std::span<const std::uint8_t> bytes) {
    if (bytes.size() > coop_message_most)
        return std::nullopt;
    CoopByteReader in(bytes);
    const std::uint8_t version = in.u8();
    const std::uint8_t kind = in.u8();
    if (!in.ok() || version != message_version || kind >= readers.size())
        return std::nullopt;
    return readers[kind](in);
}

}
