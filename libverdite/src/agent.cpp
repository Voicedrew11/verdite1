#include <verdite/agent.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
static constexpr socket_t no_socket = INVALID_SOCKET;
static void close_socket(socket_t s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static constexpr socket_t no_socket = -1;
static void close_socket(socket_t s) { close(s); }
#endif

namespace verdite {
namespace {

constexpr std::size_t max_line_length = 256;
constexpr std::size_t queue_capacity = 16;
constexpr auto reply_timeout = std::chrono::seconds(5);
constexpr auto beacon_period = std::chrono::seconds(1);

struct Pending {
    std::vector<std::string> words;
    std::promise<std::string> reply;
};

struct Agent {
    AgentConfig config;
    std::vector<AgentCommand> commands;
    std::mutex mutex;
    std::deque<std::shared_ptr<Pending>> queue;
    socket_t listener = no_socket;
    std::chrono::steady_clock::time_point last_beacon;
    bool started = false;
};
Agent agent;

void emit(std::string_view text) {
    std::printf("[%s-AGENT] %.*s\n", agent.config.tag.c_str(), static_cast<int>(text.size()), text.data());
    std::fflush(stdout);
}

std::string help() {
    std::string list;
    for (const auto &command : agent.commands)
        list += (list.empty() ? "" : ",") + json_quote(command.usage);
    list += (list.empty() ? "" : ",") + json_quote("help - this list");
    return json_ok("help", ",\"commands\":[" + list + "]");
}

std::string execute(const std::vector<std::string> &words) {
    if (words[0] == "help")
        return help();
    for (const auto &command : agent.commands)
        if (command.name == words[0]) {
            try {
                return command.run(AgentArgs(words).subspan(1));
            } catch (const std::exception &error) {
                return json_error(error.what());
            }
        }
    return json_error("unknown command '" + words[0] + "'; try help");
}

std::vector<std::string> split(std::string_view line) {
    std::vector<std::string> words;
    std::size_t at = 0;
    while (at < line.size()) {
        while (at < line.size() && std::isspace(static_cast<unsigned char>(line[at])))
            ++at;
        const std::size_t start = at;
        while (at < line.size() && !std::isspace(static_cast<unsigned char>(line[at])))
            ++at;
        if (at > start)
            words.emplace_back(line.substr(start, at - start));
    }
    return words;
}

// On a client's thread: hand the line to the game thread and wait for its reply.
std::string route(std::string_view line) {
    auto words = split(line);
    if (words.empty())
        return {};
    std::transform(words[0].begin(), words[0].end(), words[0].begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto pending = std::make_shared<Pending>();
    pending->words = std::move(words);
    auto reply = pending->reply.get_future();
    {
        std::lock_guard lock(agent.mutex);
        if (agent.queue.size() >= queue_capacity)
            return json_error("too many queued commands (16 max)");
        agent.queue.push_back(pending);
    }
    if (reply.wait_for(reply_timeout) != std::future_status::ready)
        return json_error("timed out after 5s: the game thread is not polling");
    return reply.get();
}

void send_line(socket_t client, std::string line) {
    line += '\n';
    std::size_t sent = 0;
    while (sent < line.size()) {
        const auto n = send(client, line.data() + sent, static_cast<int>(line.size() - sent), 0);
        if (n <= 0)
            return;
        sent += static_cast<std::size_t>(n);
    }
}

void serve(socket_t client) {
    std::string line;
    bool over = false;
    char buffer[256];
    for (;;) {
        const auto got = recv(client, buffer, sizeof buffer, 0);
        if (got <= 0)
            break;
        for (int i = 0; i < got; ++i) {
            const char c = buffer[i];
            if (c == '\n' || c == '\r') {
                if (over)
                    send_line(client, json_error("line too long (256 max)"));
                else if (auto reply = route(line); !reply.empty())
                    send_line(client, reply);
                line.clear();
                over = false;
            } else if (!over) {
                if (line.size() == max_line_length)
                    over = true;
                else
                    line += c;
            }
        }
    }
    close_socket(client);
}

void accept_loop(socket_t listener) {
    for (;;) {
        const socket_t client = accept(listener, nullptr, nullptr);
        if (client == no_socket)
            continue;
        std::thread(serve, client).detach();
    }
}

bool listen_on(int port) {
#ifdef _WIN32
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
        return false;
#endif
    const socket_t listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == no_socket)
        return false;
    const int yes = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&yes), sizeof yes);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, reinterpret_cast<const sockaddr *>(&address), sizeof address) != 0 || listen(listener, 4) != 0) {
        close_socket(listener);
        return false;
    }
    agent.listener = listener;
    std::thread(accept_loop, listener).detach();
    return true;
}

bool word_is(const char *spec, std::initializer_list<const char *> words) {
    std::string text;
    for (const char *c = spec; c && *c; ++c)
        if (!std::isspace(static_cast<unsigned char>(*c)))
            text += static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));
    return std::any_of(words.begin(), words.end(), [&](const char *word) { return text == word; });
}

} // namespace

bool agent_start(const AgentConfig &config, std::vector<AgentCommand> commands) {
    agent.config = config;
    agent.commands = std::move(commands);
    agent.started = config.port != 0 || config.beacon;
    if (config.beacon)
        emit("beacon on");
    if (config.port == 0)
        return true;
    if (!listen_on(config.port)) {
        std::fprintf(stderr, "[%s] agent server: cannot listen on port %d\n", config.tag.c_str(), config.port);
        return false;
    }
    std::printf("[%s] agent server: listening on 127.0.0.1:%d\n", config.tag.c_str(), config.port);
    std::fflush(stdout);
    return true;
}

void agent_poll() {
    if (!agent.started)
        return;
    for (;;) {
        std::shared_ptr<Pending> pending;
        {
            std::lock_guard lock(agent.mutex);
            if (agent.queue.empty())
                break;
            pending = agent.queue.front();
            agent.queue.pop_front();
        }
        pending->reply.set_value(execute(pending->words));
    }
    if (agent.config.beacon && agent.config.snapshot) {
        const auto now = std::chrono::steady_clock::now();
        if (now - agent.last_beacon >= beacon_period) {
            agent.last_beacon = now;
            emit(agent.config.snapshot());
        }
    }
}

bool agent_active() { return agent.started; }

void agent_note(std::string_view text) {
    if (agent.config.beacon)
        emit(text);
}

int agent_port_from(const char *spec, int default_port) {
    if (!spec || word_is(spec, {"", "0", "off", "false", "no"}))
        return 0;
    if (word_is(spec, {"1", "on", "true", "yes"}))
        return default_port;
    char *end = nullptr;
    const long port = std::strtol(spec, &end, 10);
    if (end == spec || *end || port < 1 || port > 65535)
        return 0;
    return static_cast<int>(port);
}

bool agent_switch_from(const char *spec) { return spec && word_is(spec, {"1", "on", "true", "yes"}); }

std::string json_quote(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char escape[8];
                std::snprintf(escape, sizeof escape, "\\u%04x", c);
                out += escape;
            } else {
                out += c;
            }
        }
    }
    return out + '"';
}

std::string json_error(std::string_view message) { return "{\"ok\":false,\"error\":" + json_quote(message) + "}"; }

std::string json_ok(std::string_view command, std::string_view fields) {
    return "{\"ok\":true,\"cmd\":" + json_quote(command) + std::string(fields) + "}";
}

} // namespace verdite
