// KF1's interface on RmlUi (shell.h)
#include "shell.h"

#include "game_window_art.h"

#include "internal.hpp"

#include <verdite/ui.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <optional>
#include <span>
#include <string>

// cmake/ui_assets.cmake: libverdite's fonts and port/ui/res/, carried in the binary.
std::span<const verdite::ui::Asset> kf1_ui_assets();

namespace kf::ext {
namespace {
// The faces the style sheets name: libverdite's menu Mincho and the pad glyphs.
constexpr std::array<verdite::ui::FontFace, 4> font_faces = {{
    {"fonts/HolylandMincho.ttf", "Mincho"},
    {"fonts/kenney_input_xbox_series.ttf", "pad-xbox"},
    {"fonts/kenney_input_playstation_series.ttf", "pad-playstation"},
    {"fonts/kenney_input_nintendo_switch.ttf", "pad-nintendo"},
}};

class SdlHost final : public verdite::ui::ScreenHost {
public:
    SdlHost() = default;
    ~SdlHost() override {
        for (SDL_Cursor *cursor : cursors_)
            if (cursor)
                SDL_DestroyCursor(cursor);
    }
    SdlHost(const SdlHost &) = delete;
    SdlHost &operator=(const SdlHost &) = delete;

    double seconds() const override { return static_cast<double>(SDL_GetTicksNS()) / 1e9; }
    void set_cursor(verdite::ui::Cursor cursor) override {
        const auto index = static_cast<std::size_t>(cursor);
        if (!cursors_[index])
            cursors_[index] = SDL_CreateSystemCursor(system_cursor(cursor));
        if (cursors_[index] && !SDL_SetCursor(cursors_[index]))
            std::fprintf(stderr, "[ui] the pointer's shape: %s\n", SDL_GetError());
    }
    void set_clipboard(std::string_view utf8) override {
        if (!SDL_SetClipboardText(std::string(utf8).c_str()))
            std::fprintf(stderr, "[ui] the clipboard: %s\n", SDL_GetError());
    }
    std::string clipboard() const override {
        char *text = SDL_GetClipboardText();
        std::string out = text ? text : "";
        SDL_free(text);
        return out;
    }
    void log(std::string_view line) override {
        std::fprintf(stderr, "[ui] %.*s\n", static_cast<int>(line.size()), line.data());
    }

private:
    static SDL_SystemCursor system_cursor(verdite::ui::Cursor cursor) {
        switch (cursor) {
        case verdite::ui::Cursor::Hand: return SDL_SYSTEM_CURSOR_POINTER;
        case verdite::ui::Cursor::Text: return SDL_SYSTEM_CURSOR_TEXT;
        case verdite::ui::Cursor::Arrow: break;
        }
        return SDL_SYSTEM_CURSOR_DEFAULT;
    }
    std::array<SDL_Cursor *, 3> cursors_{};
};

struct Shell {
    SDL_Window *window = nullptr;
    SdlHost host;
    std::unique_ptr<verdite::ui::Screen> screen;
    bool typing = false;   // the platform's text input is on for a field
};
std::unique_ptr<Shell> shell;

// Window points (SDL's pointer events) to the frame's pixels.
float pixel_density() {
    const float density = SDL_GetWindowPixelDensity(shell->window);
    return density > 0 ? density : 1.0f;
}

// A dp in pixels, times the player's interface scale: the menu's window
// (kf1.rcss: 1200 x 760 dp) fitted to the frame with a margin, as holyland's
// menu is, or the display's own scale where that is larger (a phone).
float px_per_dp(int width_px, int height_px) {
    constexpr float fit_width_dp = 1260, fit_height_dp = 800;
    const float fit = std::min(static_cast<float>(width_px) / fit_width_dp,
                               static_cast<float>(height_px) / fit_height_dp);
    const float display = SDL_GetWindowDisplayScale(shell->window);
    return std::max({fit, display > 0 ? display : 1.0f, 0.4f}) * settings().ui_scale;
}

bool pointer_event(const SDL_Event &event, verdite::ui::Screen &screen) {
    const float density = pixel_density();
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
        if (event.motion.which == SDL_TOUCH_MOUSEID)
            return false;   // (a finger's, which touch_event has)
        screen.pointer_move(event.motion.x * density, event.motion.y * density);
        return screen.pointer_over_document();
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (event.button.which == SDL_TOUCH_MOUSEID)
            return false;
        if (event.button.button != SDL_BUTTON_LEFT)
            return screen.pointer_over_document();   // (the game's right click is not taken through a page)
        const float x = event.button.x * density;
        const float y = event.button.y * density;
        const bool over = screen.pointer_over_document();
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
            screen.pointer_down(x, y);
        else
            screen.pointer_up(x, y);
        return over;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (!screen.pointer_over_document())
            return false;
        screen.wheel(-event.wheel.y);
        return true;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        screen.pointer_leave();
        return false;
    default:
        return false;
    }
}

bool touch_event(const SDL_Event &event, verdite::ui::Screen &screen) {
    int width = 0, height = 0;
    if (!SDL_GetWindowSizeInPixels(shell->window, &width, &height))
        return false;
    const float x = event.tfinger.x * static_cast<float>(width);
    const float y = event.tfinger.y * static_cast<float>(height);
    switch (event.type) {
    case SDL_EVENT_FINGER_DOWN: {
        screen.pointer_move(x, y);
        const bool over = screen.pointer_over_document();
        if (over)
            screen.touch_down(static_cast<std::int64_t>(event.tfinger.fingerID), x, y);
        return over;
    }
    case SDL_EVENT_FINGER_MOTION:
        screen.touch_move(static_cast<std::int64_t>(event.tfinger.fingerID), x, y);
        return screen.pointer_over_document();
    case SDL_EVENT_FINGER_UP:
        screen.touch_up(static_cast<std::int64_t>(event.tfinger.fingerID), x, y);
        return false;   // (the game lets go of its own fingers)
    default:
        return false;
    }
}

// SDL's keys that text fields and their selection use; nothing for the rest.
std::optional<verdite::ui::TextKey> text_key(const SDL_KeyboardEvent &key) {
    using verdite::ui::TextKey;
    const bool control = (key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0;
    switch (key.key) {
    case SDLK_BACKSPACE: return TextKey::Backspace;
    case SDLK_DELETE: return TextKey::Delete;
    case SDLK_LEFT: return TextKey::Left;
    case SDLK_RIGHT: return TextKey::Right;
    case SDLK_UP: return TextKey::Up;
    case SDLK_DOWN: return TextKey::Down;
    case SDLK_HOME: return TextKey::Home;
    case SDLK_END: return TextKey::End;
    case SDLK_RETURN:
    case SDLK_KP_ENTER: return TextKey::Enter;
    case SDLK_TAB: return TextKey::Tab;
    case SDLK_ESCAPE: return TextKey::Escape;
    case SDLK_A: return control ? std::optional(TextKey::SelectAll) : std::nullopt;
    case SDLK_C: return control ? std::optional(TextKey::Copy) : std::nullopt;
    case SDLK_X: return control ? std::optional(TextKey::Cut) : std::nullopt;
    case SDLK_V: return control ? std::optional(TextKey::Paste) : std::nullopt;
    default: return std::nullopt;
    }
}

// Keys and text while a field has the keyboard; then the game sees neither.
bool typing_event(const SDL_Event &event, verdite::ui::Screen &screen) {
    if (!screen.wants_text())
        return false;
    if (event.type == SDL_EVENT_TEXT_INPUT) {
        screen.text_input(event.text.text);
        return true;
    }
    if (event.type == SDL_EVENT_KEY_DOWN) {
        if (std::optional<verdite::ui::TextKey> key = text_key(event.key)) {
            const bool shift = (event.key.mod & SDL_KMOD_SHIFT) != 0;
            screen.key(*key, shift ? verdite::ui::KeyMods::Shift : verdite::ui::KeyMods::None);
        }
        return true;
    }
    return event.type == SDL_EVENT_TEXT_EDITING;
}

// The platform's text input (a phone's keyboard) on while a field has the keyboard.
void follow_typing() {
    const bool wanted = shell->screen->wants_text();
    if (wanted == shell->typing)
        return;
    shell->typing = wanted;
    const bool done = wanted ? SDL_StartTextInput(shell->window) : SDL_StopTextInput(shell->window);
    if (!done)
        std::fprintf(stderr, "[ui] text input: %s\n", SDL_GetError());
}
} // namespace

bool interface_start(SDL_Window *window) {
    shell = std::make_unique<Shell>();
    shell->window = window;
    verdite::ui::ScreenSetup setup;
    setup.host = &shell->host;
    setup.assets = kf1_ui_assets();
    setup.fonts = font_faces;
    setup.images = &game_window_art();
    shell->screen = verdite::ui::Screen::open(setup);
    if (!shell->screen) {
        std::fprintf(stderr, "[ui] the interface did not start: the game runs without it\n");
        shell.reset();
        return false;
    }
    return true;
}

void interface_stop() {
    shell.reset();
}

verdite::ui::Screen *interface_screen() {
    return shell ? shell->screen.get() : nullptr;
}

bool interface_event(const SDL_Event &event) {
    if (!shell)
        return false;
    verdite::ui::Screen &screen = *shell->screen;
    switch (event.type) {
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP:
        return touch_event(event, screen);
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_TEXT_EDITING:
        return typing_event(event, screen);
    default:
        return pointer_event(event, screen);
    }
}

const verdite::ui::Frame *interface_frame(int width_px, int height_px) {
    if (!shell || width_px <= 0 || height_px <= 0)
        return nullptr;
    shell->screen->resize(width_px, height_px, px_per_dp(width_px, height_px));
    follow_typing();
    return &shell->screen->draw();
}

} // namespace kf::ext
