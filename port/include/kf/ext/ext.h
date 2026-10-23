#ifndef KF_EXT_EXT_H
#define KF_EXT_EXT_H
// PC extensions layered on the port: overlay UI (F1), console, settings and
// live-reloading Lua mods. Inspired by Verdite2 (github.com/Voicedrew11/verdite2).
#include <SDL3/SDL.h>
#include <cstdint>
#include <string>

namespace kf::ext {
// Captures stdout/stderr into the console panel. Call first thing in main.
void console_start();
void console_line(const std::string &line);

// After the GL context and renderer exist.
bool start(SDL_Window *window, SDL_GLContext context);
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
// Draws the overlay and mod HUDs over the frame about to be swapped.
void draw(int width, int height);
// A brief centred message, such as "Quick saved".
void show_message(const std::string &text);
}

#endif // KF_EXT_EXT_H
