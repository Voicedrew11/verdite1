#include "coop_party.h"

#include "coop_bytes.h"

#include <kf/platform/automap.h>

#include <algorithm>
#include <limits>
#include <set>

namespace kf::ext {
namespace {

constexpr std::uint8_t short_sword = 0;       // KF_ITEM_SHORT_SWORD
constexpr std::uint8_t medicinal_herb = 43;   // KF_ITEM_MEDICINAL_HERB
constexpr std::uint32_t starting_gold = 150;  // PLAYER_STARTING_GOLD
constexpr std::size_t weapon_slot = 0;

// Each worn slot's items, first and past the last: the equipment menu's ranges (menu_select.cpp).
struct SlotItems { std::uint8_t first, end; };
constexpr std::array<SlotItems, coop_worn_slots> slot_items = {{
    {0, 13},    // weapons: the Short Sword .. the Moonlight Sword
    {13, 19},   // head: the Iron Mask .. the Mist Crown
    {19, 26},   // body: the Breastplate .. the Forest Armor
    {26, 32},   // shield: the Small Shield .. the Wind Guard
    {32, 35},   // arms: the Gauntlet .. the Stone Hand
    {35, 39},   // legs: the Iron Boots .. the Feather Boots
    {48, 52},   // accessory: the Light Ring .. the Two-Headed Dragon Ring
}};

constexpr std::array<std::uint8_t, 8> file_magic = {'K', 'F', 'C', 'O', 'O', 'P', 0, 0};
constexpr std::uint32_t file_version = 2;   // 1: no save point
constexpr std::size_t file_header_bytes = file_magic.size() + 12;
constexpr std::uint32_t crc32_reflected_polynomial = 0xedb88320u;

int clamp_floor(int floor) {
    return std::clamp(floor, 1, coop_floor_count);
}

std::uint32_t add_gold(std::uint32_t gold, std::int64_t count) {
    const std::int64_t most = std::numeric_limits<std::uint32_t>::max();
    return static_cast<std::uint32_t>(std::clamp<std::int64_t>(gold + count, 0, most));
}

std::uint32_t checksum(std::span<const std::uint8_t> bytes) {
    std::uint32_t crc = ~std::uint32_t{0};
    for (const std::uint8_t byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (crc32_reflected_polynomial & (0u - (crc & 1)));
    }
    return ~crc;
}

void write_state(CoopByteWriter &out, const CoopWorldState &state) {
    const CoopParty &party = state.party;
    out.u32(party.gold());
    out.raw(party.pack());
    out.u16(static_cast<std::uint16_t>(party.characters().size()));
    for (const CoopCharacter &character : party.characters()) {
        out.u64(character.id);
        out.name(character.name);
        out.u8(static_cast<std::uint8_t>(character.highest_floor));
        out.raw(character.wore);
        out.blob(character.state);
    }
    out.blob(state.world);
}

std::optional<CoopCharacter> read_character(CoopByteReader &in) {
    CoopCharacter character;
    character.id = in.u64();
    character.name = in.name();
    character.highest_floor = in.u8();
    const auto wore = in.raw(coop_worn_slots);
    if (!in.ok())
        return std::nullopt;
    std::ranges::copy(wore, character.wore.begin());
    character.state = in.blob();
    if (!in.ok() || character.id == 0 || !coop_name_valid(character.name) || !coop_worn_valid(character.wore) ||
        character.highest_floor < 1 || character.highest_floor > coop_floor_count)
        return std::nullopt;
    return character;
}

std::optional<CoopWorldState> read_state(CoopByteReader &in) {
    const std::uint32_t gold = in.u32();
    CoopItems pack{};
    const auto pack_bytes = in.raw(coop_item_count);
    if (!in.ok())
        return std::nullopt;
    std::ranges::copy(pack_bytes, pack.begin());
    if (!coop_pack_valid(pack))
        return std::nullopt;
    const std::uint16_t count = in.u16();
    std::vector<CoopCharacter> characters;
    std::set<CoopPlayerId> ids;
    for (std::uint16_t i = 0; i < count; ++i) {
        auto character = read_character(in);
        if (!character || !ids.insert(character->id).second)
            return std::nullopt;
        characters.push_back(std::move(*character));
    }
    std::vector<std::uint8_t> world = in.blob();
    if (!in.ok())
        return std::nullopt;
    return CoopWorldState{CoopParty(std::move(characters), pack, gold), std::move(world)};
}

// After the last save, version 2 on: whether there is a save point, and where.
void write_save_point(CoopByteWriter &out, const std::optional<CoopSavePoint> &point) {
    out.u8(point ? 1 : 0);
    if (!point)
        return;
    out.u8(static_cast<std::uint8_t>(point->floor));
    out.u8(point->cell_x);
    out.u8(point->cell_z);
}

bool read_save_point(CoopByteReader &in, std::optional<CoopSavePoint> &point) {
    const std::uint8_t has = in.u8();
    if (!in.ok() || has > 1)
        return false;
    if (has == 0)
        return true;
    CoopSavePoint read;
    read.floor = in.u8();
    read.cell_x = in.u8();
    read.cell_z = in.u8();
    point = read;
    return in.ok() && coop_save_point_valid(read);
}

} // namespace

bool coop_save_point_valid(const CoopSavePoint &point) {
    return point.floor >= 1 && point.floor <= coop_floor_count && point.cell_x < automap_columns &&
           point.cell_z < automap_rows;
}

bool coop_name_valid(std::string_view name) {
    if (name.empty() || name.size() > coop_name_most)
        return false;
    return std::ranges::all_of(name, [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); });
}

