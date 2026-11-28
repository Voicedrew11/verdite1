// The PC extensions' start, events, pause and frame: the interface (port/ui/: the F1 menu, the
// setup, the update notice, the HUD and the mods' drawing) over the game's frame. The game pauses
// while a menu is open (configurable).
#include "build_info.h"
#include "internal.hpp"
#include "menu.h"
#include "shell.h"
#include <verdite/ui.h>
#include <kf/platform/host.h>
#include <kf/platform/language_runtime.h>
#include <kf/platform/user_folder.h>
#include <kf/renderer/frontend.h>
#include <kf/renderer/renderer.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace kf::ext {
namespace {
SDL_Window *main_window;
bool running;
bool drawn;   // false when headless
int fps_frames;
Uint64 fps_started;
float fps_value;
int last_context;

bool is_input_event(Uint32 type) {
    switch (type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: case SDL_EVENT_TEXT_INPUT: case SDL_EVENT_TEXT_EDITING:
    case SDL_EVENT_MOUSE_MOTION: case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
        return true;
    default:
        return false;
    }
}
}

SDL_Window *window() { return main_window; }

// A menu, the setup or the update notice covers the game.
static bool covered() {
    return menu_open() || setup_open() || update_notice_open();
}

bool overlay_open() { return covered(); }
bool overlay_captures_input() { return covered(); }
// A text field on a mod's page that has the keyboard takes the keys too, but not the mouse.
bool overlay_captures_keys() {
    const verdite::ui::Screen *screen = interface_screen();
    return covered() || (screen && screen->wants_text());
}
bool pause_requested() {
    return setup_open() || update_notice_open() || (menu_open() && settings().pause_when_open) ||
           mods_pause_requested();
}
bool pause_keeps_music() {
    if (menu_open() && settings().pause_when_open)
        return false;
    return mods_pause_keeps_music();
}
// A paused mod still gets on_draw, which is how it unpauses.
bool redraw_while_paused() {
    return menu_open() || setup_shows_game() || update_notice_open() || mods_pause_requested();
}
bool wants_cursor() { return covered() || mods_take_mouse(); }

ViewRect game_view_px() {
    int width = 0, height = 0;
    if (main_window)
        SDL_GetWindowSizeInPixels(main_window, &width, &height);
    const auto rect = renderer_present_rect(width, height);
    return {static_cast<float>(rect.x), static_cast<float>(rect.y), static_cast<float>(rect.width),
            static_cast<float>(rect.height)};
}

// The original 320 columns within a whole picture.
static ViewRect original_within(const ViewRect &whole) {
    const int extra = renderer_extra_columns();
    const float column = whole.w / static_cast<float>(render_width + 2 * extra);
    return {whole.x + column * static_cast<float>(extra), whole.y,
            column * static_cast<float>(render_width), whole.h};
}

ViewRect original_view_px() { return original_within(game_view_px()); }

bool start(SDL_Window *handle, Overlay overlay) {
    main_window = handle;
    settings_load();
    // Headless: settings and mods, no interface (nothing draws it).
    drawn = overlay == Overlay::Drawn;
    if (!drawn) {
        running = true;
        apply_audio_settings();
        apply_control_settings();
        apply_render_settings();
        mods_start();
        online_start();   // two headless instances can meet in a room
        setup_begin(overlay);
        return true;
    }
    interface_start(handle);   // without it the game runs on, menus and all unanswered
    running = true;
    apply_display_settings();
    apply_audio_settings();
    apply_control_settings();
    apply_render_settings();
    std::printf("[overlay] ready - press F1 for settings, mods and console. User folder: %s\n",
                user_dir().string().c_str());
    std::printf("[overlay] %s %s (%s)\n", product_name, KF_VERSION, KF_BUILD_COMMIT);
    update_notice_start();
    mods_start();
    discord_start();
    online_start();
    setup_begin(overlay);
    fps_started = SDL_GetTicks();
    return true;
}

void shutdown() {
    if (!running)
        return;
    mods_shutdown();
    discord_stop();
    online_stop();
    settings_page_abandon();
    settings_save();
    running = false;
    if (drawn)
        interface_stop();
}

