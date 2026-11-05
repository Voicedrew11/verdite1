#include "../ext/coop_message.h"

#include <doctest/doctest.h>

using namespace kf::ext;

namespace {

template <typename Message>
Message round_trip(const Message &sent) {
    const auto bytes = coop_message_encode(sent);
    REQUIRE(bytes.has_value());
    const auto received = coop_message_decode(bytes.value_or(std::vector<std::uint8_t>{}));
    REQUIRE(received.has_value());
    const CoopMessage message = received.value_or(CoopMessage{});
    REQUIRE(std::holds_alternative<Message>(message));
    return std::get<Message>(message);
}

std::vector<std::uint8_t> encoded(const CoopMessage &message) {
    return coop_message_encode(message).value_or(std::vector<std::uint8_t>{});
}

CoopWelcome welcome() {
    CoopWelcome sent;
    sent.state = {1, 2, 3};
    sent.worn = coop_nothing_worn;
    sent.worn[0] = 2;
    sent.pack[43] = 99;
    sent.gold = 123456;
    sent.highest_floor = 4;
    sent.world = std::vector<std::uint8_t>(9000, 7);
    sent.floor = 3;
    sent.cell_x = 99;
    sent.cell_z = 12;
    return sent;
}

} // namespace

TEST_CASE("every message sent is the message received") {
    const CoopHello hello = round_trip(CoopHello{0xfeedfacecafebeefull, {"PLAYER1234", 5, 0}});
    CHECK(hello.id == 0xfeedfacecafebeefull);
    CHECK(hello.profile == CoopProfile{"PLAYER1234", 5, 0});

    const CoopWelcome got = round_trip(welcome());
    CHECK(got.state == welcome().state);
    CHECK(got.worn == welcome().worn);
    CHECK(got.pack == welcome().pack);
    CHECK(got.gold == 123456);
    CHECK(got.highest_floor == 4);
    CHECK(got.world == welcome().world);
    CHECK(got.floor == 3);
    CHECK(got.cell_x == 99);
    CHECK(got.cell_z == 12);

    const CoopCharacterUpdate update = round_trip(CoopCharacterUpdate{77, {9}, 5});
    CHECK(update.number == 77);
    CHECK(update.state == std::vector<std::uint8_t>{9});
    CHECK(update.highest_floor == 5);
    CHECK(round_trip(CoopAcknowledged{78}).number == 78);

    const CoopPackRequest request = round_trip(CoopPackRequest{5, {coop_no_item, -150}, CoopCause::Bought});
    CHECK(request.number == 5);
    CHECK(request.change.item == coop_no_item);
    CHECK(request.change.count == -150);
    CHECK(request.cause == CoopCause::Bought);

    CoopWorn worn = coop_nothing_worn;
    worn[2] = 20;
    const CoopWearRequest wear = round_trip(CoopWearRequest{6, worn});
    CHECK(wear.number == 6);
    CHECK(wear.worn == worn);

    CoopPack pack;
    pack.pack[20] = 1;
    pack.gold = 300;
    pack.highest_floor = 2;
    pack.players = {{1, worn, 0, 0}, {2, coop_nothing_worn, 41, 7}};
    const CoopPack pack_got = round_trip(pack);
    CHECK(pack_got.pack == pack.pack);
    CHECK(pack_got.gold == 300);
    CHECK(pack_got.highest_floor == 2);
    REQUIRE(pack_got.players.size() == 2);
    CHECK(pack_got.players[0].worn == worn);
    CHECK(pack_got.players[1].id == 2);
    CHECK(pack_got.players[1].applied_through == 41);
    CHECK(pack_got.players[1].worn_through == 7);

    const CoopFeedLine line = round_trip(CoopFeedLine{2, CoopCause::Used, {43, -1}});
    CHECK(line.by == 2);
    CHECK(line.cause == CoopCause::Used);
    CHECK(line.change.item == 43);
    CHECK(line.change.count == -1);
    round_trip(CoopSaveRequest{});
}

