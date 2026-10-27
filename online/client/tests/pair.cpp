// vo_pair: two sessions in one process join one room through a running service and talk until
// --seconds have passed since they connected. Every CONTROL packet must arrive, in order; STATE
// packets are counted. Exits 0 when both stayed connected the whole time.
//
//   vo_pair --service http://127.0.0.1:8787 [--room R] [--seconds 10]

#include "verdite_online.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Side {
    const char *name = "";
    vo_session *session = nullptr;
    vo_peer self = 0;
    vo_peer other = 0;
    bool failed = false;
    bool first = false;   // in the room before the other
    uint32_t control_sent = 0;
    uint32_t control_received = 0;
    uint32_t state_sent = 0;
    uint32_t state_received = 0;
    bool big_received = false;
};

struct Options {
    std::string service;
    std::string room;
    int seconds = 10;
};

Options parse_options(int argc, char **argv) {
    Options options;
    std::random_device random;
    options.room = "pair-" + std::to_string(random());
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string name = argv[i];
        if (name == "--service")
            options.service = argv[i + 1];
        else if (name == "--room")
            options.room = argv[i + 1];
        else if (name == "--seconds")
            options.seconds = std::atoi(argv[i + 1]);
    }
    return options;
}

uint32_t read_u32(const uint8_t *data) {
    uint32_t value = 0;
    std::memcpy(&value, data, sizeof value);
    return value;
}

void on_packet(Side &side, const vo_event &event) {
    if (event.size < 4) {
        std::printf("%s: short packet\n", side.name);
        side.failed = true;
        return;
    }
    const uint32_t number = read_u32(event.data);
    if (event.channel == VO_CHANNEL_STATE) {
        ++side.state_received;
        return;
    }
    if (event.size == VO_CONTROL_BYTES) {
        side.big_received = true;
        return;
    }
    if (number != side.control_received) {
        std::printf("%s: CONTROL %u arrived, %u expected\n", side.name, number, side.control_received);
        side.failed = true;
    }
    side.control_received = number + 1;
}

void poll(Side &side) {
    vo_event event{};
    while (vo_poll(side.session, &event)) {
        switch (event.kind) {
        case VO_EVENT_JOINED:
            side.self = event.peer;
            side.first = event.older == 0;
            std::printf("%s: joined as %016llx (%s)\n", side.name, static_cast<unsigned long long>(event.peer),
                        side.first ? "first" : "after another");
            break;
        case VO_EVENT_PEER_OPEN:
            side.other = event.peer;
            std::printf("%s: %016llx open (%s)\n", side.name, static_cast<unsigned long long>(event.peer),
                        event.older ? "older" : "newer");
            break;
        case VO_EVENT_PEER_CLOSED:
            std::printf("%s: %016llx closed\n", side.name, static_cast<unsigned long long>(event.peer));
            side.failed = true;
            break;
        case VO_EVENT_PACKET:
            on_packet(side, event);
            break;
        case VO_EVENT_ERROR:
            std::printf("%s: error %d: %s\n", side.name, static_cast<int>(event.error),
                        event.message ? event.message : "");
            side.failed = true;
            break;
        }
    }
}

void send_numbered(Side &side, vo_channel channel, uint32_t &counter) {
    std::array<uint8_t, 64> packet{};
    std::memcpy(packet.data(), &counter, sizeof counter);
    if (vo_send(side.session, side.other, channel, packet.data(), packet.size()))
        ++counter;
}

void send_big(Side &side) {
    std::vector<uint8_t> packet(VO_CONTROL_BYTES, 0xab);
    if (!vo_send(side.session, side.other, VO_CHANNEL_CONTROL, packet.data(), packet.size())) {
        std::printf("%s: a %d-byte CONTROL packet was refused\n", side.name, VO_CONTROL_BYTES);
        side.failed = true;
    }
}

bool connect(std::array<Side, 2> &sides, const Options &options) {
    const vo_config config{.service_url = options.service.c_str(), .game = "kf1",
                           .build = "vo-pair", .room = options.room.c_str()};
    sides[0].session = vo_open(&config);
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    while (sides[0].self == 0 && !sides[0].failed && Clock::now() < deadline) {
        poll(sides[0]);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    sides[1].session = vo_open(&config);
    while ((sides[0].other == 0 || sides[1].other == 0) && Clock::now() < deadline) {
        for (auto &side : sides)
            poll(side);
        if (sides[0].failed || sides[1].failed)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return sides[0].other != 0 && sides[1].other != 0;
}

} // namespace

int main(int argc, char **argv) {
    const Options options = parse_options(argc, argv);
    if (options.service.empty() || options.seconds <= 0) {
        std::fprintf(stderr, "usage: vo_pair --service URL [--room R] [--seconds N]\n");
        return 2;
    }
    std::array<Side, 2> sides{Side{.name = "a"}, Side{.name = "b"}};
    const auto started = Clock::now();
    if (!connect(sides, options)) {
        std::printf("not connected within 30 s\n");
        for (auto &side : sides)
            vo_close(side.session);
        return 1;
    }
    std::printf("connected in %lld ms\n", static_cast<long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count()));
    for (auto &side : sides)
        send_big(side);

    const auto until = Clock::now() + std::chrono::seconds(options.seconds);
    auto next_report = Clock::now() + std::chrono::seconds(30);
    while (Clock::now() < until && !sides[0].failed && !sides[1].failed) {
        for (auto &side : sides) {
            poll(side);
            send_numbered(side, VO_CHANNEL_STATE, side.state_sent);
            send_numbered(side, VO_CHANNEL_CONTROL, side.control_sent);
        }
        if (Clock::now() >= next_report) {
            next_report += std::chrono::seconds(30);
            const vo_link link = vo_link_of(sides[0].session, sides[0].other);
            std::printf("%lld s: rtt %u ms, CONTROL %u/%u, STATE %u/%u\n",
                        static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(
                                                   Clock::now() - started).count()),
                        link.round_trip_ms, sides[1].control_received, sides[0].control_sent,
                        sides[1].state_received, sides[0].state_sent);
            std::fflush(stdout);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50)); // 20 updates a second
    }
    // let the last packets land
    for (int i = 0; i < 20; ++i) {
        for (auto &side : sides)
            poll(side);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    bool ok = true;
    for (std::size_t i = 0; i < sides.size(); ++i) {
        const Side &side = sides[i];
        const Side &other = sides[1 - i];
        std::printf("%s: CONTROL %u of %u, STATE %u of %u, 64 KiB packet %s\n", side.name,
                    side.control_received, other.control_sent, side.state_received, other.state_sent,
                    side.big_received ? "arrived" : "missing");
        ok = ok && !side.failed && side.big_received && side.control_received == other.control_sent;
    }
    // a joined an empty room, b joined after it
    if (!sides[0].first || sides[1].first) {
        std::printf("JOINED said a was %s and b %s\n", sides[0].first ? "first" : "not first",
                    sides[1].first ? "first" : "not first");
        ok = false;
    }
    for (auto &side : sides)
        vo_close(side.session);
    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
