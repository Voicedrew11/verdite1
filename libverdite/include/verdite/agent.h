#ifndef VERDITE_AGENT_H
#define VERDITE_AGENT_H
// The agent channel: a state beacon on stdout and a line-based command channel
// on 127.0.0.1, so a program can drive and watch the game without a person.
// Game-agnostic: the game names its commands and supplies its state snapshot.
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace verdite {

// A command's reply is one line of JSON, built by the handler (json_ok, json_error).
using AgentArgs = std::span<const std::string>;
struct AgentCommand {
    std::string name;
    std::string usage;   // for help: "press <button> [holdMs=150] - ..."
    std::function<std::string(AgentArgs)> run;
};

struct AgentConfig {
    std::string tag;             // the game's tag: beacon lines are "[<tag>-AGENT] {...}"
    int port = 0;                // 0: no command channel
    bool beacon = false;         // a snapshot line about once a second
    std::function<std::string()> snapshot;   // the bare {...} state, read on the game thread
};

// Listens (if port != 0) and registers the commands; "help" is built in.
bool agent_start(const AgentConfig &config, std::vector<AgentCommand> commands);
// On the game thread, often (each host poll): runs queued commands, emits the beacon.
void agent_poll();
// Whether agent_start was called with a port or the beacon.
bool agent_active();
// A beacon-style line outside the period, such as a phase change.
void agent_note(std::string_view text);

// "1", "on", "true", "yes": the default port; "0", "off" or empty: 0; a number: that port.
int agent_port_from(const char *spec, int default_port);
// Same words for a plain switch.
bool agent_switch_from(const char *spec);

std::string json_quote(std::string_view text);
// {"ok":false,"error":"..."}
std::string json_error(std::string_view message);
// {"ok":true,"cmd":"<name>"<fields>}; fields is empty or starts with a comma.
std::string json_ok(std::string_view command, std::string_view fields = {});

} // namespace verdite

#endif // VERDITE_AGENT_H
