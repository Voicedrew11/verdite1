#include <verdite/discord.h>
#include <verdite/agent.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace verdite {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

// The IPC: frames of a little-endian u32 opcode, a u32 payload length, then
// that much UTF-8 JSON. A handshake is answered by READY, every command by a
// reply; a ping is answered with its payload as a pong.
enum class Op : std::uint32_t { Handshake = 0, Frame = 1, Close = 2, Ping = 3, Pong = 4 };

struct Frame {
    Op op = Op::Frame;
    std::string payload;
};

constexpr std::uint32_t max_payload = 64 * 1024;
constexpr int ipc_slots = 10;                 // discord-ipc-0 .. discord-ipc-9
constexpr auto io_timeout = 2s;
constexpr auto poll_period = 50ms;
constexpr auto headline_gap = 50ms;           // a new first line
constexpr auto detail_gap = 250ms;            // the second line alone
constexpr auto reconnect_min = 2s;
constexpr auto reconnect_max = 30s;
constexpr std::size_t max_line_chars = 128;   // Discord's limit, in characters

#ifdef _WIN32
class Pipe {
public:
    static std::optional<Pipe> open(const std::string &path) {
        const HANDLE handle = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return std::nullopt;
        return Pipe(handle);
    }
    Pipe(Pipe &&other) noexcept : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}
    Pipe &operator=(Pipe &&other) noexcept {
        std::swap(handle_, other.handle_);
        return *this;
    }
    Pipe(const Pipe &) = delete;
    Pipe &operator=(const Pipe &) = delete;
    ~Pipe() {
        if (handle_ != INVALID_HANDLE_VALUE)
            CloseHandle(handle_);
    }

    bool write(std::string_view bytes) {
        while (!bytes.empty()) {
            DWORD sent = 0;
            if (!WriteFile(handle_, bytes.data(), static_cast<DWORD>(bytes.size()), &sent, nullptr) || sent == 0)
                return false;
            bytes.remove_prefix(sent);
        }
        return true;
    }

    // A blocking ReadFile could not be given up on, so this waits for bytes to arrive.
    bool read(std::span<char> into, Clock::time_point deadline) {
        while (!into.empty()) {
            DWORD available = 0;
            if (!PeekNamedPipe(handle_, nullptr, 0, nullptr, &available, nullptr))
                return false;
            if (available == 0) {
                if (Clock::now() >= deadline)
                    return false;
                Sleep(5);
                continue;
            }
            DWORD got = 0;
            const DWORD wanted = std::min<DWORD>(available, static_cast<DWORD>(into.size()));
            if (!ReadFile(handle_, into.data(), wanted, &got, nullptr) || got == 0)
                return false;
            into = into.subspan(got);
        }
        return true;
    }

private:
    explicit Pipe(HANDLE handle) : handle_(handle) {}
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

std::vector<std::string> ipc_directories() { return {}; }

std::vector<std::string> ipc_paths(int slot, const std::vector<std::string> &) {
    return {"\\\\.\\pipe\\discord-ipc-" + std::to_string(slot)};
}

long process_id() { return static_cast<long>(GetCurrentProcessId()); }
#else
#ifdef MSG_NOSIGNAL
constexpr int send_flags = MSG_NOSIGNAL;
#else
constexpr int send_flags = 0;   // SO_NOSIGPIPE on the socket instead
#endif

