#include "handover.h"

#include <doctest/doctest.h>

using namespace vo;

namespace {

Handover::Packet packet(int number) {
    return {static_cast<std::byte>(number)};
}

int number(const Handover::Packet &packet) {
    return static_cast<int>(packet.at(0));
}

std::vector<int> numbers(const std::vector<Handover::Packet> &packets) {
    std::vector<int> out;
    out.reserve(packets.size());
    for (const auto &each : packets)
        out.push_back(number(each));
    return out;
}

} // namespace

TEST_CASE("packets on the old connection are delivered as they come") {
    Handover handover;
    CHECK(numbers(handover.received(Handover::From::Old, packet(1))) == std::vector{1});
    CHECK(numbers(handover.received(Handover::From::Old, packet(2))) == std::vector{2});
}

TEST_CASE("the new connection's packets wait for the peer's moved, then follow the old ones") {
    Handover handover;
    CHECK(handover.received(Handover::From::New, packet(3)).empty());
    CHECK(handover.received(Handover::From::New, packet(4)).empty());
    CHECK(numbers(handover.received(Handover::From::Old, packet(1))) == std::vector{1});
    CHECK(numbers(handover.received(Handover::From::Old, packet(2))) == std::vector{2});
    CHECK(numbers(handover.peer_moved()) == std::vector{3, 4});
    CHECK(numbers(handover.received(Handover::From::New, packet(5))) == std::vector{5});
}

TEST_CASE("nothing is taken from the old connection after the peer's moved") {
    Handover handover;
    CHECK(handover.peer_moved().empty());
    CHECK(handover.received(Handover::From::Old, packet(9)).empty());
}

TEST_CASE("the peer closing the old connection finishes only once both sides moved") {
    Handover handover;
    CHECK_FALSE(handover.peer_closed_old());
    handover.sent_moved();
    CHECK_FALSE(handover.peer_closed_old());
    CHECK(handover.peer_moved().empty());
    CHECK(handover.peer_closed_old());
    CHECK(handover.finished());
}

TEST_CASE("the old connection closes only when both sides have moved and ours was seen") {
    Handover handover;
    CHECK_FALSE(handover.sending_on_new());
    handover.sent_moved();
    CHECK(handover.sending_on_new());
    CHECK_FALSE(handover.finished());
    CHECK(handover.peer_moved().empty());
    CHECK_FALSE(handover.finished());
    handover.peer_saw_moved();
    CHECK(handover.finished());
}

TEST_CASE("the bytes held from the new connection are counted until the peer's moved") {
    Handover handover;
    CHECK(handover.held_bytes() == 0);
    handover.received(Handover::From::Old, Handover::Packet(100));
    CHECK(handover.held_bytes() == 0);
    handover.received(Handover::From::New, Handover::Packet(300));
    handover.received(Handover::From::New, Handover::Packet(20));
    CHECK(handover.held_bytes() == 320);
    CHECK(handover.peer_moved().size() == 2);
    CHECK(handover.held_bytes() == 0);
    handover.received(Handover::From::New, Handover::Packet(50));
    CHECK(handover.held_bytes() == 0);
}
