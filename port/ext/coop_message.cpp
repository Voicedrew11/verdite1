#include "coop_message.h"

#include "coop_bytes.h"

#include <kf/platform/automap.h>

#include <algorithm>
#include <array>
#include <type_traits>

namespace kf::ext {
namespace {

constexpr std::uint8_t message_version = 2;
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

void write_profile(CoopByteWriter &out, const CoopProfile &profile) {
    out.name(profile.name);
    out.u8(profile.colour);
    out.u8(profile.helmet);
}

bool read_profile(CoopByteReader &in, CoopProfile &profile) {
    profile.name = in.name();
    profile.colour = in.u8();
    profile.helmet = in.u8();
    return in.ok() && coop_profile_valid(profile);
}

// One writer and one reader per message, in the variant's order.

void write(CoopByteWriter &out, const CoopHello &hello) {
    out.u64(hello.id);
    write_profile(out, hello.profile);
}

bool read(CoopByteReader &in, CoopHello &hello) {
    hello.id = in.u64();
    return read_profile(in, hello.profile) && hello.id != 0;
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

void write_save_point(CoopByteWriter &out, const CoopSavePoint &point) {
    out.u8(static_cast<std::uint8_t>(point.floor));
    out.u8(point.cell_x);
    out.u8(point.cell_z);
}

bool read_save_point(CoopByteReader &in, CoopSavePoint &point) {
    point.floor = in.u8();
    point.cell_x = in.u8();
    point.cell_z = in.u8();
    return in.ok() && coop_save_point_valid(point);
}

void write(CoopByteWriter &out, const CoopSaveRequest &request) {
    write_save_point(out, request.at);
}

bool read(CoopByteReader &in, CoopSaveRequest &request) {
    return read_save_point(in, request.at);
}

void write(CoopByteWriter &out, const CoopRoster &roster) {
    out.u8(static_cast<std::uint8_t>(roster.players.size()));
    for (const CoopRoster::Player &player : roster.players) {
        out.u64(player.id);
        out.u64(player.peer);
        write_profile(out, player.profile);
    }
}

bool read(CoopByteReader &in, CoopRoster &roster) {
    const std::uint8_t count = in.u8();
    if (!in.ok() || count > players_most)
        return false;
    for (std::uint8_t i = 0; i < count; ++i) {
        CoopRoster::Player player{in.u64(), in.u64(), {}};
        if (!read_profile(in, player.profile) || player.id == 0 || player.peer == 0)
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

void write(CoopByteWriter &out, const CoopActorHit &hit) {
    out.u8(static_cast<std::uint8_t>(hit.floor));
    out.u8(hit.slot);
    out.u8(hit.definition);
    out.u16(hit.base_power);
    for (const std::uint16_t damage : hit.damage)
        out.u16(damage);
    out.u16(hit.scale);
    out.u8(hit.credited ? 1 : 0);
}

bool read(CoopByteReader &in, CoopActorHit &hit) {
    hit.floor = in.u8();
    hit.slot = in.u8();
    hit.definition = in.u8();
    hit.base_power = in.u16();
    for (std::uint16_t &damage : hit.damage)
        damage = in.u16();
    hit.scale = in.u16();
    const std::uint8_t credited = in.u8();
    hit.credited = credited == 1;
    return in.ok() && floor_valid(hit.floor) && hit.slot < coop_actor_slots && credited <= 1;
}

void write(CoopByteWriter &out, const CoopActorAim &aim) {
    out.u8(static_cast<std::uint8_t>(aim.floor));
    out.u8(aim.slot);
    out.u8(aim.definition);
}

bool read(CoopByteReader &in, CoopActorAim &aim) {
    aim.floor = in.u8();
    aim.slot = in.u8();
    aim.definition = in.u8();
    return in.ok() && floor_valid(aim.floor) && aim.slot < coop_actor_slots;
}

void write(CoopByteWriter &out, const CoopKill &kill) {
    out.u8(static_cast<std::uint8_t>(kill.floor));
    out.u16(kill.experience);
}

bool read(CoopByteReader &in, CoopKill &kill) {
    kill.floor = in.u8();
    kill.experience = in.u16();
    return in.ok() && floor_valid(kill.floor);
}

void write(CoopByteWriter &out, const CoopSwing &swing) {
    out.u8(static_cast<std::uint8_t>(swing.floor));
    out.u8(swing.slot);
    out.u8(swing.definition);
    out.u16(swing.minimum_distance);
    out.u16(swing.maximum_distance);
    out.u16(static_cast<std::uint16_t>(swing.angle_offset));
    out.u16(static_cast<std::uint16_t>(swing.angle_tolerance));
    out.u16(swing.status);
}

bool read(CoopByteReader &in, CoopSwing &swing) {
    swing.floor = in.u8();
    swing.slot = in.u8();
    swing.definition = in.u8();
    swing.minimum_distance = in.u16();
    swing.maximum_distance = in.u16();
    swing.angle_offset = static_cast<std::int16_t>(in.u16());
    swing.angle_tolerance = static_cast<std::int16_t>(in.u16());
    swing.status = in.u16();
    return in.ok() && floor_valid(swing.floor) && swing.slot < coop_actor_slots;
}

void write(CoopByteWriter &out, const CoopPlayerHit &hit) {
    out.u8(static_cast<std::uint8_t>(hit.floor));
    for (const std::uint16_t field : {hit.cutting, hit.striking, hit.piercing, hit.status, hit.magic, hit.fire,
             hit.scale_q12, hit.multiplier_tenths})
        out.u16(field);
}

bool read(CoopByteReader &in, CoopPlayerHit &hit) {
    hit.floor = in.u8();
    for (std::uint16_t *field : {&hit.cutting, &hit.striking, &hit.piercing, &hit.status, &hit.magic, &hit.fire,
             &hit.scale_q12, &hit.multiplier_tenths})
        *field = in.u16();
    return in.ok() && floor_valid(hit.floor);
}

void write(CoopByteWriter &out, const CoopWipe &wipe) {
    out.u32(wipe.life);
    write(out, wipe.start);
}

bool read(CoopByteReader &in, CoopWipe &wipe) {
    wipe.life = in.u32();
    return read(in, wipe.start) && wipe.life != 0;
}

void write(CoopByteWriter &out, const CoopWiped &wiped) {
    out.u32(wiped.life);
}

bool read(CoopByteReader &in, CoopWiped &wiped) {
    wiped.life = in.u32();
    return in.ok() && wiped.life != 0;
}

void write(CoopByteWriter &out, const CoopRules &rules) {
    out.u8(static_cast<std::uint8_t>(rules.tick_rate));
    out.u8(static_cast<std::uint8_t>(rules.revive_wait_s));
}

bool read(CoopByteReader &in, CoopRules &rules) {
    rules.tick_rate = in.u8();
    rules.revive_wait_s = in.u8();
    return in.ok() && coop_tick_rate_valid(rules.tick_rate) && coop_revive_wait_valid(rules.revive_wait_s);
}

void write(CoopByteWriter &out, const CoopTurnedAway &away) {
    out.u8(static_cast<std::uint8_t>(away.why));
}

bool read(CoopByteReader &in, CoopTurnedAway &away) {
    const std::uint8_t why = in.u8();
    away.why = static_cast<CoopTurnedAwayWhy>(why);
    return in.ok() && why <= static_cast<std::uint8_t>(CoopTurnedAwayWhy::SentAway);
}

template <typename Message>
std::optional<CoopMessage> read_as(CoopByteReader &in) {
    Message message{};
    if (!read(in, message) || !in.done())
        return std::nullopt;
    return CoopMessage{std::move(message)};
}

using Reader = std::optional<CoopMessage> (*)(CoopByteReader &);
constexpr std::array<Reader, 25> readers = {&read_as<CoopHello>, &read_as<CoopWelcome>, &read_as<CoopCharacterUpdate>,
    &read_as<CoopAcknowledged>, &read_as<CoopPackRequest>, &read_as<CoopWearRequest>, &read_as<CoopPack>,
    &read_as<CoopFeedLine>, &read_as<CoopSaveRequest>, &read_as<CoopRoster>, &read_as<CoopFloorAt>,
    &read_as<CoopFloorOrders>, &read_as<CoopFloorRecord>, &read_as<CoopFloorSnapshot>, &read_as<CoopFloorTaken>,
    &read_as<CoopFloorWrites>, &read_as<CoopActorHit>, &read_as<CoopActorAim>, &read_as<CoopKill>,
    &read_as<CoopSwing>, &read_as<CoopPlayerHit>, &read_as<CoopWipe>, &read_as<CoopWiped>,
    &read_as<CoopRules>, &read_as<CoopTurnedAway>};
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