TEST_CASE("a message cut short, with bytes left over, or of another version or kind is refused") {
    const auto good = encoded(welcome());
    for (std::size_t size = 0; size < good.size(); size += 97)
        CHECK_FALSE(coop_message_decode(std::span(good).first(size)));
    CHECK_FALSE(coop_message_decode(std::span(good).first(good.size() - 1)));
    auto longer = good;
    longer.push_back(0);
    CHECK_FALSE(coop_message_decode(longer));
    auto version = good;
    version[0] = 1;
    CHECK_FALSE(coop_message_decode(version));
    auto kind = encoded(CoopSaveRequest{});
    kind[1] = 16;
    CHECK_FALSE(coop_message_decode(kind));
}

TEST_CASE("an item, stack, floor, cell, cause or name out of range is refused") {
    CHECK_FALSE(coop_message_decode(encoded(CoopHello{1, {"lower", 0, 0}})));
    CHECK_FALSE(coop_message_decode(encoded(CoopHello{0, {"HOST", 0, 0}})));
    CHECK_FALSE(coop_message_decode(encoded(CoopHello{1, {"HOST", coop_colour_count, 0}})));
    CHECK_FALSE(coop_message_decode(encoded(CoopHello{1, {"HOST", 0, kf::online_player_helmet_count}})));
    CHECK_FALSE(coop_message_decode(encoded(CoopHello{1, {"HOST", 0, 0, kf::online_player_symbol_count}})));
    CHECK_FALSE(coop_message_decode(encoded(CoopRoster{{{1, 2, {"HOST", coop_colour_count, 0}}}})));
    CHECK_FALSE(coop_message_decode(encoded(CoopPackRequest{1, {80, 1}, CoopCause::Found})));
    CHECK_FALSE(coop_message_decode(encoded(CoopCharacterUpdate{1, {}, 6})));
    CHECK_FALSE(coop_message_decode(encoded(CoopCharacterUpdate{1, {}, 0})));
    auto cause = encoded(CoopPackRequest{1, {43, 1}, CoopCause::Found});
    cause.back() = 4;
    CHECK_FALSE(coop_message_decode(cause));
    CoopWelcome stack = welcome();
    stack.pack[43] = 100;
    CHECK_FALSE(coop_message_decode(encoded(stack)));
    CoopWelcome cell = welcome();
    cell.cell_x = 100;
    CHECK_FALSE(coop_message_decode(encoded(cell)));
    CoopWelcome floor = welcome();
    floor.floor = 6;
    CHECK_FALSE(coop_message_decode(encoded(floor)));
    CoopWorn worn = coop_nothing_worn;
    worn[1] = 80;
    CHECK_FALSE(coop_message_decode(encoded(CoopWearRequest{1, worn})));
    worn[1] = 43;   // an herb on the head
    CHECK_FALSE(coop_message_decode(encoded(CoopWearRequest{1, worn})));
    CoopPack floor_pack;
    floor_pack.highest_floor = 0;
    CHECK_FALSE(coop_message_decode(encoded(floor_pack)));
}