bool process_event(const SDL_Event &event) {
    // Android may end the process in the background without an exit; the game sees the event too.
    if (event.type == SDL_EVENT_WILL_ENTER_BACKGROUND && running && !settings_page_holding())
        settings_save();
    if (event.type == SDL_EVENT_WILL_ENTER_BACKGROUND)
        online_went_to_background();
    if (!running || !drawn)
        return false;
    if (binding_capture_event(event))
        return true;
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
        const bool alt_enter = event.key.key == SDLK_RETURN && (event.key.mod & SDL_KMOD_ALT);
        if (event.key.key == SDLK_F11 || alt_enter) {
            settings().fullscreen = !settings().fullscreen;
            apply_display_settings();
            settings_mark_dirty();
            return true;
        }
        if (event.key.key == SDLK_F1) {
            menu_set_open(!menu_open());
            return true;
        }
    }
    if (event.type == SDL_EVENT_KEY_UP && (event.key.key == SDLK_F1 || event.key.key == SDLK_F11))
        return true;
    if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
        apply_render_settings();
    if (interface_event(event) || update_notice_event(event) || menu_event(event) || setup_event(event))
        return true;
    if (event.type == SDL_EVENT_MOUSE_WHEEL)
        mods_ui_wheel(event.wheel.y);
    // A mod that takes the mouse (the full map: zoom, drag, hover) gets it; the keyboard still reaches the game.
    const bool mouse_event = event.type == SDL_EVENT_MOUSE_MOTION || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
        || event.type == SDL_EVENT_MOUSE_BUTTON_UP || event.type == SDL_EVENT_MOUSE_WHEEL;
    if (mods_take_mouse() && mouse_event)
        return true;
    if (covered())
        return is_input_event(event.type);
    // The keys typed into a field on a mod's page are not the game's. A key let go still reaches the game,
    // so a key pressed before the box had the keyboard is not left held.
    const bool typed = event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_TEXT_INPUT ||
                       event.type == SDL_EVENT_TEXT_EDITING;
    const verdite::ui::Screen *screen = interface_screen();
    return typed && screen && screen->wants_text();
}

void poll() {
    if (!running)
        return;
    mods_poll();
    options_poll();
    update_notice_poll();
    online_poll();
    messages_poll();
    if (drawn) {
        discord_poll();
        touch_commands_poll();
    }
    settings_save_if_dirty();
}

void game_frame(int input_context) {
    if (!running)
        return;
    last_context = input_context;
    mods_frame(input_context);
}

static std::string message_text;
static Uint64 message_started;

void show_message(const std::string &text) {
    message_text = text;
    message_started = SDL_GetTicks();
}

// The message over the game: centred, fading out at the end.
static void draw_message(verdite::ui::Canvas &canvas, int width_px, int height_px) {
    constexpr Uint64 shown_ms = 1800, fade_ms = 400;
    const Uint64 age = SDL_GetTicks() - message_started;
    if (message_text.empty() || age >= shown_ms)
        return;
    const float alpha = age + fade_ms > shown_ms ? static_cast<float>(shown_ms - age) / fade_ms : 1.0f;
    const float size = std::max(18.0f, static_cast<float>(height_px) / 30.0f);
    const verdite::ui::Point extent = canvas.text_size(message_text, size);
    const verdite::ui::Point at{(static_cast<float>(width_px) - extent.x) * 0.5f, static_cast<float>(height_px) * 0.2f};
    canvas.rounded_rect({at.x - size * 0.6f, at.y - size * 0.3f, extent.x + size * 1.2f, extent.y + size * 0.6f},
                        size * 0.3f, {0, 0, 0, 150 / 255.0f * alpha});
    canvas.text(at, message_text, {1, 235 / 255.0f, 170 / 255.0f, alpha}, size);
}

static void count_frame() {
    ++fps_frames;
    const Uint64 now = SDL_GetTicks();
    if (now - fps_started >= 1000) {
        fps_value = static_cast<float>(fps_frames) * 1000.0f / static_cast<float>(now - fps_started);
        fps_frames = 0;
        fps_started = now;
    }
}

// The frame counter in the top left.
static void draw_fps(verdite::ui::Canvas &canvas, float px_per_dp) {
    char text[32];
    std::snprintf(text, sizeof text, "%.0f fps", fps_value);
    canvas.text({8 * px_per_dp, 6 * px_per_dp}, text, {1, 1, 120 / 255.0f, 1},
                hud_font_size * px_per_dp, verdite::ui::Shadow::Drop);
}

const verdite::ui::Frame *build_interface(int width_px, int height_px) {
    verdite::ui::Screen *screen = running ? interface_screen() : nullptr;
    if (!screen || width_px <= 0 || height_px <= 0)
        return nullptr;
    count_frame();
    // KF_TEST_SETTINGS=<page>: opens the menu on that page, for captures.
    static const char *test_page = std::getenv("KF_TEST_SETTINGS");
    if (test_page) {
        menu_open_page(test_page);
        test_page = nullptr;
        // KF_TEST_LANGUAGE_TASK=download: presses "Download English patch".
        if (const char *task = std::getenv("KF_TEST_LANGUAGE_TASK"); task && std::strcmp(task, "download") == 0)
            language_task_download_english();
    }
    while (std::optional<verdite::ui::Action> action = screen->take_action()) {
        menu_action(*action);
        setup_action(*action);
        update_notice_action(*action);
        mods_ui_action(*action);
    }
    update_notice_frame();
    setup_frame();
    menu_frame();
    verdite::ui::Canvas &front = screen->canvas(verdite::ui::Layer::Front);
    item_description_draw(screen->canvas(verdite::ui::Layer::Back));
    mods_draw_hud();
    touch_controls_draw(front);
    draw_message(front, width_px, height_px);
    if (settings().show_fps)
        draw_fps(front, screen->px_per_dp());
    return interface_frame(width_px, height_px);
}

}