bool coop_pack_valid(const CoopItems &pack) {
    return std::ranges::all_of(pack, [](std::uint8_t count) { return count <= coop_stack_most; });
}

bool coop_item_fits(std::size_t slot, std::uint8_t item) {
    if (slot >= coop_worn_slots)
        return false;
    return item == coop_no_item || (item >= slot_items[slot].first && item < slot_items[slot].end);
}

bool coop_worn_valid(const CoopWorn &worn) {
    for (std::size_t slot = 0; slot < coop_worn_slots; ++slot)
        if (!coop_item_fits(slot, worn[slot]))
            return false;
    return true;
}

CoopParty::CoopParty(std::vector<CoopCharacter> characters, const CoopItems &pack, std::uint32_t gold)
    : characters_(std::move(characters)), pack_(pack), gold_(gold) {}

const CoopCharacter &CoopParty::join(CoopPlayerId id, std::string_view name) {
    CoopCharacter *character = find(id);
    if (!character) {
        CoopCharacter fresh{id, std::string(name), {}, coop_nothing_worn, 1};
        fresh.wore[weapon_slot] = short_sword;
        characters_.push_back(std::move(fresh));
        character = &characters_.back();
        add_to_pack(short_sword, 1);
        add_to_pack(medicinal_herb, 1);
        gold_ = add_gold(gold_, starting_gold);
    }
    character->name = name;
    if (!here(id)) {
        in_room_.push_back(id);
        const CoopWorn wore = character->wore;
        character->wore = coop_nothing_worn;
        wear(id, wore);
    }
    return *character;
}

void CoopParty::leave(CoopPlayerId id) {
    std::erase(in_room_, id);
}

std::int32_t CoopParty::apply(CoopPlayerId by, CoopPackChange change) {
    if (change.item == coop_no_item) {
        const std::uint32_t before = gold_;
        gold_ = add_gold(gold_, change.count);
        return static_cast<std::int32_t>(static_cast<std::int64_t>(gold_) - before);
    }
    if (change.item >= coop_item_count)
        return 0;
    const int before = pack_[change.item];
    const int lowest = std::min(before, worn_by_others(by, change.item));
    const int highest = std::max(before, coop_stack_most);
    const int after = static_cast<int>(std::clamp<std::int64_t>(std::int64_t{before} + change.count, lowest, highest));
    pack_[change.item] = static_cast<std::uint8_t>(after);
    return after - before;
}

CoopWorn CoopParty::wear(CoopPlayerId id, const CoopWorn &worn) {
    CoopCharacter *character = find(id);
    if (!character || !here(id))
        return coop_nothing_worn;
    CoopWorn now = coop_nothing_worn;
    for (std::size_t slot = 0; slot < coop_worn_slots; ++slot) {
        const std::uint8_t item = worn[slot];
        if (item != coop_no_item && coop_item_fits(slot, item) && pack_[item] > worn_by_others(id, item))
            now[slot] = item;
    }
    character->wore = now;
    return now;
}

void CoopParty::store(CoopPlayerId id, std::span<const std::uint8_t> state, int highest_floor) {
    CoopCharacter *character = find(id);
    if (!character)
        return;
    character->state.assign(state.begin(), state.end());
    character->highest_floor = std::max(character->highest_floor, clamp_floor(highest_floor));
}

int CoopParty::free_for(CoopPlayerId id, std::uint8_t item) const {
    if (item >= coop_item_count)
        return 0;
    return std::max(0, pack_[item] - worn_by_others(id, item));
}

const CoopItems &CoopParty::pack() const {
    return pack_;
}

std::uint32_t CoopParty::gold() const {
    return gold_;
}

int CoopParty::highest_floor() const {
    int highest = 1;
    for (const CoopCharacter &character : characters_)
        highest = std::max(highest, character.highest_floor);
    return highest;
}

std::span<const CoopCharacter> CoopParty::characters() const {
    return characters_;
}

