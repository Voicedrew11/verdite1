// Discord Rich Presence: the title, or the level, floor and place, HP and MP and
// EXP to the next level, and whether the menu, a shop or a save screen is up,
// with the time played this session. libverdite talks to Discord; this composes
// the text from the game's state.
//   KF_DISCORD=1|0       on or off for this run, over the setting
//   KF_DISCORD_APP=id    the Discord application, over application_id
#include "internal.hpp"
#include <kf/platform/prelude.h>
#include <kf/game/player.h>
#include <kf/platform/automap.h>
#include <kf/platform/input.h>
#include <kf/platform/screen.h>
#include <verdite/agent.h>
#include <verdite/discord.h>
#include <verdite/places.h>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace kf::ext {
namespace {
constexpr const char *application_id = "1562861926269059113";   // "King's Field JP"
constexpr const char *large_image = "kf1";
constexpr const char *large_text = "King's Field · the Royal Cemetery";
constexpr const char *separator = " · ";
constexpr std::uint64_t compose_period_ms = 50;
constexpr std::uint64_t idle_clear_ms = 10ull * 60 * 1000;   // no play for this long takes the presence down
constexpr int place_reach_steps = 10;

// Each name is a seed cell, placed from the 01Nintendo floor maps fitted to the
// collision grid; the walkable cells around it take its name (verdite/places.h).
struct FloorSeed {
    int floor;
    verdite::PlaceSeed seed;
};
constexpr std::array<FloorSeed, 41> place_seeds = {{
    {1, {15, 5, "Entrance"}},
    {1, {92, 94, "Dragon Temple"}},
    {1, {29, 12, "Dragon Spring"}},
    {1, {25, 56, "Arrow Trap Hall"}},
    {1, {69, 92, "Trap Switch"}},
    {1, {25, 41, "Light's Shop"}},
    {1, {8, 59, "Church"}},
    {1, {6, 49, "Ozzy's Chamber"}},
    {1, {78, 24, "Gill's Shop"}},
    {1, {34, 87, "Keeper's Map Room"}},
    {1, {64, 80, "Grave Robber's Tunnel"}},
    {2, {32, 62, "Eric Blair's Room"}},
    {2, {95, 6, "Sage's Cave"}},
    {2, {69, 87, "Heinz's Tunnel"}},
    {2, {33, 9, "Fairy's Grove"}},
    {2, {40, 74, "Iron Ball Trap"}},
    {2, {37, 33, "Dungeon Entrance"}},
    {2, {31, 17, "Portal to Floor 3"}},
    {2, {48, 54, "Treasure Room"}},
    {2, {43, 97, "Earth Seal Room"}},
    {2, {86, 92, "Northeast Save Room"}},
    {3, {65, 76, "Royal Treasury"}},
    {3, {16, 59, "Temple of Prayer"}},
    {3, {98, 58, "Old King's Chamber"}},
    {3, {33, 26, "Eric Blair's Room"}},
    {3, {32, 97, "Harp Bridge"}},
    {3, {43, 92, "Portal to Floor 4"}},
    {3, {33, 77, "Truth Mirror Ledge"}},
    {4, {16, 86, "Spider Room"}},
    {4, {16, 55, "Spider Room"}},
    {4, {85, 84, "Spider Room"}},
    {4, {85, 56, "Spider Room"}},
    {4, {7, 38, "Golem Chamber"}},
    {4, {38, 68, "Portal to Floor 5"}},
    {4, {53, 43, "Dark Magician's Lair"}},
    {4, {53, 69, "Great Hall"}},
    {5, {81, 45, "Dragon's Lair"}},
    {5, {39, 40, "Hall of Reinhardt II"}},
    {5, {37, 11, "King's Chamber"}},
    {5, {14, 32, "Portal Chamber"}},
    {5, {39, 90, "Great Chamber"}},
}};

struct Presence {
    std::optional<bool> forced;   // KF_DISCORD
    std::string application = application_id;
    bool started = false;
    bool in_game = false;
    std::int64_t session_start_s = 0;
    int places_floor = 0;
    verdite::PlaceMap places;
    std::string place;
    std::uint32_t automap_serial = 0;
    std::uint64_t last_play_ms = 0;
    std::uint64_t last_compose_ms = 0;
};
Presence presence;

bool enabled() { return presence.forced.value_or(settings().discord); }

std::int64_t unix_now_s() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string join(const std::vector<std::string> &parts) {
    std::string text;
    for (const auto &part : parts)
        text += (text.empty() ? "" : separator) + part;
    return text;
}

std::string with_thousands(long value) {
    std::string digits = std::to_string(value);
    for (int at = static_cast<int>(digits.size()) - 3; at > 0; at -= 3)
        digits.insert(static_cast<std::size_t>(at), ",");
    return digits;
}

void build_places(int floor, const AutomapSnapshot &map) {
    std::vector<std::uint8_t> walkable(static_cast<std::size_t>(automap_columns) * automap_rows);
    for (std::size_t i = 0; i < walkable.size(); ++i)
        walkable[i] = map.cells[i] != AutomapCell::Void && map.cells[i] != AutomapCell::Wall;
    std::vector<verdite::PlaceSeed> seeds;
    for (const auto &entry : place_seeds)
        if (entry.floor == floor)
            seeds.push_back(entry.seed);
    presence.places = verdite::PlaceMap(automap_columns, automap_rows, walkable, seeds, place_reach_steps);
    presence.places_floor = floor;
}

std::string place_at(const AutomapSnapshot &map) {
    if (map.floor < 1 || map.floor > automap_floor_count)
        return {};
    if (map.floor != presence.places_floor)
        build_places(map.floor, map);
    return std::string(presence.places.at(static_cast<int>(std::floor(map.player_x)),
                                          static_cast<int>(std::floor(map.player_z))));
}

std::string details_line(const AutomapSnapshot &map) {
    const auto &s = settings();
    std::vector<std::string> parts;
    if (s.discord_level && player_state.progress_state.level > 0)
        parts.push_back("Level " + std::to_string(player_state.progress_state.level));
    if (s.discord_location && map.floor >= 1 && map.floor <= automap_floor_count) {
        parts.push_back("Floor " + std::to_string(map.floor));
        if (!presence.place.empty())
            parts.push_back(presence.place);
    }
    return parts.empty() ? "Exploring" : join(parts);
}

const char *screen_label(GameScreen screen) {
    switch (screen) {
    case GameScreen::Menu: return "In the menu";
    case GameScreen::Shop: return "Shopping";
    case GameScreen::Save: return "Saving";
    case GameScreen::None: return nullptr;
    }
    return nullptr;
}

std::string state_line(GameScreen screen) {
    const auto &s = settings();
    const auto &vitals = player_state.vitals;
    std::vector<std::string> parts;
    if (const char *label = screen_label(screen))
        parts.emplace_back(label);
    if (s.discord_vitals)
        parts.push_back("HP " + std::to_string(vitals.current_hp) + "/" + std::to_string(vitals.maximum_hp) + separator +
                        "MP " + std::to_string(vitals.current_mp) + "/" + std::to_string(vitals.maximum_mp));
    if (s.discord_exp && player_state.next_level_experience > player_state.experience)
        parts.push_back(with_thousands(player_state.next_level_experience - player_state.experience) + " EXP to next level");
    return join(parts);
}

void note_place(const std::string &place) {
    if (place == presence.place)
        return;
    presence.place = place;
    std::printf("[KF1] discord: place %s\n", place.empty() ? "(none)" : place.c_str());
}

// None when the presence should come down: no play for a long while.
std::optional<verdite::DiscordActivity> compose(std::uint64_t now_ms) {
    // The player block is filled by a new game or a load; the opening leaves it stale.
    if (host_input_context() == InputContext::Opening || player_state.vitals.maximum_hp == 0) {
        presence.in_game = false;
        return verdite::DiscordActivity{"At the title screen", {}, large_image, large_text, 0};
    }
    const auto &map = automap_snapshot();
    if (!presence.in_game) {
        presence.in_game = true;
        presence.session_start_s = unix_now_s();
        presence.last_play_ms = now_ms;
        std::printf("[KF1] discord: in game, floor %d, level %u\n", map.floor, static_cast<unsigned>(player_state.progress_state.level));
    }
    // The game publishes the automap on every update, so a new serial is play.
    if (map.serial != presence.automap_serial) {
        presence.automap_serial = map.serial;
        presence.last_play_ms = now_ms;
    }
    const GameScreen screen = host_screen();
    if (screen == GameScreen::None && now_ms - presence.last_play_ms > idle_clear_ms)
        return std::nullopt;
    note_place(place_at(map));
    return verdite::DiscordActivity{details_line(map), state_line(screen), large_image, large_text,
                                    presence.session_start_s};
}

bool start_publisher() {
    presence.started = true;
    if (verdite::discord_start({presence.application, "KF1"}))
        return true;
    std::printf("[KF1] discord: no application id (KF_DISCORD_APP); presence off\n");
    return false;
}
}

void discord_start() {
    if (const char *spec = std::getenv("KF_DISCORD"); spec && *spec)
        presence.forced = verdite::agent_switch_from(spec);
    if (const char *app = std::getenv("KF_DISCORD_APP"); app && *app)
        presence.application = app;
}

void discord_poll() {
    if (!presence.started && (!enabled() || !start_publisher()))
        return;
    const std::uint64_t now_ms = SDL_GetTicks();
    if (now_ms - presence.last_compose_ms < compose_period_ms)
        return;
    presence.last_compose_ms = now_ms;
    const auto activity = enabled() ? compose(now_ms) : std::nullopt;
    if (activity)
        verdite::discord_show(*activity);
    else
        verdite::discord_clear();
}

void discord_stop() { verdite::discord_stop(); }

bool discord_forced() { return presence.forced.has_value(); }
bool discord_enabled() { return enabled(); }
}
