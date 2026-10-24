#include <verdite/discord.h>

#include <doctest/doctest.h>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace verdite;
using namespace std::chrono_literals;

namespace {

constexpr std::uint32_t op_handshake = 0, op_frame = 1, op_close = 2, op_ping = 3, op_pong = 4;

struct Frame {
    std::uint32_t op = 0;
    std::string payload;
};

bool read_exactly(int fd, char *into, std::size_t size) {
    while (size > 0) {
        pollfd ready{fd, POLLIN, 0};
        if (poll(&ready, 1, 3000) <= 0)
            return false;
        const ssize_t got = recv(fd, into, size, 0);
        if (got <= 0)
            return false;
        into += got;
        size -= static_cast<std::size_t>(got);
    }
    return true;
}

std::optional<Frame> read_frame(int fd) {
    std::array<unsigned char, 8> header{};
    if (!read_exactly(fd, reinterpret_cast<char *>(header.data()), header.size()))
        return std::nullopt;
    auto u32 = [&](int at) {
        return static_cast<std::uint32_t>(header[at] | header[at + 1] << 8 | header[at + 2] << 16 | header[at + 3] << 24);
    };
    Frame frame{u32(0), std::string(u32(4), '\0')};
    if (!read_exactly(fd, frame.payload.data(), frame.payload.size()))
        return std::nullopt;
    return frame;
}

// False when the client has gone, as it does after the clear on exit.
bool write_frame(int fd, std::uint32_t op, const std::string &json) {
    std::string bytes;
    for (const std::uint32_t value : {op, static_cast<std::uint32_t>(json.size())})
        for (int shift = 0; shift < 32; shift += 8)
            bytes += static_cast<char>((value >> shift) & 0xff);
    bytes += json;
    return send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(bytes.size());
}

// A Discord on discord-ipc-0 in a directory of its own, which XDG_RUNTIME_DIR
// points at while it lives. It records every frame the client sends.
class FakeDiscord {
public:
    bool ping_first = false;            // ping before each reply
    std::atomic<bool> close_next{false};   // hang up instead of the next reply

    FakeDiscord() {
        std::string pattern = (std::filesystem::temp_directory_path() / "verdite-discord-XXXXXX").string();
        if (!mkdtemp(pattern.data()))
            throw std::runtime_error("cannot make a directory");
        directory_ = pattern;
        setenv("XDG_RUNTIME_DIR", directory_.c_str(), 1);
        listener_ = socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        const std::string path = (directory_ / "discord-ipc-0").string();
        std::copy(path.begin(), path.end(), address.sun_path);
        if (listener_ < 0 || bind(listener_, reinterpret_cast<const sockaddr *>(&address), sizeof address) != 0 ||
            listen(listener_, 4) != 0)
            throw std::runtime_error("cannot listen");
        thread_ = std::thread([this] { serve(); });
    }
    FakeDiscord(const FakeDiscord &) = delete;
    FakeDiscord &operator=(const FakeDiscord &) = delete;
    ~FakeDiscord() {
        stopping_ = true;
        thread_.join();
        close(listener_);
        std::filesystem::remove_all(directory_);
    }

    std::vector<Frame> frames() {
        std::lock_guard lock(mutex_);
        return frames_;
    }

    // Waits up to the timeout for the recorded frames to satisfy the test.
    bool wait_until(const std::function<bool(const std::vector<Frame> &)> &test, std::chrono::milliseconds timeout = 3s) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (test(frames()))
                return true;
            std::this_thread::sleep_for(5ms);
        }
        return test(frames());
    }

    bool wait_for_count(std::size_t count, std::chrono::milliseconds timeout = 3s) {
        return wait_until([&](const std::vector<Frame> &all) { return all.size() >= count; }, timeout);
    }

private:
    void record(const Frame &frame) {
        std::lock_guard lock(mutex_);
        frames_.push_back(frame);
    }

    void serve() {
        while (!stopping_) {
            pollfd ready{listener_, POLLIN, 0};
            if (poll(&ready, 1, 20) <= 0)
                continue;
            const int client = accept(listener_, nullptr, nullptr);
            if (client < 0)
                continue;
            converse(client);
            close(client);
        }
    }

    void converse(int client) {
        while (!stopping_) {
            pollfd ready{client, POLLIN, 0};
            if (poll(&ready, 1, 20) <= 0)
                continue;
            const auto frame = read_frame(client);
            if (!frame)
                return;
            record(*frame);
            if (frame->op == op_handshake) {
                if (!write_frame(client, op_frame, R"({"cmd":"DISPATCH","evt":"READY","data":{"v":1}})"))
                    return;
                continue;
            }
            if (close_next.exchange(false)) {
                (void)write_frame(client, op_close, R"({"code":1000,"message":"bye"})");
                return;
            }
            if (ping_first) {
                if (!write_frame(client, op_ping, R"({"ping":7})"))
                    return;
                const auto pong = read_frame(client);
                if (!pong)
                    return;
                record(*pong);
            }
            if (!write_frame(client, op_frame, R"({"cmd":"SET_ACTIVITY","evt":null,"data":{}})"))
                return;
        }
    }

    std::filesystem::path directory_;
    int listener_ = -1;
    std::atomic<bool> stopping_{false};
    std::mutex mutex_;
    std::vector<Frame> frames_;
    std::thread thread_;
};

