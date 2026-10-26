#include <kf/platform/agent.h>
#include <kf/ext/ext.h>
#include <kf/platform/host.h>
#include <kf/platform/input.h>
#include <kf/platform/menu_pointer.h>
#include <kf/game/player.h>
#include <verdite/agent.h>
#include <verdite/crash.h>
#include <verdite/menu_pointer.h>
#include <SDL3/SDL.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <optional>
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

std::string snapshot() {
    const InputContext context = host_input_context();
    const auto &vitals = player_state.vitals;
    char text[512];
    // The player block is filled by a new game or a load; the opening leaves it stale.
    const bool in_game = context != InputContext::Opening && vitals.maximum_hp != 0;
    int n = std::snprintf(text, sizeof text, "{\"context\":\"%s\",\"inGame\":%s,\"clockMs\":%llu,\"frames\":%llu",
        input_context_name(context), in_game ? "true" : "false",
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
    verdite::crash_event("agent: press " + names + " " + std::to_string(hold) + " ms");
    return verdite::json_ok("press", ",\"button\":" + verdite::json_quote(names) + ",\"holdMs\":" + std::to_string(hold));
}

std::string do_release(verdite::AgentArgs) {
    press = {};
    return verdite::json_ok("release");
}

// The menu frame on screen: each row's widget (0 window, 1 list, 2 choice), box, and
// whether it is the cursor's and of the widget taking input.
std::string do_menu(verdite::AgentArgs) {
    std::string rows;
    for (const auto &row : host_menu_pointer().shown_rows()) {
        char text[160];
        std::snprintf(text, sizeof text, "%s{\"widget\":%u,\"row\":%d,\"box\":[%g,%g,%g,%g],\"cursor\":%s,\"input\":%s}",
            rows.empty() ? "" : ",", row.widget, row.row, row.box.x, row.box.y, row.box.width, row.box.height,
            row.cursor ? "true" : "false", row.input ? "true" : "false");
        rows += text;
    }
    return verdite::json_ok("menu", ",\"rows\":[" + rows + "]");
}

// A position in the original 320 x 240 pixels, from "<x> <y>".
std::optional<std::array<float, 2>> menu_position(verdite::AgentArgs args) {
    if (args.size() != 2)
        return std::nullopt;
    char *end_x = nullptr, *end_y = nullptr;
    const float x = std::strtof(args[0].c_str(), &end_x);
    const float y = std::strtof(args[1].c_str(), &end_y);
    if (*end_x || *end_y)
        return std::nullopt;
    return std::array{x, y};
}

std::string do_point(verdite::AgentArgs args) {
    if (host_input_context() != InputContext::Menu)
        return verdite::json_error("no menu is up");
    const auto position = menu_position(args);
    if (!position)
        return verdite::json_error("point <x> <y>");
    host_menu_pointer().moved((*position)[0], (*position)[1]);
    return verdite::json_ok("point");
}

std::string do_click(verdite::AgentArgs args) {
    if (host_input_context() != InputContext::Menu)
        return verdite::json_error("no menu is up");
    if (args.size() == 1 && args[0] == "right") {
        host_menu_pointer().back();
        verdite::crash_event("agent: click right");
        return verdite::json_ok("click");
    }
    const auto position = menu_position(args);
    if (!position)
        return verdite::json_error("click <x> <y> | click right");
    host_menu_pointer().pressed((*position)[0], (*position)[1]);
    host_menu_pointer().released((*position)[0], (*position)[1]);
    verdite::crash_event("agent: click " + args[0] + " " + args[1]);
    return verdite::json_ok("click");
}

std::string do_options(verdite::AgentArgs) { return verdite::json_ok("options", ",\"options\":" + ext::options_json()); }

std::string do_option(verdite::AgentArgs args) {
    if (args.empty() || args.size() > 2)
        return verdite::json_error("option <id> [value]");
    const std::string why = ext::option_change(args[0], args.size() == 2 ? args[1] : std::string());
    return why.empty() ? verdite::json_ok("option") : verdite::json_error(args[0] + ": " + why);
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
        {"menu", "menu - the rows of the menu on screen, in game pixels", do_menu},
        {"point", "point <x> <y> - move the mouse over a menu (game pixels)", do_point},
        {"click", "click <x> <y> | click right - click a menu with the mouse", do_click},
        {"options", "options - the settings this platform shows, with their values, as JSON", do_options},
        {"option", "option <id> [value] - change a setting (true/false, a number, a choice's value), or press an action", do_option},
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
