#include "../ext/coop_party.h"

#include <doctest/doctest.h>

using namespace kf::ext;

namespace {

constexpr std::uint8_t short_sword = 0;
constexpr std::uint8_t knight_plate = 20;
constexpr std::uint8_t herb = 43;
constexpr std::size_t body_slot = 2;

CoopWorn wearing(std::uint8_t weapon, std::uint8_t body = coop_no_item) {
    CoopWorn worn = coop_nothing_worn;
    worn[0] = weapon;
    worn[body_slot] = body;
    return worn;
}

} // namespace

TEST_CASE("a new player brings the starting kit and wears its sword") {
    CoopParty party;
    const CoopCharacter &host = party.join(1, "HOST");
    CHECK(host.state.empty());
    CHECK(host.wore == wearing(short_sword));
    party.join(2, "GUEST");
    CHECK(party.pack()[short_sword] == 2);
    CHECK(party.pack()[herb] == 2);
    CHECK(party.gold() == 300);
    // Back again: no second kit.
    party.leave(2);
    party.join(2, "GUEST");
    CHECK(party.pack()[short_sword] == 2);
    CHECK(party.gold() == 300);
}

TEST_CASE("what another player wears cannot be taken or worn") {
    CoopParty party;
    party.join(1, "HOST");
    party.join(2, "GUEST");
    CHECK(party.apply(1, {knight_plate, 1}) == 1);
    CHECK(party.wear(1, wearing(short_sword, knight_plate)) == wearing(short_sword, knight_plate));
    CHECK(party.free_for(2, knight_plate) == 0);
    CHECK(party.free_for(1, knight_plate) == 1);
    CHECK(party.wear(2, wearing(short_sword, knight_plate)) == wearing(short_sword));
    CHECK(party.apply(2, {knight_plate, -1}) == 0);
    CHECK(party.pack()[knight_plate] == 1);
}

TEST_CASE("worn gear goes back to the pack when its wearer leaves, and back on if still free") {
    CoopParty party;
    party.join(1, "HOST");
    party.join(2, "GUEST");
    party.apply(1, {knight_plate, 1});
    party.wear(1, wearing(short_sword, knight_plate));
    party.leave(1);
    CHECK(party.free_for(2, knight_plate) == 1);
    CHECK(party.in_room().size() == 1);
    // Still free when they come back: they wear it again.
    CHECK(party.join(1, "HOST").wore == wearing(short_sword, knight_plate));
    party.leave(1);
    party.wear(2, wearing(short_sword, knight_plate));
    CHECK(party.join(1, "HOST").wore == wearing(short_sword));
}

TEST_CASE("gear goes on only in its own slot") {
    CoopParty party;
    party.join(1, "HOST");
    party.apply(1, {knight_plate, 1});
    CoopWorn wrong = coop_nothing_worn;
    wrong[0] = knight_plate;
    wrong[1] = herb;
    CHECK(party.wear(1, wrong) == coop_nothing_worn);
    CHECK(coop_item_fits(6, 51));
    CHECK_FALSE(coop_item_fits(6, 52));
    CHECK_FALSE(coop_item_fits(7, coop_no_item));
}

TEST_CASE("the pack stops at a full stack and at nothing, the purse at nothing") {
    CoopParty party;
    party.join(1, "HOST");
    CHECK(party.apply(1, {herb, 200}) == 98);
    CHECK(party.pack()[herb] == 99);
    CHECK(party.apply(1, {herb, -150}) == -99);
    CHECK(party.apply(1, {coop_no_item, -1000}) == -150);
    CHECK(party.gold() == 0);
    CHECK(party.apply(1, {coop_no_item, 25}) == 25);
    CHECK(party.apply(1, {200, 1}) == 0);
}

TEST_CASE("two players using the last herb within a round trip both heal, and the pack stays at 0") {
    CoopParty party;
    party.join(1, "HOST");
    party.join(2, "GUEST");
    party.apply(1, {herb, -1});
    CoopPackMirror guest(party.pack(), party.gold());
    CHECK(guest.pack()[herb] == 1);
    party.apply(1, {herb, -1});   // the host uses the last herb
    CHECK(party.pack()[herb] == 0);
    CoopItems game = guest.pack();
    game[herb] = 0;   // the guest used the herb it saw
    const auto sent = guest.take_changes(game, guest.gold());
    REQUIRE(sent.size() == 1);
    CHECK(party.apply(2, sent[0].change) == 0);
    guest.receive(party.pack(), party.gold(), sent[0].number);
    CHECK(guest.pack()[herb] == 0);
}