class Pipe {
public:
    static std::optional<Pipe> open(const std::string &path) {
        sockaddr_un address{};
        if (path.size() >= sizeof address.sun_path)
            return std::nullopt;
        Pipe pipe(socket(AF_UNIX, SOCK_STREAM, 0));
        if (pipe.fd_ < 0)
            return std::nullopt;
#ifdef SO_NOSIGPIPE
        const int yes = 1;
        if (setsockopt(pipe.fd_, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof yes) != 0)
            return std::nullopt;
#endif
        timeval timeout{};
        timeout.tv_sec = std::chrono::seconds(io_timeout).count();
        if (setsockopt(pipe.fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout) != 0)
            return std::nullopt;
        address.sun_family = AF_UNIX;
        std::copy(path.begin(), path.end(), address.sun_path);
        if (connect(pipe.fd_, reinterpret_cast<const sockaddr *>(&address), sizeof address) != 0)
            return std::nullopt;
        return pipe;
    }
    Pipe(Pipe &&other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Pipe &operator=(Pipe &&other) noexcept {
        std::swap(fd_, other.fd_);
        return *this;
    }
    Pipe(const Pipe &) = delete;
    Pipe &operator=(const Pipe &) = delete;
    ~Pipe() {
        if (fd_ >= 0)
            close(fd_);
    }

    bool write(std::string_view bytes) {
        while (!bytes.empty()) {
            const ssize_t sent = send(fd_, bytes.data(), bytes.size(), send_flags);
            if (sent < 0 && errno == EINTR)
                continue;
            if (sent <= 0)
                return false;
            bytes.remove_prefix(static_cast<std::size_t>(sent));
        }
        return true;
    }

    bool read(std::span<char> into, Clock::time_point deadline) {
        while (!into.empty()) {
            const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now()).count();
            if (left <= 0)
                return false;
            pollfd ready{fd_, POLLIN, 0};
            const int polled = poll(&ready, 1, static_cast<int>(left));
            if (polled < 0 && errno == EINTR)
                continue;
            if (polled <= 0)
                return false;
            const ssize_t got = recv(fd_, into.data(), into.size(), 0);
            if (got < 0 && errno == EINTR)
                continue;
            if (got <= 0)
                return false;
            into = into.subspan(static_cast<std::size_t>(got));
        }
        return true;
    }

private:
    explicit Pipe(int fd) : fd_(fd) {}
    int fd_ = -1;
};

// Where Discord puts its socket: the runtime directory (also under the Flatpak
// and Snap sandboxes), then the temporary directories. Read on the caller's
// thread, since getenv is not safe beside a setenv.
std::vector<std::string> ipc_directories() {
    std::vector<std::string> directories;
    auto add = [&](std::string directory) {
        if (!directory.empty() && std::find(directories.begin(), directories.end(), directory) == directories.end())
            directories.push_back(std::move(directory));
    };
    if (const char *runtime = std::getenv("XDG_RUNTIME_DIR"); runtime && *runtime) {
        add(runtime);
        add(std::string(runtime) + "/app/com.discordapp.Discord");
        add(std::string(runtime) + "/snap.discord");
    }
    for (const char *name : {"TMPDIR", "TMP", "TEMP"})
        if (const char *directory = std::getenv(name))
            add(directory);
    add("/tmp");
    return directories;
}

std::vector<std::string> ipc_paths(int slot, const std::vector<std::string> &directories) {
    std::vector<std::string> paths;
    paths.reserve(directories.size());
    for (const auto &directory : directories)
        paths.push_back(directory + "/discord-ipc-" + std::to_string(slot));
    return paths;
}

long process_id() { return static_cast<long>(getpid()); }
#endif

void put_u32(std::string &out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
        out += static_cast<char>((value >> shift) & 0xff);
}

std::uint32_t get_u32(std::span<const char, 4> bytes) {
    std::uint32_t value = 0;
    for (int i = 3; i >= 0; --i)
        value = (value << 8) | static_cast<unsigned char>(bytes[i]);
    return value;
}

bool write_frame(Pipe &pipe, Op op, std::string_view json) {
    std::string frame;
    frame.reserve(8 + json.size());
    put_u32(frame, static_cast<std::uint32_t>(op));
    put_u32(frame, static_cast<std::uint32_t>(json.size()));
    frame += json;
    return pipe.write(frame);
}

std::optional<Frame> read_frame(Pipe &pipe) {
    const auto deadline = Clock::now() + io_timeout;
    std::array<char, 8> header{};
    if (!pipe.read(header, deadline))
        return std::nullopt;
    const std::uint32_t length = get_u32(std::span(header).subspan<4, 4>());
    if (length > max_payload)
        return std::nullopt;
    Frame frame{static_cast<Op>(get_u32(std::span(header).first<4>())), std::string(length, '\0')};
    if (!pipe.read(frame.payload, deadline))
        return std::nullopt;
    return frame;
}