TEST_CASE("the floor messages sent are the messages received") {
    const CoopRoster roster = round_trip(CoopRoster{{{1, 0x10, {"HOST", 0, 0}}, {2, 0x20, {"GUEST", 6, 0}}}});
    REQUIRE(roster.players.size() == 2);
    CHECK(roster.players[1].id == 2);
    CHECK(roster.players[1].peer == 0x20);
    CHECK(roster.players[1].profile == CoopProfile{"GUEST", 6, 0});

    const CoopFloorAt at = round_trip(CoopFloorAt{4, 9});
    CHECK(at.floor == 4);
    CHECK(at.left_floor_with == 9);

    const CoopFloorOrders orders = round_trip(CoopFloorOrders{{CoopFloorOwnership{2, 1, 3}, CoopRunFromRecord{3, 4},
        CoopTakeFloor{5, 6, 7}, CoopShowFloor{1, 8, 9}}});
    REQUIRE(orders.orders.size() == 4);
    const auto &row = std::get<CoopFloorOwnership>(orders.orders[0]);
    CHECK((row.floor == 2 && row.owner == 1 && row.heir == 3));
    const auto &run = std::get<CoopRunFromRecord>(orders.orders[1]);
    CHECK((run.floor == 3 && run.to == 4));
    const auto &take = std::get<CoopTakeFloor>(orders.orders[2]);
    CHECK((take.floor == 5 && take.from == 6 && take.to == 7));
    const auto &show = std::get<CoopShowFloor>(orders.orders[3]);
    CHECK((show.floor == 1 && show.owner == 8 && show.to == 9));

    const CoopFloorRecord record = round_trip(CoopFloorRecord{3, {1, 2, 3}});
    CHECK(record.floor == 3);
    CHECK(record.record == std::vector<std::uint8_t>{1, 2, 3});

    const CoopFloorSnapshot snapshot =
        round_trip(CoopFloorSnapshot{2, CoopSnapshotFor::Handover, {4}, std::vector<std::uint8_t>(40000, 5)});
    CHECK(snapshot.floor == 2);
    CHECK(snapshot.purpose == CoopSnapshotFor::Handover);
    CHECK(snapshot.record == std::vector<std::uint8_t>{4});
    CHECK(snapshot.live.size() == 40000);

    CHECK(round_trip(CoopFloorTaken{5}).floor == 5);
    const CoopFloorWrites writes = round_trip(CoopFloorWrites{1, {7, 8}});
    CHECK(writes.floor == 1);
    CHECK(writes.writes == std::vector<std::uint8_t>{7, 8});
}

TEST_CASE("a floor message with a floor, order or purpose out of range is refused") {
    CHECK_FALSE(coop_message_decode(encoded(CoopFloorAt{0, 0})));
    CHECK_FALSE(coop_message_decode(encoded(CoopFloorAt{6, 0})));
    CHECK_FALSE(coop_message_decode(encoded(CoopFloorTaken{6})));
    CHECK_FALSE(coop_message_decode(encoded(CoopFloorRecord{0, {}})));
    CHECK_FALSE(coop_message_decode(encoded(CoopRoster{{{0, 1, {"HOST", 0, 0}}}})));
    auto order = encoded(CoopFloorOrders{{CoopRunFromRecord{1, 2}}});
    order[3] = 4;   // the order's kind
    CHECK_FALSE(coop_message_decode(order));
    auto purpose = encoded(CoopFloorSnapshot{1, CoopSnapshotFor::Show, {}, {}});
    purpose[3] = 3;
    CHECK_FALSE(coop_message_decode(purpose));
}

TEST_CASE("a hit, an aim and a kill come back as they were sent") {
    const CoopActorHit hit = round_trip(CoopActorHit{3, 127, 9, 40, {1, 2, 3, 4, 5}, 4096, true});
    CHECK((hit.floor == 3 && hit.slot == 127 && hit.definition == 9 && hit.base_power == 40));
    CHECK(hit.damage == std::array<std::uint16_t, 5>{1, 2, 3, 4, 5});
    CHECK((hit.scale == 4096 && hit.credited));
    const CoopActorAim aim = round_trip(CoopActorAim{2, 5, 6});
    CHECK((aim.floor == 2 && aim.slot == 5 && aim.definition == 6));
    const CoopKill kill = round_trip(CoopKill{4, 300});
    CHECK((kill.floor == 4 && kill.experience == 300));
}

TEST_CASE("a swing and a hit on a player come back as they were sent") {
    const CoopSwing swing = round_trip(CoopSwing{2, 40, 7, 100, 2500, -512, 300, 4});
    CHECK((swing.floor == 2 && swing.slot == 40 && swing.definition == 7));
    CHECK((swing.minimum_distance == 100 && swing.maximum_distance == 2500));
    CHECK((swing.angle_offset == -512 && swing.angle_tolerance == 300 && swing.status == 4));
    const CoopPlayerHit hit = round_trip(CoopPlayerHit{5, 1, 2, 3, 8, 4, 5, 4096, 10});
    CHECK((hit.floor == 5 && hit.cutting == 1 && hit.striking == 2 && hit.piercing == 3 && hit.status == 8));
    CHECK((hit.magic == 4 && hit.fire == 5 && hit.scale_q12 == 4096 && hit.multiplier_tenths == 10));
    CHECK_FALSE(coop_message_decode(encoded(CoopSwing{1, 128, 0, 0, 0, 0, 0, 0})));
    CHECK_FALSE(coop_message_decode(encoded(CoopPlayerHit{6})));
}