TEST_CASE("the guest's mirror keeps its own changes on top until the host has them") {
    CoopItems host{};
    host[herb] = 3;
    CoopPackMirror mirror(host, 100);
    CoopItems game = host;
    game[herb] = 2;
    const auto first = mirror.take_changes(game, 90);
    REQUIRE(first.size() == 2);
    CHECK(first[0].change.item == herb);
    CHECK(first[0].change.count == -1);
    CHECK(first[1].change.item == coop_no_item);
    CHECK(first[1].change.count == -10);
    CHECK(mirror.take_changes(game, 90).empty());
    // The host's pack from before those changes, with someone else's herb in it.
    host[herb] = 4;
    mirror.receive(host, 100, 0);
    CHECK(mirror.pack()[herb] == 3);
    CHECK(mirror.gold() == 90);
    mirror.receive(host, 90, first[1].number);
    CHECK(mirror.pack()[herb] == 4);
    CHECK(mirror.gold() == 90);
}

TEST_CASE("the highest floor is the highest any character has reached, and never falls") {
    CoopParty party;
    party.join(1, "HOST");
    party.join(2, "GUEST");
    const std::vector<std::uint8_t> state = {1, 2, 3};
    party.store(2, state, 4);
    party.store(2, state, 2);
    party.store(1, state, 9);
    CHECK(party.highest_floor() == 5);
    CHECK(party.characters()[1].highest_floor == 4);
    party.store(7, state, 3);
    CHECK(party.characters().size() == 2);
}

TEST_CASE("a coop world written is the world read") {
    CoopParty party;
    party.join(1, "HOST");
    party.join(0x1234567890abcdefull, "PLAYER1234");
    party.apply(1, {knight_plate, 1});
    party.wear(1, wearing(short_sword, knight_plate));
    party.store(1, std::vector<std::uint8_t>{7}, 3);
    CoopWorldFile file{{party, {5, 6, 7}}, CoopWorldState{CoopParty({}, CoopItems{}, 42), {}}};
    const auto decoded = coop_world_decode(coop_world_encode(file));
    REQUIRE(decoded.has_value());
    const CoopWorldFile read = decoded.value_or(CoopWorldFile{});
    const CoopParty &back = read.live.party;
    CHECK(back.pack() == party.pack());
    CHECK(back.gold() == party.gold());
    CHECK(back.highest_floor() == 3);
    CHECK(back.in_room().empty());
    REQUIRE(back.characters().size() == 2);
    CHECK(back.characters()[0].wore == wearing(short_sword, knight_plate));
    CHECK(back.characters()[0].state == std::vector<std::uint8_t>{7});
    CHECK(back.characters()[1].id == 0x1234567890abcdefull);
    CHECK(back.characters()[1].name == "PLAYER1234");
    CHECK(read.live.world == std::vector<std::uint8_t>{5, 6, 7});
    REQUIRE(read.last_save.has_value());
    CHECK(read.last_save.value_or(CoopWorldState{}).party.gold() == 42);
    const auto without = coop_world_decode(coop_world_encode({{party, {}}, std::nullopt}));
    REQUIRE(without.has_value());
    CHECK_FALSE(without.value_or(CoopWorldFile{}).last_save.has_value());
}

TEST_CASE("a coop world keeps where its last save was made") {
    CoopParty party;
    party.join(1, "HOST");
    CoopWorldFile file{{party, {1}}, CoopWorldState{party, {2}}, CoopSavePoint{3, 45, 67}};
    const auto decoded = coop_world_decode(coop_world_encode(file));
    REQUIRE(decoded.has_value());
    const auto kept = decoded.value_or(CoopWorldFile{}).save_point;
    REQUIRE(kept.has_value());
    const CoopSavePoint point = kept.value_or(CoopSavePoint{});
    CHECK((point.floor == 3 && point.cell_x == 45 && point.cell_z == 67));
    file.save_point = CoopSavePoint{6, 0, 0};
    CHECK_FALSE(coop_world_decode(coop_world_encode(file)));
    file.save_point = CoopSavePoint{1, 100, 0};
    CHECK_FALSE(coop_world_decode(coop_world_encode(file)));
}

TEST_CASE("a coop world keeps its times played") {
    CoopParty party;
    party.join(1, "HOST");
    CoopWorldFile file{{party, {1}}, std::nullopt};
    file.last_played_s = 1800000000;
    file.played_ms = 3723000;
    const auto decoded = coop_world_decode(coop_world_encode(file));
    REQUIRE(decoded.has_value());
    const CoopWorldFile read = decoded.value_or(CoopWorldFile{});
    CHECK(read.last_played_s == 1800000000);
    CHECK(read.played_ms == 3723000);
}

