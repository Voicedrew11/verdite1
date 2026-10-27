#include "signaling.h"

#include <doctest/doctest.h>

using namespace vo;

namespace {

// The frame, which must parse.
ServiceFrame parsed(std::string_view text) {
    const auto frame = parse_service_frame(text);
    REQUIRE(frame.has_value());
    return frame.value_or(ServiceFrame{});
}

} // namespace

TEST_CASE("a peer id is 16 lowercase hex digits, never zero") {
    CHECK(parse_peer("00000000000000ff") == 0xffu);
    CHECK(parse_peer("ffffffffffffffff") == ~PeerId{0});
    CHECK(peer_hex(0x0123456789abcdefu) == "0123456789abcdef");
    CHECK_FALSE(parse_peer("0000000000000000"));
    CHECK_FALSE(parse_peer("00000000000000FF"));
    CHECK_FALSE(parse_peer("ff"));
    CHECK_FALSE(parse_peer("00000000000000fff"));
    CHECK_FALSE(parse_peer("000000000000000g"));
}

TEST_CASE("a room's address follows the service's scheme and checks the names") {
    CHECK(room_url("https://online.example.net/", "kf1", "sign-1", "kf1-0.1.0+ab") ==
          "wss://online.example.net/v1/rooms/kf1/sign-1?protocol=1&build=kf1-0.1.0%2Bab");
    CHECK(room_url("http://127.0.0.1:8787", "kf1", "r", "b") ==
          "ws://127.0.0.1:8787/v1/rooms/kf1/r?protocol=1&build=b");
    CHECK_FALSE(room_url("ftp://x", "kf1", "r", "b"));
    CHECK_FALSE(room_url("https://x", "KF1", "r", "b"));
    CHECK_FALSE(room_url("https://x", "kf1", "a/b", "b"));
    CHECK_FALSE(room_url("https://x", "kf1", "r", "b&protocol=2"));
    CHECK_FALSE(room_url("https://x", "kf1", "", "b"));
    CHECK_FALSE(room_url("https://x", "kf1", std::string(65, 'r'), "b"));
}

TEST_CASE("a listed room needs a public code and a host name; the list's address is checked too") {
    CHECK(room_url("https://x", "kf1", "PABCDEFG", "b", "Knight 7") ==
          "wss://x/v1/rooms/kf1/PABCDEFG?protocol=1&build=b&listed=1&host=Knight%207");
    CHECK_FALSE(room_url("https://x", "kf1", "ABCDEF", "b", "Knight"));
    CHECK_FALSE(room_url("https://x", "kf1", "PABCDEF0", "b", "Knight"));
    CHECK_FALSE(room_url("https://x", "kf1", "PABCDEFG", "b", "Knight&x=1"));
    CHECK_FALSE(room_url("https://x", "kf1", "PABCDEFG", "b", std::string(17, 'k')));
    CHECK(lobby_url("http://127.0.0.1:8787/", "kf1", "kf1-0.1+a") ==
          "ws://127.0.0.1:8787/v1/lobby/kf1?protocol=1&build=kf1-0.1%2Ba");
    CHECK_FALSE(lobby_url("https://x", "kf1", "b c"));
}

TEST_CASE("rooms lists the public rooms, every row checked") {
    const ServiceFrame frame = parsed(
        R"({"type":"rooms","rooms":[{"room":"PABCDEFG","host":"Knight 7","floor":3,"players":2,"most":4}]})");
    CHECK(frame.kind == ServiceFrame::Kind::Rooms);
    REQUIRE(frame.rooms.size() == 1);
    CHECK(frame.rooms[0].room == "PABCDEFG");
    CHECK(frame.rooms[0].host == "Knight 7");
    CHECK(frame.rooms[0].floor == 3);
    CHECK(frame.rooms[0].players == 2);
    CHECK(frame.rooms[0].most == 4);
    CHECK(parsed(R"({"type":"rooms","rooms":[]})").rooms.empty());
    CHECK_FALSE(parse_service_frame(R"({"type":"rooms","rooms":[{"room":"a/b","host":"h","floor":1,"players":1,"most":4}]})"));
    CHECK_FALSE(parse_service_frame(R"({"type":"rooms","rooms":[{"room":"r","host":"<b>","floor":1,"players":1,"most":4}]})"));
    CHECK_FALSE(parse_service_frame(R"({"type":"rooms","rooms":[{"room":"r","host":"h","floor":100,"players":1,"most":4}]})"));
    CHECK_FALSE(parse_service_frame(R"({"type":"rooms","rooms":[{"room":"r","host":"h","floor":1,"players":1,"most":0}]})"));
    CHECK_FALSE(parse_service_frame(R"({"type":"rooms","rooms":{}})"));
    std::string many = R"({"type":"rooms","rooms":[)";
    for (std::size_t i = 0; i <= listed_rooms_most; ++i)
        many += std::string(i ? "," : "") + R"({"room":"r","host":"h","floor":1,"players":1,"most":4})";
    CHECK_FALSE(parse_service_frame(many + "]}"));
}

TEST_CASE("describe carries the floor, at most the most") {
    CHECK(describe_frame(3) == R"({"type":"describe","floor":3})");
    CHECK(describe_frame(500) == R"({"type":"describe","floor":99})");
}

TEST_CASE("welcome names you and the peers already in the room") {
    const ServiceFrame frame = parsed(
        R"({"type":"welcome","protocol":1,"peer":"000000000000000a","peers":["000000000000000b"]})");
    CHECK(frame.kind == ServiceFrame::Kind::Welcome);
    CHECK(frame.protocol == 1);
    CHECK(frame.peer == 0xa);
    REQUIRE(frame.peers.size() == 1);
    CHECK(frame.peers[0] == 0xb);
}

