#include <verdite/agent.h>

#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <future>
#include <stdexcept>
#include <string>
#include <vector>

using namespace verdite;

TEST_CASE("json_quote escapes what JSON requires") {
    CHECK(json_quote("plain") == "\"plain\"");
    CHECK(json_quote("a\"b\\c") == "\"a\\\"b\\\\c\"");
    CHECK(json_quote("line\nreturn\rtab\t") == "\"line\\nreturn\\rtab\\t\"");
    CHECK(json_quote(std::string_view("\x01\x1f", 2)) == "\"\\u0001\\u001f\"");
    CHECK(json_quote("") == "\"\"");
}

TEST_CASE("json_ok and json_error") {
    CHECK(json_ok("state") == R"({"ok":true,"cmd":"state"})");
    CHECK(json_ok("state", R"(,"hp":30)") == R"({"ok":true,"cmd":"state","hp":30})");
    CHECK(json_error("no \"x\"") == R"({"ok":false,"error":"no \"x\""})");
}

TEST_CASE("agent_port_from") {
    CHECK(agent_port_from(nullptr, 27901) == 0);
    for (const char *off : {"", "0", "off", "false", "no", " OFF "})
        CHECK(agent_port_from(off, 27901) == 0);
    for (const char *on : {"1", "on", "true", "yes", " Yes "})
        CHECK(agent_port_from(on, 27901) == 27901);
    CHECK(agent_port_from("27950", 27901) == 27950);
    CHECK(agent_port_from("65535", 27901) == 65535);
    for (const char *bad : {"65536", "-5", "12ab", "port"})
        CHECK(agent_port_from(bad, 27901) == 0);
}

TEST_CASE("agent_switch_from") {
    CHECK_FALSE(agent_switch_from(nullptr));
    CHECK_FALSE(agent_switch_from("0"));
    CHECK_FALSE(agent_switch_from("maybe"));
    CHECK(agent_switch_from("1"));
    CHECK(agent_switch_from("ON"));
}

namespace {

// Sends text to the channel and reads replies until `lines` have come back.
std::vector<std::string> converse(int port, const std::string &text, std::size_t lines) {
    const int client = socket(AF_INET, SOCK_STREAM, 0);
    if (client < 0)
        throw std::runtime_error("socket failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(client, reinterpret_cast<const sockaddr *>(&address), sizeof address) != 0 ||
        send(client, text.data(), text.size(), 0) != static_cast<ssize_t>(text.size())) {
        close(client);
        throw std::runtime_error("cannot reach the channel");
    }
    std::vector<std::string> replies;
    std::string line;
    char buffer[256];
    while (replies.size() < lines) {
        const auto got = recv(client, buffer, sizeof buffer, 0);
        if (got <= 0)
            break;
        for (ssize_t i = 0; i < got; ++i) {
            if (buffer[i] == '\n') {
                replies.push_back(line);
                line.clear();
            } else {
                line += buffer[i];
            }
        }
    }
    close(client);
    return replies;
}

// Talks to the channel from another thread while this one polls, as the game thread would.
std::vector<std::string> converse_polling(int port, const std::string &text, std::size_t lines) {
    auto replies = std::async(std::launch::async, converse, port, text, lines);
    while (replies.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready)
        agent_poll();
    return replies.get();
}

// The agent is one per process: started once, with two test commands, for every channel check.
int channel_port() {
    static const int port = [] {
        const int chosen = 40000 + static_cast<int>(getpid() % 20000);
        std::vector<AgentCommand> commands;
        commands.push_back({"echo", "echo <words> - the words back", [](AgentArgs args) {
            std::string words;
            for (const auto &word : args)
                words += (words.empty() ? "" : " ") + word;
            return json_ok("echo", ",\"words\":" + json_quote(words));
        }});
        commands.push_back({"boom", "boom - throws", [](AgentArgs) -> std::string {
            throw std::runtime_error("it broke");
        }});
        if (!agent_start({"TEST", chosen, false, {}}, std::move(commands)))
            throw std::runtime_error("cannot start the agent");
        return chosen;
    }();
    return port;
}

} // namespace

TEST_CASE("the command channel runs commands on the polling thread") {
    const int port = channel_port();
    CHECK(agent_active());

    SUBCASE("a command and its arguments; the name ignores case") {
        CHECK(converse_polling(port, "ECHO  a   b\n", 1) ==
              std::vector<std::string>{R"({"ok":true,"cmd":"echo","words":"a b"})"});
    }
    SUBCASE("help lists the commands and itself") {
        const auto replies = converse_polling(port, "help\n", 1);
        REQUIRE(replies.size() == 1);
        CHECK(replies[0] == R"({"ok":true,"cmd":"help","commands":["echo <words> - the words back",)"
                            R"("boom - throws","help - this list"]})");
    }
    SUBCASE("an unknown command") {
        CHECK(converse_polling(port, "dance\n", 1) ==
              std::vector<std::string>{R"({"ok":false,"error":"unknown command 'dance'; try help"})"});
    }
    SUBCASE("a throwing handler becomes an error reply") {
        CHECK(converse_polling(port, "boom\n", 1) ==
              std::vector<std::string>{R"({"ok":false,"error":"it broke"})"});
    }
    SUBCASE("blank lines get no reply; CR LF ends a line") {
        CHECK(converse_polling(port, "\n   \r\necho x\r\n", 1) ==
              std::vector<std::string>{R"({"ok":true,"cmd":"echo","words":"x"})"});
    }
    SUBCASE("a line over 256 characters is refused, and the next one still runs") {
        const std::string text = "echo " + std::string(300, 'x') + "\necho y\n";
        CHECK(converse_polling(port, text, 2) ==
              std::vector<std::string>{R"j({"ok":false,"error":"line too long (256 max)"})j",
                                       R"({"ok":true,"cmd":"echo","words":"y"})"});
    }
}
