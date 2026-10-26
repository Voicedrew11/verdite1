#ifndef KF_EXT_EXT_H
#define KF_EXT_EXT_H
// PC extensions layered on the port: overlay UI (F1), console, settings and
// live-reloading Lua mods. Inspired by Verdite2 (github.com/Voicedrew11/verdite2).
#include <SDL3/SDL.h>
#include <cstdint>
#include <string>
#include <string_view>

struct ImDrawData;
namespace verdite { class CrashWriter; }

namespace kf::ext {
// Whether Dear ImGui is drawn: not headless, where nothing is.
enum class Overlay : std::uint8_t { Hidden, Drawn };
// Captures stdout/stderr into the console panel. Call first thing in main.
void console_start();
void console_line(const std::string &line);

// After the window and renderer exist.
bool start(SDL_Window *window, Overlay overlay);
void shutdown();

// Returns true when the overlay consumed the event (the game must not see it).
bool process_event(const SDL_Event &event);
bool overlay_open();
// The game clock and audio stop while this is true.
bool pause_requested();
// While pause_requested: whether the music keeps playing (the full map).
bool pause_keeps_music();
// The overlay or full map is showing while the game is held.
bool redraw_while_paused();
// The overlay or full map wants a free mouse cursor.
bool wants_cursor();
// File watching and settings persistence; cheap enough to call on every host poll.
void poll();
// Once per game-presented frame, before draw().
void game_frame(int input_context);
// The overlay and mod HUDs for this frame, for the frontend to draw; null if none.
ImDrawData *build_overlay(int width, int height);
// A brief centred message, such as "Quick saved".
void show_message(const std::string &text);
// The settings as this platform shows them, as JSON (verdite::options_json), for the
// agent channel and Android's settings panel.
std::string options_json();
// Changes an option by id from typed text (verdite::option_value_from_text), or
// presses an action given no text. Empty on success, else why not.
std::string option_change(std::string_view id, std::string_view text);
// For a crash report: the settings that change how the game plays or draws. Runs in the crash
// handler, so it only reads.
void crash_write_settings(verdite::CrashWriter &out);
}

#endif // KF_EXT_EXT_H