TEST_CASE("a coop world of version 2 is read, with its times not known") {
    CoopParty party;
    party.join(1, "HOST");
    CoopWorldFile file{{party, {1}}, std::nullopt};
    file.save_point = CoopSavePoint{2, 3, 4};
    auto bytes = coop_world_encode(file);
    bytes.resize(bytes.size() - 16);
    const auto put_u32 = [&](std::size_t at, std::uint32_t value) {
        for (int i = 0; i < 4; ++i)
            bytes[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(value >> (8 * i));
    };
    constexpr std::size_t header = 20;
    std::uint32_t crc = ~0u;
    for (std::size_t at = header; at < bytes.size(); ++at) {
        crc ^= bytes[at];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    put_u32(8, 2);
    put_u32(12, static_cast<std::uint32_t>(bytes.size() - header));
    put_u32(16, ~crc);
    const auto decoded = coop_world_decode(bytes);
    REQUIRE(decoded.has_value());
    const CoopWorldFile read = decoded.value_or(CoopWorldFile{});
    CHECK(read.save_point.has_value());
    CHECK(read.last_played_s == 0);
    CHECK(read.played_ms == 0);
}

TEST_CASE("a coop world of version 1 is read, with no save point") {
    CoopParty party;
    party.join(1, "HOST");
    auto bytes = coop_world_encode({{party, {1}}, CoopWorldState{party, {2}}, std::nullopt});
    // Version 1 had no save point or times: its payload ends at the last save, with the header to match.
    bytes.resize(bytes.size() - 17);
    const auto put_u32 = [&](std::size_t at, std::uint32_t value) {
        for (int i = 0; i < 4; ++i)
            bytes[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(value >> (8 * i));
    };
    constexpr std::size_t header = 20;
    std::uint32_t crc = ~0u;
    for (std::size_t at = header; at < bytes.size(); ++at) {
        crc ^= bytes[at];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    put_u32(8, 1);
    put_u32(12, static_cast<std::uint32_t>(bytes.size() - header));
    put_u32(16, ~crc);
    const auto decoded = coop_world_decode(bytes);
    REQUIRE(decoded.has_value());
    const CoopWorldFile read = decoded.value_or(CoopWorldFile{});
    CHECK(read.last_save.has_value());
    CHECK_FALSE(read.save_point.has_value());
}

TEST_CASE("a coop world that is not one is refused") {
    CoopParty party;
    party.join(1, "HOST");
    const auto good = coop_world_encode({{party, {1}}, std::nullopt});
    REQUIRE(coop_world_decode(good));
    for (std::size_t at = 0; at < good.size(); ++at) {
        auto bad = good;
        bad[at] ^= 0x40;
        CHECK_FALSE(coop_world_decode(bad));
    }
    for (std::size_t size = 0; size < good.size(); ++size)
        CHECK_FALSE(coop_world_decode(std::span(good).first(size)));
    auto longer = good;
    longer.push_back(0);
    CHECK_FALSE(coop_world_decode(longer));
}

TEST_CASE("a coop world with a stack over 99, a bad name, a shared id or a floor past 5 is refused") {
    const auto refused = [](std::vector<CoopCharacter> characters, CoopItems pack) {
        return !coop_world_decode(coop_world_encode({{CoopParty(std::move(characters), pack, 0), {}}, std::nullopt}));
    };
    const CoopCharacter good{1, "HOST", {}, coop_nothing_worn, 1};
    CHECK_FALSE(refused({good}, {}));
    CoopItems over{};
    over[herb] = 100;
    CHECK(refused({good}, over));
    CHECK(refused({{1, "host", {}, coop_nothing_worn, 1}}, {}));
    CHECK(refused({{1, "", {}, coop_nothing_worn, 1}}, {}));
    CHECK(refused({{1, "ABCDEFGHIJKLM", {}, coop_nothing_worn, 1}}, {}));
    CHECK(refused({good, good}, {}));
    CHECK(refused({{0, "HOST", {}, coop_nothing_worn, 1}}, {}));
    CHECK(refused({{1, "HOST", {}, coop_nothing_worn, 6}}, {}));
    CHECK(refused({{1, "HOST", {}, wearing(80), 1}}, {}));
    CHECK(refused({{1, "HOST", {}, wearing(herb), 1}}, {}));
}