bool contains(const std::string &text, const std::string &part) { return text.find(part) != std::string::npos; }

DiscordActivity activity(std::string details, std::string state) {
    return {std::move(details), std::move(state), "logo", "The game", 1700000000};
}

} // namespace

TEST_CASE("no application id starts nothing") {
    CHECK_FALSE(discord_start({"", "TEST"}));
}

TEST_CASE("an activity is shown after the handshake, and only resent when it changes") {
    FakeDiscord discord;
    REQUIRE(discord_start({"1234", "TEST"}));
    CHECK(discord.frames().empty());   // nothing to show: not even a connection

    discord_show(activity("Level 3", "HP 10/20"));
    REQUIRE(discord.wait_for_count(2));
    auto frames = discord.frames();
    CHECK(frames[0].op == op_handshake);
    CHECK(frames[0].payload == R"({"v":1,"client_id":"1234"})");
    CHECK(frames[1].op == op_frame);
    CHECK(contains(frames[1].payload, R"("cmd":"SET_ACTIVITY")"));
    CHECK(contains(frames[1].payload, R"("pid":)" + std::to_string(getpid())));
    CHECK(contains(frames[1].payload, R"("activity":{"instance":false,"timestamps":{"start":1700000000},)"
                                      R"("details":"Level 3","state":"HP 10/20",)"
                                      R"("assets":{"large_image":"logo","large_text":"The game"}})"));

    discord_show(activity("Level 3", "HP 10/20"));
    std::this_thread::sleep_for(400ms);
    CHECK(discord.frames().size() == 2);

    discord_show(activity("Level 3", "HP 9/20"));
    REQUIRE(discord.wait_for_count(3));
    CHECK(contains(discord.frames()[2].payload, R"("state":"HP 9/20")"));

    discord_clear();
    REQUIRE(discord.wait_for_count(4));
    CHECK_FALSE(contains(discord.frames()[3].payload, "activity"));
    discord_stop();
}

TEST_CASE("a new first line goes sooner than a change to the second alone") {
    FakeDiscord discord;
    REQUIRE(discord_start({"1234", "TEST"}));
    discord_show(activity("Floor 1", "HP 1"));
    REQUIRE(discord.wait_for_count(2));

    const auto start = std::chrono::steady_clock::now();
    discord_show(activity("Floor 1", "HP 2"));
    REQUIRE(discord.wait_for_count(3));
    CHECK(std::chrono::steady_clock::now() - start >= 200ms);

    discord_show(activity("Floor 2", "HP 2"));
    const auto headline = std::chrono::steady_clock::now();
    REQUIRE(discord.wait_for_count(4));
    CHECK(std::chrono::steady_clock::now() - headline < 200ms);
    discord_stop();
}

TEST_CASE("lines are escaped and clipped to 128 characters, not bytes") {
    FakeDiscord discord;
    REQUIRE(discord_start({"1234", "TEST"}));
    std::string long_line;
    for (int i = 0; i < 200; ++i)
        long_line += "\xc3\xa9";   // é, two bytes
    discord_show(activity(long_line, "a \"quoted\" line"));
    REQUIRE(discord.wait_for_count(2));
    const auto payload = discord.frames()[1].payload;
    std::string clipped;
    for (int i = 0; i < 127; ++i)
        clipped += "\xc3\xa9";
    CHECK(contains(payload, "\"details\":\"" + clipped + "\xe2\x80\xa6\""));
    CHECK(contains(payload, R"("state":"a \"quoted\" line")"));
    discord_stop();
}

TEST_CASE("a ping before the reply is answered with its payload") {
    FakeDiscord discord;
    discord.ping_first = true;
    REQUIRE(discord_start({"1234", "TEST"}));
    discord_show(activity("Level 1", "HP 1"));
    REQUIRE(discord.wait_for_count(3));
    const auto frames = discord.frames();
    CHECK(frames[2].op == op_pong);
    CHECK(frames[2].payload == R"({"ping":7})");

    discord_show(activity("Level 2", "HP 1"));
    REQUIRE(discord.wait_for_count(5));   // still connected: no second handshake
    CHECK(discord.frames()[3].op == op_frame);
    discord_stop();
}

TEST_CASE("Discord hanging up is followed by a reconnect and the latest activity") {
    FakeDiscord discord;
    discord.close_next = true;
    REQUIRE(discord_start({"1234", "TEST"}));
    discord_show(activity("Level 1", "HP 1"));
    REQUIRE(discord.wait_for_count(2));

    discord_show(activity("Level 2", "HP 1"));
    REQUIRE(discord.wait_for_count(4, 6s));
    const auto frames = discord.frames();
    CHECK(frames[2].op == op_handshake);
    CHECK(contains(frames[3].payload, R"("details":"Level 2")"));
    discord_stop();
}

TEST_CASE("stopping takes the presence down") {
    FakeDiscord discord;
    REQUIRE(discord_start({"1234", "TEST"}));
    discord_show(activity("Level 1", "HP 1"));
    REQUIRE(discord.wait_for_count(2));
    discord_stop();
    REQUIRE(discord.wait_for_count(3));
    CHECK_FALSE(contains(discord.frames()[2].payload, "activity"));
}
