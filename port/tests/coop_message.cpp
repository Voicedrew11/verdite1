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
    const CoopHello hello = round_trip(CoopHello{0xfeedfacecafebeefull, "PLAYER1234", {4, 5}});
    CHECK(hello.id == 0xfeedfacecafebeefull);
    CHECK(hello.name == "PLAYER1234");
    CHECK(hello.fresh_state == std::vector<std::uint8_t>{4, 5});

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
    CHECK(round_trip(CoopWearRequest{worn}).worn == worn);

    CoopPack pack;
    pack.pack[20] = 1;
    pack.gold = 300;
    pack.players = {{1, worn, 0}, {2, coop_nothing_worn, 41}};
    const CoopPack pack_got = round_trip(pack);
    CHECK(pack_got.pack == pack.pack);
    CHECK(pack_got.gold == 300);
    REQUIRE(pack_got.players.size() == 2);
    CHECK(pack_got.players[0].worn == worn);
    CHECK(pack_got.players[1].id == 2);
    CHECK(pack_got.players[1].applied_through == 41);

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
    version[0] = 2;
    CHECK_FALSE(coop_message_decode(version));
    auto kind = encoded(CoopSaveRequest{});
    kind[1] = 9;
    CHECK_FALSE(coop_message_decode(kind));
}

TEST_CASE("an item, stack, floor, cell, cause or name out of range is refused") {
    CHECK_FALSE(coop_message_decode(encoded(CoopHello{1, "lower", {}})));
    CHECK_FALSE(coop_message_decode(encoded(CoopHello{0, "HOST", {}})));
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
    CHECK_FALSE(coop_message_decode(encoded(CoopWearRequest{worn})));
}

TEST_CASE("a welcome with a world too big for one message is nothing") {
    CoopWelcome big = welcome();
    big.world.assign(coop_message_most, 0);
    CHECK_FALSE(coop_message_encode(big));
}