std::optional<Pipe> connect_to_discord(const std::string &application_id, const std::vector<std::string> &directories) {
    const std::string handshake = "{\"v\":1,\"client_id\":" + json_quote(application_id) + "}";
    for (int slot = 0; slot < ipc_slots; ++slot)
        for (const auto &path : ipc_paths(slot, directories)) {
            auto pipe = Pipe::open(path);
            if (!pipe || !write_frame(*pipe, Op::Handshake, handshake))
                continue;
            const auto ready = read_frame(*pipe);
            if (ready && ready->op == Op::Frame && ready->payload.find("\"READY\"") != std::string::npos)
                return pipe;
        }
    return std::nullopt;
}

// Clipped by characters, not bytes, so a UTF-8 sequence is never cut.
std::string clip(std::string_view line) {
    std::size_t characters = 0, cut = line.size();
    for (std::size_t at = 0; at < line.size(); ++at) {
        if ((static_cast<unsigned char>(line[at]) & 0xc0) == 0x80)
            continue;
        if (characters == max_line_chars - 1)
            cut = at;
        ++characters;
    }
    if (characters <= max_line_chars)
        return std::string(line);
    return std::string(line.substr(0, cut)) + "\xe2\x80\xa6";   // an ellipsis
}

std::string activity_json(const DiscordActivity &activity) {
    std::string json = "{\"instance\":false";
    if (activity.start_unix_s > 0)
        json += ",\"timestamps\":{\"start\":" + std::to_string(activity.start_unix_s) + "}";
    if (!activity.details.empty())
        json += ",\"details\":" + json_quote(clip(activity.details));
    if (!activity.state.empty())
        json += ",\"state\":" + json_quote(clip(activity.state));
    if (!activity.large_image.empty()) {
        json += ",\"assets\":{\"large_image\":" + json_quote(activity.large_image);
        if (!activity.large_text.empty())
            json += ",\"large_text\":" + json_quote(clip(activity.large_text));
        json += "}";
    }
    return json + "}";
}

// No activity takes the presence down.
std::string set_activity(const std::optional<DiscordActivity> &activity, std::uint64_t nonce) {
    std::string arguments = "{\"pid\":" + std::to_string(process_id());
    if (activity)
        arguments += ",\"activity\":" + activity_json(*activity);
    return "{\"cmd\":\"SET_ACTIVITY\",\"args\":" + arguments + "},\"nonce\":\"" + std::to_string(nonce) + "\"}";
}

enum class Outcome : std::uint8_t { Accepted, Refused, Lost };
struct Reply {
    Outcome outcome = Outcome::Lost;
    std::string text;
};

// Discord answers every command, and pings may come first.
Reply await_reply(Pipe &pipe) {
    for (;;) {
        auto frame = read_frame(pipe);
        if (!frame || frame->op == Op::Close)
            return {Outcome::Lost, frame ? frame->payload : std::string()};
        if (frame->op == Op::Ping) {
            if (!write_frame(pipe, Op::Pong, frame->payload))
                return {};
            continue;
        }
        if (frame->op != Op::Frame)
            continue;
        const bool refused = frame->payload.find("\"evt\":\"ERROR\"") != std::string::npos;
        return {refused ? Outcome::Refused : Outcome::Accepted, std::move(frame->payload)};
    }
}

struct Publisher {
    DiscordConfig config;
    std::vector<std::string> directories;
    std::mutex mutex;
    std::condition_variable wake;
    std::optional<DiscordActivity> wanted;   // none: no presence
    std::uint64_t revision = 0;
    bool stopping = false;
    std::thread thread;
};
Publisher publisher;

void log(const std::string &text) {
    std::printf("[%s] discord: %s\n", publisher.config.tag.c_str(), text.c_str());
    std::fflush(stdout);
}

// The publisher thread's view of Discord.
struct Link {
    std::optional<Pipe> pipe;
    std::optional<DiscordActivity> shown;   // what Discord was last told
    Clock::time_point last_send{};
    Clock::time_point next_connect{};
    int failures = 0;
    bool told_missing = false;
    std::uint64_t nonce = 0;
};