TEST_CASE("a welcome listing yourself or a malformed peer is refused") {
    CHECK_FALSE(parse_service_frame(
        R"({"type":"welcome","protocol":1,"peer":"000000000000000a","peers":["000000000000000a"]})"));
    CHECK_FALSE(parse_service_frame(
        R"({"type":"welcome","protocol":1,"peer":"000000000000000a","peers":[7]})"));
    CHECK_FALSE(parse_service_frame(R"({"type":"welcome","protocol":1.5,"peer":"000000000000000a","peers":[]})"));
}

TEST_CASE("turn flattens each server's urls, keeping the credential") {
    const ServiceFrame frame = parsed(
        R"({"type":"turn","seconds":3600,"iceServers":[{"urls":"stun:s.example:3478"},
            {"urls":["turn:t.example:3478?transport=udp","turns:t.example:443?transport=tcp"],
             "username":"u","credential":"c"}]})");
    CHECK(frame.seconds == 3600);
    REQUIRE(frame.ice_servers.size() == 3);
    CHECK(frame.ice_servers[0].username.empty());
    CHECK(frame.ice_servers[1].url == "turn:t.example:3478?transport=udp");
    CHECK(frame.ice_servers[2].username == "u");
    CHECK(frame.ice_servers[2].credential == "c");
}

TEST_CASE("turn with no servers is local testing; a bad lifetime is refused") {
    const ServiceFrame local = parsed(R"({"type":"turn","seconds":3600,"iceServers":[]})");
    CHECK(local.ice_servers.empty());
    CHECK_FALSE(parse_service_frame(R"({"type":"turn","seconds":0,"iceServers":[]})"));
    CHECK_FALSE(parse_service_frame(R"({"type":"turn","seconds":"60","iceServers":[]})"));
    CHECK_FALSE(parse_service_frame(R"({"type":"turn","seconds":60,"iceServers":[{"urls":5}]})"));
}

TEST_CASE("a signal carries an offer, an answer or a candidate, and nothing else") {
    const ServiceFrame offer = parsed(
        R"({"type":"signal","from":"000000000000000b","data":{"gen":0,"kind":"offer","sdp":"v=0"}})");
    CHECK(offer.peer == 0xb);
    CHECK(offer.signal.kind == Signal::Kind::Offer);
    CHECK(offer.signal.sdp == "v=0");

    const ServiceFrame candidate = parsed(
        R"({"type":"signal","from":"000000000000000b","data":{"gen":0,"kind":"candidate","candidate":"a=c","mid":"0"}})");
    CHECK(candidate.signal.kind == Signal::Kind::Candidate);
    CHECK(candidate.signal.mid == "0");

    CHECK_FALSE(parse_service_frame(
        R"({"type":"signal","from":"000000000000000b","data":{"gen":0,"kind":"poke"}})"));
    CHECK_FALSE(parse_service_frame(
        R"({"type":"signal","from":"000000000000000b","data":{"gen":0,"kind":"candidate","candidate":"a=c"}})"));
    CHECK_FALSE(parse_service_frame(R"({"type":"signal","from":"000000000000000b","data":"offer"})"));
}

TEST_CASE("join, leave and error frames") {
    const ServiceFrame join = parsed(R"({"type":"join","peer":"000000000000000c"})");
    CHECK(join.kind == ServiceFrame::Kind::Join);
    CHECK(join.peer == 0xc);
    const ServiceFrame error = parsed(R"({"type":"error","code":"full","message":"4 peers"})");
    CHECK(error.code == "full");
    CHECK(error.message == "4 peers");
    CHECK_FALSE(parse_service_frame(R"({"type":"leave"})"));
}

TEST_CASE("text that is not a frame is refused") {
    CHECK_FALSE(parse_service_frame(""));
    CHECK_FALSE(parse_service_frame("[]"));
    CHECK_FALSE(parse_service_frame(R"({"type":"welcome")"));
    CHECK_FALSE(parse_service_frame(R"({"type":"hello"})"));
    CHECK_FALSE(parse_service_frame(std::string(frame_bytes + 1, ' ')));
}

TEST_CASE("a signal names its generation; one without is refused") {
    const ServiceFrame restart = parsed(
        R"({"type":"signal","from":"000000000000000b","data":{"gen":3,"kind":"restart"}})");
    CHECK(restart.signal.kind == Signal::Kind::Restart);
    CHECK(restart.signal.generation == 3);
    CHECK_FALSE(parse_service_frame(
        R"({"type":"signal","from":"000000000000000b","data":{"kind":"offer","sdp":"v=0"}})"));
    CHECK_FALSE(parse_service_frame(
        R"({"type":"signal","from":"000000000000000b","data":{"gen":-1,"kind":"restart"}})"));
}

TEST_CASE("a signal written is a signal read back") {
    Signal sent;
    sent.kind = Signal::Kind::Candidate;
    sent.candidate = "a=candidate:1 \"q\"";
    sent.mid = "0";
    sent.generation = 2;
    const std::string frame = signal_frame(0xb, sent);
    CHECK(frame.find(R"("to":"000000000000000b")") != std::string::npos);
    // the service relays data as it is, with "from" in place of "to"
    std::string relayed = frame;
    relayed.replace(relayed.find("\"to\""), 4, "\"from\"");
    const ServiceFrame back = parsed(relayed);
    CHECK(back.signal.candidate == sent.candidate);
    CHECK(back.signal.mid == sent.mid);
    CHECK(back.signal.generation == 2);
}