TEST_CASE("a hit or an aim on a slot past the actors is refused") {
    CHECK_FALSE(coop_message_decode(encoded(CoopActorHit{1, 128, 0, 0, {}, 0, false})));
    CHECK_FALSE(coop_message_decode(encoded(CoopActorAim{1, 200, 0})));
    CHECK_FALSE(coop_message_decode(encoded(CoopKill{0, 1})));
    CHECK(coop_message_decode(encoded(CoopKill{1, coop_kill_experience_most})));
    CHECK_FALSE(coop_message_decode(encoded(CoopKill{1, coop_kill_experience_most + 1})));
    auto credited = encoded(CoopActorHit{1, 1, 0, 0, {}, 0, true});
    credited.back() = 2;
    CHECK_FALSE(coop_message_decode(credited));
}

TEST_CASE("a welcome with a world too big for one message is nothing") {
    CoopWelcome big = welcome();
    big.world.assign(coop_message_most, 0);
    CHECK_FALSE(coop_message_encode(big));
}

TEST_CASE("a save request, a wipe and its answer come back as they were sent") {
    const CoopSaveRequest save = round_trip(CoopSaveRequest{{5, 39, 47}});
    CHECK((save.at.floor == 5 && save.at.cell_x == 39 && save.at.cell_z == 47));
    const CoopWipe wipe = round_trip(CoopWipe{3, welcome()});
    CHECK(wipe.life == 3);
    CHECK(wipe.start.state == welcome().state);
    CHECK(wipe.start.world == welcome().world);
    CHECK((wipe.start.floor == 3 && wipe.start.cell_x == 99 && wipe.start.cell_z == 12));
    CHECK(round_trip(CoopWiped{7}).life == 7);
}

TEST_CASE("a save request off the map, or a wipe of life 0, is refused") {
    CHECK_FALSE(coop_message_decode(encoded(CoopSaveRequest{{6, 1, 1}})));
    CHECK_FALSE(coop_message_decode(encoded(CoopSaveRequest{{1, 100, 1}})));
    CHECK_FALSE(coop_message_decode(encoded(CoopSaveRequest{{1, 1, 100}})));
    CHECK_FALSE(coop_message_decode(encoded(CoopWipe{0, welcome()})));
    CHECK_FALSE(coop_message_decode(encoded(CoopWiped{0})));
}

TEST_CASE("the room's rules and a guest turned away are sent as they are") {
    CHECK(round_trip(CoopRules{30, 10}) == CoopRules{30, 10});
    CHECK(round_trip(CoopTurnedAway{CoopTurnedAwayWhy::SentAway}).why == CoopTurnedAwayWhy::SentAway);
    CHECK(round_trip(CoopTurnedAway{CoopTurnedAwayWhy::RoomClosed}).why == CoopTurnedAwayWhy::RoomClosed);
}

TEST_CASE("a rule outside its choices, or an unknown reason, is not a message") {
    auto rules = encoded(CoopRules{15, 30});
    rules[rules.size() - 2] = 16;   // tick rate
    CHECK_FALSE(coop_message_decode(rules).has_value());
    rules = encoded(CoopRules{15, 30});
    rules.back() = 0;               // revive wait
    CHECK_FALSE(coop_message_decode(rules).has_value());
    auto away = encoded(CoopTurnedAway{CoopTurnedAwayWhy::SentAway});
    away.back() = 2;
    CHECK_FALSE(coop_message_decode(away).has_value());
}