// On the publisher thread.
void drop(Link &link, const std::string &why) {
    log("connection lost: " + why);
    link.pipe.reset();
    link.shown.reset();
    link.failures = 1;
    link.next_connect = Clock::now() + reconnect_min;
}

// On the publisher thread. Backs off while Discord is not running.
bool reconnect(Link &link, Clock::time_point now) {
    if (now < link.next_connect)
        return false;
    link.pipe = connect_to_discord(publisher.config.application_id, publisher.directories);
    if (!link.pipe) {
        ++link.failures;
        link.next_connect = now + std::min<Clock::duration>(reconnect_max, reconnect_min * (1 << std::min(link.failures - 1, 4)));
        if (!link.told_missing)
            log("Discord not found; will keep trying");
        link.told_missing = true;
        return false;
    }
    log("connected");
    link.told_missing = false;
    link.failures = 0;
    link.shown.reset();
    return true;
}

Clock::duration send_gap(const std::optional<DiscordActivity> &shown, const std::optional<DiscordActivity> &wanted) {
    return shown && wanted && shown->details == wanted->details ? detail_gap : headline_gap;
}

// On the publisher thread.
void send(Link &link, const std::optional<DiscordActivity> &wanted, Clock::time_point now) {
    if (!link.pipe)
        return;
    link.last_send = now;
    if (!write_frame(*link.pipe, Op::Frame, set_activity(wanted, ++link.nonce))) {
        drop(link, "cannot write");
        return;
    }
    const Reply reply = await_reply(*link.pipe);
    switch (reply.outcome) {
    case Outcome::Accepted:
        link.shown = wanted;
        return;
    case Outcome::Refused:
        // Not sent again until it changes.
        log("Discord refused the activity: " + reply.text);
        link.shown = wanted;
        return;
    case Outcome::Lost:
        drop(link, reply.text.empty() ? "no reply" : reply.text);
        return;
    }
}

// On the publisher thread.
void step(Link &link, const std::optional<DiscordActivity> &wanted) {
    const auto now = Clock::now();
    if (!link.pipe && (!wanted || !reconnect(link, now)))
        return;
    if (wanted == link.shown || now - link.last_send < send_gap(link.shown, wanted))
        return;
    send(link, wanted, now);
}

// The publisher thread.
void run() {
    Link link;
    std::uint64_t seen = 0;
    for (;;) {
        std::optional<DiscordActivity> wanted;
        {
            std::unique_lock lock(publisher.mutex);
            publisher.wake.wait_for(lock, poll_period, [&] { return publisher.stopping || publisher.revision != seen; });
            if (publisher.stopping)
                break;
            seen = publisher.revision;
            wanted = publisher.wanted;
        }
        step(link, wanted);
    }
    // The process is going away: no waiting for the reply.
    if (link.pipe && link.shown && !write_frame(*link.pipe, Op::Frame, set_activity(std::nullopt, ++link.nonce)))
        log("cannot clear the presence on exit");
}

void want(std::optional<DiscordActivity> activity) {
    {
        std::lock_guard lock(publisher.mutex);
        if (publisher.wanted == activity)
            return;
        publisher.wanted = std::move(activity);
        ++publisher.revision;
    }
    publisher.wake.notify_one();
}

} // namespace

bool discord_start(const DiscordConfig &config) {
    if (publisher.thread.joinable())
        return true;
    if (config.application_id.empty())
        return false;
    publisher.config = config;
    publisher.directories = ipc_directories();
    publisher.thread = std::thread(run);
    return true;
}

void discord_show(const DiscordActivity &activity) { want(activity); }

void discord_clear() { want(std::nullopt); }

void discord_stop() {
    if (!publisher.thread.joinable())
        return;
    {
        std::lock_guard lock(publisher.mutex);
        publisher.stopping = true;
    }
    publisher.wake.notify_one();
    publisher.thread.join();
    std::lock_guard lock(publisher.mutex);
    publisher.stopping = false;
    publisher.wanted.reset();
    ++publisher.revision;
}

} // namespace verdite