std::span<const CoopPlayerId> CoopParty::in_room() const {
    return in_room_;
}

CoopCharacter *CoopParty::find(CoopPlayerId id) {
    const auto found = std::ranges::find(characters_, id, &CoopCharacter::id);
    return found == characters_.end() ? nullptr : &*found;
}

bool CoopParty::here(CoopPlayerId id) const {
    return std::ranges::find(in_room_, id) != in_room_.end();
}

int CoopParty::worn_by_others(CoopPlayerId id, std::uint8_t item) const {
    int worn = 0;
    for (const CoopCharacter &character : characters_)
        if (character.id != id && here(character.id))
            worn += static_cast<int>(std::ranges::count(character.wore, item));
    return worn;
}

void CoopParty::add_to_pack(std::uint8_t item, int count) {
    pack_[item] = static_cast<std::uint8_t>(std::min(coop_stack_most, pack_[item] + count));
}

CoopPackMirror::CoopPackMirror(const CoopItems &pack, std::uint32_t gold)
    : host_pack_(pack), host_gold_(gold), pack_(pack), gold_(gold) {}

std::vector<CoopPackMirror::Sent> CoopPackMirror::take_changes(const CoopItems &game_pack, std::uint32_t game_gold) {
    std::vector<Sent> sent;
    for (std::size_t item = 0; item < coop_item_count; ++item) {
        const int count = game_pack[item] - pack_[item];
        if (count != 0)
            sent.push_back({next_number_++, {static_cast<std::uint8_t>(item), count}});
    }
    const std::int64_t gold = std::int64_t{game_gold} - gold_;
    if (gold != 0) {
        const std::int64_t most = std::numeric_limits<std::int32_t>::max();
        sent.push_back({next_number_++, {coop_no_item, static_cast<std::int32_t>(std::clamp(gold, -most, most))}});
    }
    pending_.insert(pending_.end(), sent.begin(), sent.end());
    pack_ = game_pack;
    gold_ = game_gold;
    return sent;
}

void CoopPackMirror::receive(const CoopItems &pack, std::uint32_t gold, std::uint32_t applied_through) {
    host_pack_ = pack;
    host_gold_ = gold;
    std::erase_if(pending_, [&](const Sent &sent) { return sent.number <= applied_through; });
    show_pending();
}

const CoopItems &CoopPackMirror::pack() const {
    return pack_;
}

std::uint32_t CoopPackMirror::gold() const {
    return gold_;
}

void CoopPackMirror::show_pending() {
    pack_ = host_pack_;
    gold_ = host_gold_;
    for (const Sent &sent : pending_) {
        if (sent.change.item == coop_no_item) {
            gold_ = add_gold(gold_, sent.change.count);
            continue;
        }
        const int count = pack_[sent.change.item] + sent.change.count;
        pack_[sent.change.item] = static_cast<std::uint8_t>(std::clamp(count, 0, coop_stack_most));
    }
}

std::vector<std::uint8_t> coop_world_encode(const CoopWorldFile &file) {
    CoopByteWriter payload;
    write_state(payload, file.live);
    payload.u8(file.last_save ? 1 : 0);
    if (file.last_save)
        write_state(payload, *file.last_save);
    write_save_point(payload, file.save_point);
    CoopByteWriter out;
    out.raw(file_magic);
    out.u32(file_version);
    out.u32(static_cast<std::uint32_t>(payload.bytes().size()));
    out.u32(checksum(payload.bytes()));
    out.raw(payload.bytes());
    return std::move(out.bytes());
}

std::optional<CoopWorldFile> coop_world_decode(std::span<const std::uint8_t> bytes) {
    CoopByteReader header(bytes);
    if (!std::ranges::equal(header.raw(file_magic.size()), file_magic))
        return std::nullopt;
    const std::uint32_t version = header.u32();
    const std::uint32_t size = header.u32();
    const std::uint32_t sum = header.u32();
    if (!header.ok() || version < 1 || version > file_version || size != bytes.size() - file_header_bytes)
        return std::nullopt;
    const auto payload = bytes.subspan(file_header_bytes);
    if (sum != checksum(payload))
        return std::nullopt;
    CoopByteReader in(payload);
    auto live = read_state(in);
    if (!live)
        return std::nullopt;
    CoopWorldFile file{std::move(*live), std::nullopt, std::nullopt};
    const std::uint8_t has_last_save = in.u8();
    if (has_last_save > 1)
        return std::nullopt;
    if (has_last_save) {
        file.last_save = read_state(in);
        if (!file.last_save)
            return std::nullopt;
    }
    if (version >= 2 && !read_save_point(in, file.save_point))
        return std::nullopt;
    if (!in.done())
        return std::nullopt;
    return file;
}

}
