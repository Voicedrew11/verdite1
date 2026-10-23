#include <kf/platform/agent.h>
#include <kf/platform/host.h>
#include <kf/platform/input.h>
#include <kf/game/player.h>
#include <verdite/agent.h>
#include <SDL3/SDL.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace kf {
namespace {
constexpr int default_port = 27901;   // Verdite2 27900, Verdite3 27903
constexpr std::uint64_t ns_per_ms = 1000000;
constexpr long default_hold_ms = 150, max_hold_ms = 600000;

struct Press { u32 buttons = 0; std::uint64_t until_ns = 0; };
Press press;
bool quit_requested;

struct ButtonName { const char *name; Button button; };
constexpr std::array<ButtonName, 14> button_names = {{
    {"up", Button::Up}, {"down", Button::Down}, {"left", Button::Left}, {"right", Button::Right},
    {"strafe_left", Button::StrafeLeft}, {"strafe_right", Button::StrafeRight},
    {"look_up", Button::LookUp}, {"look_down", Button::LookDown},
    {"attack", Button::Attack}, {"magic", Button::Magic}, {"confirm", Button::Confirm},
    {"back", Button::Back}, {"start", Button::Start}, {"select", Button::Select}}};

const char *context_name(InputContext context) {
    switch (context) {
    case InputContext::Opening: return "opening";
    case InputContext::Gameplay: return "gameplay";
    case InputContext::Menu: return "menu";
    case InputContext::Scripted: return "scripted";
    }
    return "unknown";
}

std::string snapshot() {
    const InputContext context = host_input_context();
    const auto &vitals = player_state.vitals;
    char text[512];
    // The player block is filled by a new game or a load; the opening leaves it stale.
    const bool in_game = context != InputContext::Opening && vitals.maximum_hp != 0;
    int n = std::snprintf(text, sizeof text, "{\"context\":\"%s\",\"inGame\":%s,\"clockMs\":%llu,\"frames\":%llu",
        context_name(context), in_game ? "true" : "false",
        static_cast<unsigned long long>(host_clock_ns() / ns_per_ms),
        static_cast<unsigned long long>(host_frames_presented()));
    if (in_game) {
        const auto &position = player_state.camera_position;
        const auto &rotation = player_state.camera_rotation;
        n += std::snprintf(text + n, sizeof text - n,
            ",\"hp\":%u,\"maxHp\":%u,\"mp\":%u,\"maxMp\":%u,\"area\":%d,\"level\":%u,\"gold\":%u,"
            "\"pos\":[%d,%d,%d],\"pitch\":%d,\"yaw\":%d",
            vitals.current_hp, vitals.maximum_hp, vitals.current_mp, vitals.maximum_mp,
            static_cast<int>(player_state.progress_state.current_floor), player_state.progress_state.level,
            player_state.gold, position.vx, position.vy, position.vz, rotation.vx, rotation.vy);
    }
    return std::string(text, n) + "}";
}

std::string do_state(verdite::AgentArgs) { return verdite::json_ok("state", ",\"state\":" + snapshot()); }

std::string do_press(verdite::AgentArgs args) {
    if (args.empty())
        return verdite::json_error("press <button>[+<button>...] [holdMs=150]");
    u32 buttons = 0;
    std::string names = args[0];
    for (std::size_t at = 0; at <= names.size();) {
        const std::size_t end = std::min(names.find('+', at), names.size());
        const std::string name = names.substr(at, end - at);
        bool found = false;
        for (const auto &entry : button_names)
            if (name == entry.name) {
                buttons |= button_mask(entry.button);
                found = true;
            }
        if (!found)
            return verdite::json_error("no such button '" + name + "'; try help");
        at = end + 1;
    }
    long hold = default_hold_ms;
    if (args.size() > 1) {
        char *end = nullptr;
        hold = std::strtol(args[1].c_str(), &end, 10);
        if (*end || hold < 1 || hold > max_hold_ms)
            return verdite::json_error("holdMs must be 1..600000");
    }
    press = {buttons, host_clock_ns() + static_cast<std::uint64_t>(hold) * ns_per_ms};
    return verdite::json_ok("press", ",\"button\":" + verdite::json_quote(names) + ",\"holdMs\":" + std::to_string(hold));
}

std::string do_release(verdite::AgentArgs) {
    press = {};
    return verdite::json_ok("release");
}

std::string do_quit(verdite::AgentArgs) {
    quit_requested = true;
    return verdite::json_ok("quit");
}
}

void agent_start() {
    verdite::AgentConfig config;
    config.tag = "KF1";
    config.port = verdite::agent_port_from(std::getenv("KF_SHELL"), default_port);
    config.beacon = verdite::agent_switch_from(std::getenv("KF_AGENT"));
    config.snapshot = snapshot;
    std::string buttons;
    for (const auto &entry : button_names)
        buttons += (buttons.empty() ? "" : " ") + std::string(entry.name);
    verdite::agent_start(config, {
        {"state", "state - the player and phase as JSON", do_state},
        {"press", "press <button>[+<button>...] [holdMs=150] - hold game buttons for game-clock ms, replacing the last press: " + buttons, do_press},
        {"release", "release - let go of the pressed buttons", do_release},
        {"quit", "quit - exit the game", do_quit},
    });
}

void agent_poll() {
    verdite::agent_poll();
    if (quit_requested) {
        // Let the client thread send the reply first.
        SDL_Delay(100);
        host_shutdown();
        std::exit(0);
    }
}

u32 agent_buttons() {
    return press.buttons && host_clock_ns() < press.until_ns ? press.buttons : 0;
}
}
