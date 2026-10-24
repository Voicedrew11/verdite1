#include <kf/platform/host.h>
#include <kf/platform/language_runtime.h>
#include <kf/platform/saves.h>
#include <kf/audio/sound.h>
#include <kf/platform/input.h>
#include <kf/platform/controls.h>
#include <kf/platform/agent.h>
#include <kf/renderer/backend.h>
#include <kf/renderer/frontend.h>
#include <kf/renderer/renderer.h>
#include <kf/renderer/projection.h>
#include <kf/lib/fixed_math.h>
#include "../renderer/vertex_carry.h"
#include <SDL3/SDL.h>
#if KF_EXT
#include <kf/ext/ext.h>
#endif
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <vector>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>

EM_JS(void, host_browser_status, (const char *message), {
    document.getElementById('status').textContent = UTF8ToString(message);
});
EM_JS(int, host_browser_mouse_captured, (), {
    return document.pointerLockElement === Module.canvas;
});
EM_JS(void, host_browser_language, (const char *current, const char *requested, const char *message), {
    Module['languageChanged'](UTF8ToString(current), UTF8ToString(requested), UTF8ToString(message));
});
#endif

namespace kf {
struct HostState {
    SDL_Window *window;
    SDL_Gamepad *gamepad;
    std::vector<DrawFace> frame_faces;
    InputState input;
    InputContext input_context;
    std::array<SDL_Keycode, SDL_SCANCODE_COUNT> pressed_keys;
    Uint64 epoch, paused_ns, pause_start;
    double look_remainder_x, look_remainder_y;
    bool focused, mouse_captured, resume_mouse_capture;
    bool paused, overlay_was_open, cursor_released, recapture_mouse;
    u32 quick_pressed;
    bool opening_load_pressed;
    bool run_toggled;
    PadLayout pad_layout;
    struct FrameLight {
        float x, y, z, radius, r, g, b, flicker;
        u32 seed;
    } frame_lights[render_light_capacity];
    int frame_light_count;
    std::uint64_t frames_presented;
    Uint64 turbo_ns;   // the game clock under turbo
    LookDelta look_consumed;   // by the game update being drawn
};
static HostState host;
static HostOptions options;
static constexpr Uint64 ns_per_second = 1000000000;
static constexpr Uint64 ns_per_millisecond = 1000000;
static constexpr Uint64 clock_ticks_per_second = 60;
static constexpr Uint64 unfocused_poll_interval_ns = 16 * ns_per_millisecond;
static constexpr Uint64 maximum_wait_slice_ns = 8 * ns_per_millisecond;
static constexpr u32 gamepad_axis_control_first = 100;
static constexpr u32 gamepad_axis_positive_first = gamepad_axis_control_first + 1;
static constexpr int gamepad_axis_deadzone = 16000;
static constexpr double mouse_degrees_per_pixel = 0.12;
static constexpr double angle_units_per_turn = 4096;
static constexpr double degrees_per_turn = 360;

static void frontend_present(Uint64 now);

static void platform_yield(Uint64 nanoseconds) {
#ifdef __EMSCRIPTEN__
    emscripten_sleep(static_cast<unsigned>((nanoseconds + ns_per_millisecond - 1) / ns_per_millisecond));
#else
    SDL_DelayNS(nanoseconds);
#endif
}

struct GamepadBinding { SDL_GamepadButton code; Action action; };

// The face buttons by position (SDL's south is Cross on a PlayStation pad).
// Japanese is this disc's own; West is the US King's Field's: Square attacks,
// Cross examines and confirms, Circle opens the menu and backs out, Triangle
// uses the item or spell.
constexpr std::array<GamepadBinding, 4> japanese_face_buttons = {
    GamepadBinding{SDL_GAMEPAD_BUTTON_NORTH, Action::attack}, GamepadBinding{SDL_GAMEPAD_BUTTON_WEST, Action::magic},
    GamepadBinding{SDL_GAMEPAD_BUTTON_EAST, Action::interact}, GamepadBinding{SDL_GAMEPAD_BUTTON_SOUTH, Action::back}};
constexpr std::array<GamepadBinding, 4> west_face_buttons = {
    GamepadBinding{SDL_GAMEPAD_BUTTON_WEST, Action::attack}, GamepadBinding{SDL_GAMEPAD_BUTTON_NORTH, Action::magic},
    GamepadBinding{SDL_GAMEPAD_BUTTON_SOUTH, Action::interact}, GamepadBinding{SDL_GAMEPAD_BUTTON_EAST, Action::back}};

static std::span<const GamepadBinding> face_buttons(PadLayout layout) {
    return layout == PadLayout::Japanese ? std::span(japanese_face_buttons) : std::span(west_face_buttons);
}

static void bind_gamepad_buttons(std::span<const GamepadBinding> buttons) {
    for (const auto &button : buttons)
        input_bind(&host.input, {InputDevice::gamepad, static_cast<u32>(button.code)}, button.action);
}

static void bind_inputs() {
    auto *input = &host.input;
    struct KeyBinding { SDL_Keycode code; Action action; };
    constexpr std::array<KeyBinding, 23> keys = {
        KeyBinding{SDLK_W, Action::forward}, KeyBinding{SDLK_UP, Action::forward},
        KeyBinding{SDLK_S, Action::backward}, KeyBinding{SDLK_DOWN, Action::backward},
        KeyBinding{SDLK_A, Action::strafe_left}, KeyBinding{SDLK_D, Action::strafe_right},
        KeyBinding{SDLK_LEFT, Action::turn_left}, KeyBinding{SDLK_RIGHT, Action::turn_right},
        KeyBinding{SDLK_PAGEUP, Action::look_up}, KeyBinding{SDLK_PAGEDOWN, Action::look_down},
        KeyBinding{SDLK_E, Action::interact}, KeyBinding{SDLK_RETURN, Action::confirm},
        KeyBinding{SDLK_SPACE, Action::attack}, KeyBinding{SDLK_Q, Action::magic},
        KeyBinding{SDLK_TAB, Action::inventory}, KeyBinding{SDLK_BACKSPACE, Action::back},
        KeyBinding{SDLK_ESCAPE, Action::pause_or_back}, KeyBinding{SDLK_P, Action::pause},
        KeyBinding{SDLK_LSHIFT, Action::run}, KeyBinding{SDLK_RSHIFT, Action::run},
        KeyBinding{SDLK_F2, Action::quick_save}, KeyBinding{SDLK_F4, Action::quick_load},
        KeyBinding{SDLK_R, Action::compare_language}
    };
    for (const auto &key : keys)
        input_bind(input, {InputDevice::keyboard, static_cast<u32>(key.code)}, key.action);
    input_bind(input, {InputDevice::mouse, SDL_BUTTON_LEFT}, Action::attack);
    input_bind(input, {InputDevice::mouse, SDL_BUTTON_RIGHT}, Action::magic);
    constexpr std::array<GamepadBinding, 10> buttons = {
        GamepadBinding{SDL_GAMEPAD_BUTTON_DPAD_UP, Action::forward}, GamepadBinding{SDL_GAMEPAD_BUTTON_DPAD_DOWN, Action::backward},
        GamepadBinding{SDL_GAMEPAD_BUTTON_DPAD_LEFT, Action::turn_left}, GamepadBinding{SDL_GAMEPAD_BUTTON_DPAD_RIGHT, Action::turn_right},
        GamepadBinding{SDL_GAMEPAD_BUTTON_START, Action::inventory}, GamepadBinding{SDL_GAMEPAD_BUTTON_BACK, Action::pause},
        GamepadBinding{SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, Action::strafe_left},
        GamepadBinding{SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, Action::strafe_right},
        GamepadBinding{SDL_GAMEPAD_BUTTON_LEFT_STICK, Action::run},
        GamepadBinding{SDL_GAMEPAD_BUTTON_RIGHT_STICK, Action::compare_language}
    };
    bind_gamepad_buttons(buttons);
    bind_gamepad_buttons(face_buttons(host.pad_layout));
    // Axis directions use distinct controls so releasing a stick cannot release a held D-pad.
    input_bind(input, {InputDevice::gamepad, gamepad_axis_control_first + SDL_GAMEPAD_AXIS_LEFTX * 2}, Action::turn_left);
    input_bind(input, {InputDevice::gamepad, gamepad_axis_positive_first + SDL_GAMEPAD_AXIS_LEFTX * 2}, Action::turn_right);
    input_bind(input, {InputDevice::gamepad, gamepad_axis_control_first + SDL_GAMEPAD_AXIS_LEFTY * 2}, Action::forward);
    input_bind(input, {InputDevice::gamepad, gamepad_axis_positive_first + SDL_GAMEPAD_AXIS_LEFTY * 2}, Action::backward);
    input_bind(input, {InputDevice::gamepad, gamepad_axis_positive_first + SDL_GAMEPAD_AXIS_LEFT_TRIGGER * 2}, Action::look_up);
    input_bind(input, {InputDevice::gamepad, gamepad_axis_positive_first + SDL_GAMEPAD_AXIS_RIGHT_TRIGGER * 2}, Action::look_down);
}

static void open_available_gamepad() {
    int count = 0;
    auto *ids = SDL_GetGamepads(&count);
    for (int i = 0; i < count && !host.gamepad; ++i)
        host.gamepad = SDL_OpenGamepad(ids[i]);
    SDL_free(ids);
}

// The window and the renderer's backend: Vulkan, or headless a window on the
// dummy driver (settings and the overlay's state hang off it) and a backend
// that draws nothing.
static bool start_window() {
    if (options.headless) {
        host.window = SDL_CreateWindow("King's Field", 960, 720, 0);
        if (!host.window) {
            std::fprintf(stderr, "%s\n", SDL_GetError());
            return false;
        }
        return renderer_start(make_null_backend());
    }
    host.window = SDL_CreateWindow("King's Field", 960, 720, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!host.window) {
        std::fprintf(stderr, "[renderer] no Vulkan window: %s\n", SDL_GetError());
        return false;
    }
    BackendStart started = make_vulkan_backend(host.window);
    if (started.backend && renderer_start(std::move(started.backend)))
        return true;
    std::fprintf(stderr, "[renderer] %s\n", started.error.empty() ? "Vulkan cannot draw" : started.error.c_str());
    return false;
}

void host_configure(const HostOptions &chosen) { options = chosen; }

bool host_start() {
    if (options.headless) {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
        SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "%s\n", SDL_GetError());
        return false;
    }
    if (!start_window()) {
        host_shutdown();
        return false;
    }
    bind_inputs();
    open_available_gamepad();
    host.epoch = SDL_GetTicksNS();
    host.focused = true;
    if (!sound_start()) {
        host_shutdown();
        return false;
    }
#if KF_EXT
    ext::start(host.window, options.headless ? ext::Overlay::Hidden : ext::Overlay::Drawn);
#endif
    agent_start();
    if (options.headless || options.turbo)
        std::printf("Host:%s%s\n", options.headless ? " headless" : "", options.turbo ? " turbo" : "");
    return true;
}

void host_shutdown() {
#if KF_EXT
    ext::shutdown();
#endif
    language_resources_stop();
    sound_shutdown();
    save_storage_shutdown();
    if (host.gamepad)
        SDL_CloseGamepad(host.gamepad);
    renderer_stop();
    if (host.window)
        SDL_DestroyWindow(host.window);
    host = {};
    SDL_Quit();
}

void host_language_status(const char *message) {
    std::printf("%s (%s)\n", message, language_name(game_language()));
    std::fflush(stdout);
#ifdef __EMSCRIPTEN__
    host_browser_language(language_code(game_language()), language_code(language_requested()), message);
#endif
}

[[noreturn]] void host_fail(const char *message) {
    std::fprintf(stderr, "%s\n", message);
    std::fflush(stderr);
#ifdef __EMSCRIPTEN__
    host_browser_status(message);
#else
    if (host.window && !options.headless) {
        SDL_SetWindowRelativeMouseMode(host.window, false);
        sound_set_paused(true);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "King's Field — Error", message, host.window);
    }
#endif
    host_shutdown();
    std::exit(1);
}

static bool mouse_is_captured() {
    if (!host.window || !SDL_GetWindowRelativeMouseMode(host.window))
        return false;
#ifdef __EMSCRIPTEN__
    return host_browser_mouse_captured() != 0;
#else
    return true;
#endif
}

static void refresh_mouse_capture() {
    const bool captured = mouse_is_captured();
    if (!captured && host.mouse_captured) {
        if (host.input_context == InputContext::Gameplay || host.input_context == InputContext::Scripted)
            host.resume_mouse_capture = false;
        input_disconnect(&host.input, InputDevice::mouse);
        host.input.pending.look_x = host.input.pending.look_y = 0;
        host.look_remainder_x = host.look_remainder_y = 0;
    }
    host.mouse_captured = captured;
}

static void process_keyboard_event(const SDL_KeyboardEvent &event) {
    if (event.repeat || event.scancode <= SDL_SCANCODE_UNKNOWN || event.scancode >= SDL_SCANCODE_COUNT)
        return;
    auto &pressed_key = host.pressed_keys[event.scancode];
    if (event.down) {
        if (pressed_key != SDLK_UNKNOWN)
            return;
        // Letter shortcuts follow physical QWERTY positions in every layout.
        // Other keys retain logical remaps, including Caps Lock to Escape.
        if (event.scancode >= SDL_SCANCODE_A && event.scancode <= SDL_SCANCODE_Z)
            pressed_key = SDLK_A + static_cast<SDL_Keycode>(event.scancode - SDL_SCANCODE_A);
        else
            pressed_key = SDL_GetKeyFromScancode(event.scancode, SDL_KMOD_NONE, false);
        if (pressed_key != SDLK_UNKNOWN)
            input_button(&host.input, {InputDevice::keyboard, pressed_key}, true);
    } else {
        // Remember the key-down mapping even if the layout changes while held.
        const auto released_key = pressed_key;
        pressed_key = SDLK_UNKNOWN;
        if (released_key == SDLK_UNKNOWN)
            return;
        // Two physical keys can both map to Escape; releasing one must not
        // release the logical action while the other is still down.
        for (const auto held_key : host.pressed_keys)
            if (held_key == released_key)
                return;
        input_button(&host.input, {InputDevice::keyboard, released_key}, false);
    }
}

static void process_event(const SDL_Event &event) {
#if KF_EXT
    if (ext::process_event(event))
        return;
#endif
    switch (event.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        host_shutdown();
        std::exit(0);
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if (host.focused)
            process_keyboard_event(event.key);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (!host.focused)
            break;
        if (event.button.down && host.input_context == InputContext::Gameplay &&
                (!host.mouse_captured || !mouse_is_captured())) {
            SDL_SetWindowRelativeMouseMode(host.window, true);
            break;
        }
        input_button(&host.input, {InputDevice::mouse, event.button.button}, event.button.down);
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (host.focused && host.input_context == InputContext::Gameplay &&
                mouse_is_captured()) {
            double x = event.motion.xrel, y = event.motion.yrel;
#ifdef __EMSCRIPTEN__
            double css_width = 0, css_height = 0;
            int width = 0, height = 0;
            SDL_GetWindowSize(host.window, &width, &height);
            if (width > 0 && height > 0 && emscripten_get_element_css_size("#canvas", &css_width, &css_height) == EMSCRIPTEN_RESULT_SUCCESS) {
                x *= css_width / width;
                y *= css_height / height;
            }
#endif
            input_motion(&host.input, x, y);
        }
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        if (!host.gamepad)
            host.gamepad = SDL_OpenGamepad(event.gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (host.gamepad && event.gdevice.which == SDL_GetGamepadID(host.gamepad)) {
            SDL_CloseGamepad(host.gamepad);
            host.gamepad = nullptr;
            input_disconnect(&host.input, InputDevice::gamepad);
            open_available_gamepad();
        }
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        if (host.focused && host.gamepad && event.gbutton.which == SDL_GetGamepadID(host.gamepad))
            input_button(&host.input, {InputDevice::gamepad, event.gbutton.button}, event.gbutton.down);
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (host.focused && host.gamepad && event.gaxis.which == SDL_GetGamepadID(host.gamepad)) {
            input_button(&host.input, {InputDevice::gamepad, gamepad_axis_control_first + event.gaxis.axis * 2u}, event.gaxis.value < -gamepad_axis_deadzone);
            input_button(&host.input, {InputDevice::gamepad, gamepad_axis_positive_first + event.gaxis.axis * 2u}, event.gaxis.value > gamepad_axis_deadzone);
        }
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        if (host.focused) {
            host.focused = false;
            host.resume_mouse_capture = false;
            input_clear(&host.input);
            host.pressed_keys.fill(SDLK_UNKNOWN);
            host.look_remainder_x = host.look_remainder_y = 0;
            SDL_SetWindowRelativeMouseMode(host.window, false);
        }
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        if (!host.focused)
            host.focused = true;
        break;
    case SDL_EVENT_WINDOW_EXPOSED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_RESTORED:
        // Modal waits still service presentation. Never resubmit their blended
        // draw commands here: the offscreen frame already contains the result.
        if (event.window.windowID == SDL_GetWindowID(host.window))
            frontend_present(host_clock_ns());
        break;
    default: break;
    }
}

// The game clock and audio stop while the window is unfocused or the overlay
// asks for a pause; the paused interval is excluded from host_clock_ns().
static void update_pause() {
    // KF_TEST_NO_PAUSE: keep running unfocused (automated testing).
    static const bool focus_pauses = !std::getenv("KF_TEST_NO_PAUSE");
    const bool unfocused = focus_pauses && !host.focused;
    bool wanted = unfocused;
    SoundPause audio = unfocused ? SoundPause::All : SoundPause::None;
#if KF_EXT
    if (!unfocused && ext::pause_requested()) {
        wanted = true;
        audio = ext::pause_keeps_music() ? SoundPause::KeepMusic : SoundPause::All;
    }
#endif
    static SoundPause applied = SoundPause::None;
    if (wanted != host.paused) {
        if (wanted)
            host.pause_start = SDL_GetTicksNS();
        else
            host.paused_ns += SDL_GetTicksNS() - host.pause_start;
        host.paused = wanted;
    }
    if (audio != applied) {
        sound_set_pause(audio);
        applied = audio;
    }
}

#if KF_EXT
// Opening or closing the overlay drops held game input so nothing stays pressed.
// While the overlay or the full map wants the cursor, mouse capture is released,
// and it returns when they close if the game had it.
static void sync_overlay_input() {
    const bool open = ext::overlay_open();
    if (open != host.overlay_was_open) {
        host.overlay_was_open = open;
        input_clear(&host.input);
        host.pressed_keys.fill(SDLK_UNKNOWN);
        host.look_remainder_x = host.look_remainder_y = 0;
    }
    const bool cursor = ext::wants_cursor();
    if (cursor == host.cursor_released)
        return;
    host.cursor_released = cursor;
    if (cursor) {
        host.recapture_mouse = mouse_is_captured();
        input_disconnect(&host.input, InputDevice::mouse);
        SDL_SetWindowRelativeMouseMode(host.window, false);
    } else if (host.recapture_mouse && host.focused && host.input_context == InputContext::Gameplay) {
        SDL_SetWindowRelativeMouseMode(host.window, true);
    }
}
#endif

void host_poll() {
    // Only platform work runs here. In the browser each yield suspends the existing
    // game stack; event callbacks never enter a second gameplay call chain.
    for (;;) {
        SDL_Event event;
        while (SDL_PollEvent(&event))
            process_event(event);
        refresh_mouse_capture();
        agent_poll();
        sound_poll();
#if KF_EXT
        ext::poll();
        sync_overlay_input();
#endif
        update_pause();
        if (!host.paused)
            return;
        // Mouse movement while held (full map, overlay) must not turn the camera afterwards.
        host.input.pending.look_x = host.input.pending.look_y = 0;
#if KF_EXT
        // The game is held here, so keep the overlay itself animating.
        if (ext::redraw_while_paused() && host.focused)
            frontend_present(host_clock_ns());
#endif
        platform_yield(unfocused_poll_interval_ns);
    }
}

std::uint64_t host_clock_ns() {
    if (options.turbo)
        return host.turbo_ns;
    const auto now = host.paused ? host.pause_start : SDL_GetTicksNS();
    return now - host.epoch - host.paused_ns;
}

std::uint64_t host_clock_tick() {
    const auto elapsed = host_clock_ns();
    return elapsed / ns_per_second * clock_ticks_per_second + elapsed % ns_per_second * clock_ticks_per_second / ns_per_second;
}

static bool frontend_wants_frames();
static Uint64 frontend_next_present_ns();

static void wait_until_ns(std::uint64_t target) {
    if (options.turbo) {
        // Paused (overlay, unfocused): host_poll holds here, and the clock with it.
        host_poll();
        host.turbo_ns = std::max<Uint64>(host.turbo_ns, target);
        return;
    }
    for (;;) {
        host_poll();
        const auto now = host_clock_ns();
        if (now >= target)
            return;
        Uint64 slice = std::min<Uint64>(target - now, maximum_wait_slice_ns);
        if (frontend_wants_frames()) {
            if (now >= frontend_next_present_ns()) {
                frontend_present(now);
                continue;
            }
            slice = std::min<Uint64>(slice, frontend_next_present_ns() - now);
        }
        platform_yield(slice);
    }
}

void host_wait_until_tick(std::uint64_t deadline) {
    const auto target = deadline / clock_ticks_per_second * ns_per_second +
                        (deadline % clock_ticks_per_second * ns_per_second + clock_ticks_per_second - 1) / clock_ticks_per_second;
    wait_until_ns(target);
}

void host_wait_frame() {
    host_wait_until_tick(host_clock_tick() + 1);
}

UpdatePacer host_begin_update_pacer(u32 steps_per_second) {
    if (steps_per_second == 0 || steps_per_second > ns_per_second)
        host_fail("Invalid update frequency");
    host_poll();
    return {host_clock_ns(), steps_per_second, 0};
}

void host_wait_update(UpdatePacer &pacer) {
    const auto rate = pacer.steps_per_second;
    if (rate == 0 || rate > ns_per_second)
        host_fail("Uninitialized update pacer");
    // Carry fractional nanoseconds; work and presentation consume this interval.
    const auto fraction = pacer.fractional_ns + ns_per_second % rate;
    pacer.deadline_ns += ns_per_second / rate + fraction / rate;
    pacer.fractional_ns = static_cast<u32>(fraction % rate);
    wait_until_ns(pacer.deadline_ns);
    const auto now = host_clock_ns();
    if (now - pacer.deadline_ns >= (ns_per_second + rate - 1) / rate) {
        // A long stall must not cause a burst of catch-up game steps.
        pacer.deadline_ns = now;
        pacer.fractional_ns = 0;
    }
}

// KF_TEST_INPUT="poll:action:polls,...": holds an action from a given
// host_read_buttons call for a number of calls, for automated menu tests.
// Actions: up, down, left, right, confirm, back, inventory.
static u32 test_input_actions() {
    struct Step { int start, length; Action action; };
    static std::vector<Step> steps;
    static int polls = -1;
    if (polls < 0) {
        polls = 0;
        const char *script = std::getenv("KF_TEST_INPUT");
        const struct { const char *name; Action action; } names[] = {
            {"up", Action::forward}, {"down", Action::backward}, {"left", Action::turn_left},
            {"right", Action::turn_right}, {"confirm", Action::confirm}, {"back", Action::back},
            {"inventory", Action::inventory}};
        while (script && *script) {
            int start = 0, length = 0;
            char name[16] = {};
            if (std::sscanf(script, "%d:%15[a-z]:%d", &start, name, &length) == 3)
                for (const auto &entry : names)
                    if (std::strcmp(entry.name, name) == 0)
                        steps.push_back({start, length, entry.action});
            script = std::strchr(script, ',');
            if (script)
                ++script;
        }
    }
    const int now = polls++;
    u32 held = 0;
    for (const auto &step : steps)
        if (now >= step.start && now < step.start + step.length)
            held |= action_bit(step.action);
    return held;
}

bool host_action_held(Action action) {
    host_poll();
    return (host.input.pending.held & action_bit(action)) != 0;
}

u32 host_read_buttons() {
    host_poll();
    // Original game code samples held buttons and derives its own edges.
    const u32 actions = host.input.pending.held | test_input_actions();
    // Quick save/load only act in gameplay; presses in menus or scenes are dropped.
    if (host.input_context == InputContext::Gameplay)
        host.quick_pressed |= host.input.pending.pressed & (action_bit(Action::quick_save) | action_bit(Action::quick_load));
    if (host.input_context == InputContext::Gameplay && player_run_toggle
            && (host.input.pending.pressed & action_bit(Action::run))) {
        host.run_toggled = !host.run_toggled;
        host_show_message(host.run_toggled ? "Running" : "Walking");
    }
    if (host.input_context == InputContext::Opening)
        host.opening_load_pressed |= (host.input.pending.pressed & action_bit(Action::quick_load)) != 0;
    host.input.pending.pressed = host.input.pending.released = 0;
    struct ButtonAction { Action action; Button button; };
    constexpr std::array<ButtonAction, 14> mapping = {
        ButtonAction{Action::forward, Button::Up}, ButtonAction{Action::backward, Button::Down},
        ButtonAction{Action::turn_left, Button::Left}, ButtonAction{Action::turn_right, Button::Right},
        ButtonAction{Action::strafe_left, Button::StrafeLeft}, ButtonAction{Action::strafe_right, Button::StrafeRight},
        ButtonAction{Action::look_up, Button::LookUp}, ButtonAction{Action::look_down, Button::LookDown},
        ButtonAction{Action::attack, Button::Attack}, ButtonAction{Action::magic, Button::Magic},
        ButtonAction{Action::interact, Button::Confirm}, ButtonAction{Action::confirm, Button::Confirm},
        ButtonAction{Action::back, Button::Back}, ButtonAction{Action::pause, Button::Select}
    };
    u32 buttons = 0;
    for (const auto &entry : mapping)
        if (actions & action_bit(entry.action))
            buttons |= button_mask(entry.button);
    // The inventory key opens the menu (Start) and, inside menus, backs out like Back.
    if (actions & action_bit(Action::inventory))
        buttons |= button_mask(host.input_context == InputContext::Menu ? Button::Back : Button::Start);
    if (actions & action_bit(Action::pause_or_back))
        buttons |= button_mask(host.input_context == InputContext::Menu ? Button::Back : Button::Select);
    return buttons | agent_buttons();
}

bool host_run_held() {
    // KF_TEST_RUN: hold run for automated tests.
    static const bool test_run = std::getenv("KF_TEST_RUN") != nullptr;
    if (player_run_toggle)
        return test_run || host.run_toggled;
    return test_run || (host.input.pending.held & action_bit(Action::run)) != 0;
}

QuickRequest host_take_quick_request() {
    // KF_TEST_QUICK=save|load@<update>: one scripted request for automated tests.
    static const char *test = std::getenv("KF_TEST_QUICK");
    static int test_updates;
    if (test && std::strchr(test, '@') && ++test_updates == std::atoi(std::strchr(test, '@') + 1))
        return test[0] == 's' ? QuickRequest::Save : QuickRequest::Load;
    // KF_TEST_LANGUAGE=ja|en@<update>: one scripted language change, as from F1.
    static const char *test_language = std::getenv("KF_TEST_LANGUAGE");
    static int language_updates;
    Language language;
    char code[3] = {};
    if (test_language && std::strchr(test_language, '@') &&
        ++language_updates == std::atoi(std::strchr(test_language, '@') + 1) &&
        std::sscanf(test_language, "%2[a-z]", code) == 1 && language_parse(code, &language))
        language_request(language);
    const u32 pressed = host.quick_pressed;
    host.quick_pressed = 0;
    if (pressed & action_bit(Action::quick_load))
        return QuickRequest::Load;
    return pressed & action_bit(Action::quick_save) ? QuickRequest::Save : QuickRequest::None;
}

bool host_opening_quick_load() {
    // KF_TEST_TITLE_LOAD: press quick load on the first opening poll.
    static bool test_load = std::getenv("KF_TEST_TITLE_LOAD") != nullptr;
    host.opening_load_pressed |= test_load;
    if (classic_mode)
        host.opening_load_pressed = false;
    test_load = false;
    if (!host.opening_load_pressed)
        return false;
    host.opening_load_pressed = false;
    static u8 data[save_file_capacity];
    std::size_t size = 0;
    if (save_file_read(SaveSlot::Quick, data, sizeof data, &size) != SaveFileResult::Ok) {
        host_show_message("No quick save to load");
        return false;
    }
    host.quick_pressed = action_bit(Action::quick_load);
    return true;
}

void host_wait_buttons_released(u32 mask) {
    while (host_read_buttons() & mask)
        host_wait_frame();
}

void host_wait_button_press() {
    while (!host_read_buttons())
        host_wait_frame();
}

InputContext host_set_input_context(InputContext context) {
    refresh_mouse_capture();
    const auto previous = host.input_context;
    // Nested Scripted -> Menu -> Scripted transitions retain the gameplay
    // capture request until the outer interaction returns to gameplay.
    if (previous == InputContext::Gameplay && context != InputContext::Gameplay)
        host.resume_mouse_capture = mouse_is_captured();
    if (context == InputContext::Opening)
        host.resume_mouse_capture = false;
    host.input_context = context;
    host.input.pending.look_x = host.input.pending.look_y = 0;
    host.look_remainder_x = host.look_remainder_y = 0;
    if ((context == InputContext::Opening || context == InputContext::Menu) && host.window)
        SDL_SetWindowRelativeMouseMode(host.window, false);
    if (context == InputContext::Gameplay && previous != InputContext::Gameplay) {
        if (host.resume_mouse_capture && host.focused && host.window && !mouse_is_captured()) {
            if (!SDL_SetWindowRelativeMouseMode(host.window, true))
                std::fprintf(stderr, "Cannot restore mouse capture: %s\n", SDL_GetError());
        }
        host.resume_mouse_capture = false;
    }
    refresh_mouse_capture();
    return previous;
}

InputContext host_input_context() { return host.input_context; }

void host_set_pad_layout(PadLayout layout) {
    host.pad_layout = layout;
    bind_gamepad_buttons(face_buttons(layout));
}

LookDelta host_take_look() {
    constexpr double angle_units_per_pixel = mouse_degrees_per_pixel * angle_units_per_turn / degrees_per_turn;
    // KF_TEST_SPIN=<pixels per update>: steady mouse turn for automated tests.
    static const double test_spin = std::getenv("KF_TEST_SPIN") ? std::atof(std::getenv("KF_TEST_SPIN")) : 0.0;
    host.input.pending.look_x += test_spin;
    // KF_TEST_TILT=<pixels per update>: steady mouse tilt (positive looks down).
    static const double test_tilt = std::getenv("KF_TEST_TILT") ? std::atof(std::getenv("KF_TEST_TILT")) : 0.0;
    host.input.pending.look_y += test_tilt;
    const double x = host.input.pending.look_x * angle_units_per_pixel + host.look_remainder_x;
    const double y = host.input.pending.look_y * angle_units_per_pixel + host.look_remainder_y;
    const s32 dx = static_cast<s32>(std::trunc(x));
    const s32 dy = static_cast<s32>(std::trunc(y));
    host.look_remainder_x = x - dx;
    host.look_remainder_y = y - dy;
    host.input.pending.look_x = host.input.pending.look_y = 0;
    host.look_consumed = {-dx, dy};
    return host.look_consumed;
}

std::uint64_t host_frames_presented() { return host.frames_presented; }

void host_show_message(const char *text) {
    std::printf("[quicksave] %s\n", text);
#if KF_EXT
    ext::show_message(text);
#endif
}

// Written while a frame is built and read when the overlay draws over it.
static MenuPreviewItem menu_preview_building, menu_preview_shown;

void host_menu_preview_item(u8 item_id, s32 top) { menu_preview_building = {item_id, top}; }

MenuPreviewItem host_menu_preview() { return menu_preview_shown; }

// Frame smoothing on the frontend: what moved since the update before.
static VertexCarry vertex_carry;

void host_begin_frame() {
    vertex_carry.begin_update();
    menu_preview_building = {};
    host.frame_faces.clear();
    host.frame_light_count = 0;
    render_entity_tag = 0;
}

CarriedModel host_carry_model(const MATRIX &model, std::span<const SVECTOR> vertices) {
    return vertex_carry.carry_model({render_entity_tag, render_entity_identity}, model, vertices);
}

std::span<const std::array<float, 2>> host_carry_screen(std::span<const std::array<float, 2>> points) {
    return vertex_carry.carry_screen({render_entity_tag, render_entity_identity}, points);
}

u16 host_carry_dial(float pivot_x, float pivot_y) {
    return vertex_carry.add_dial(pivot_x, pivot_y);
}

void host_enqueue_face(const DrawFace &face) {
    if (host.frame_faces.size() == host.frame_faces.max_size())
        host_fail("Render command allocation is too large.");
    // Animated instances reuse the same projection/morph scratch storage. No
    // pointer into that storage survives this submission.
    try {
        host.frame_faces.push_back(face);
        host.frame_faces.back().entity = render_entity_tag;
    } catch (const std::bad_alloc &) {
        host_fail("Cannot allocate render commands.");
    }
}

void host_present_frame(const FrameStyle &style) {
    const FaceList faces{host.frame_faces.data(), host.frame_faces.size(), style};
    host_present_faces(&faces);
}

void host_add_light(float x, float y, float z, float radius, float r, float g, float b, float flicker, u32 seed) {
    if (host.frame_light_count < render_light_capacity)
        host.frame_lights[host.frame_light_count++] = {x, y, z, radius, r, g, b, flicker, seed};
}

// ---- Frame smoothing (PC extension) ----
// The game updates and renders at its tick rate (20 a second as shipped). The
// frontend redraws the last update from a camera between the last two, as often
// as the display allows, and does its own near clipping and light flicker
// (port/renderer/frontend.cpp); the host hands it each finished update and asks
// for frames between. Game logic and timing are untouched.
namespace {
// The camera of the world frame being built; none for a 2D frame.
std::optional<CameraPose> frame_camera;
struct DisplayRefresh {
    double hz = 0;
    Uint64 checked_ns = 0;
};
DisplayRefresh display_refresh;

int angle_delta(int from, int to) {
    constexpr int half_turn = 0x800, wrap_mask = 0xfff;   // 4096 angle units per turn
    return ((to - from + half_turn) & wrap_mask) - half_turn;
}

Uint64 smooth_frame_period_ns() {
    const int limit = render_frame_limit;
    if (limit == 0) {
        const Uint64 now = SDL_GetTicksNS();
        if (!display_refresh.hz || now - display_refresh.checked_ns > ns_per_second) {
            const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(host.window));
            display_refresh.hz = mode && mode->refresh_rate > 0 ? mode->refresh_rate : 60.0;
            display_refresh.checked_ns = now;
        }
        return static_cast<Uint64>(ns_per_second / display_refresh.hz);
    }
    return limit < 0 ? 0 : ns_per_second / static_cast<Uint64>(limit);
}
} // namespace

void host_frame_camera(const VECTOR &position, const SVECTOR &rotation, const Projection &projection,
                       const LightColor &fog_color) {
    frame_camera = CameraPose{position, rotation, projection, fog_color};
    vertex_carry.seen_from({position, rotation, projection});
}

void host_light_occluders(const u8 *solid, int columns, int rows, float tile_size) {
    const auto cells = static_cast<std::size_t>(std::max(columns, 0)) * static_cast<std::size_t>(std::max(rows, 0));
    renderer_set_occluders({solid, cells}, columns, rows, tile_size);
}

static void frontend_submit(const FaceList *faces);

void host_present_faces(const FaceList *faces) {
    menu_preview_shown = menu_preview_building;
    ++host.frames_presented;
#if KF_EXT
    ext::game_frame(static_cast<int>(host.input_context));
#endif
    frontend_submit(faces);
    frame_camera.reset();
}

// ---- The renderer frontend ----
namespace {
struct FrontendOutput {
    PictureOptions requested;          // as the settings asked
    bool smoothing_applied = false;    // requested, and the input context allows it
    std::optional<CameraPose> last_camera;
    bool world_frame = false;          // the last update had a camera
    Uint64 next_present_ns = 0;
    std::vector<GameLight> lights;
};
FrontendOutput frontend_output;

// Smoothing only where the camera is the player's: not in menus or the map,
// and not headless, where nothing is shown.
bool smoothing_allowed() {
    return !options.headless &&
           (host.input_context == InputContext::Gameplay || host.input_context == InputContext::Scripted);
}

void apply_frontend_options() {
    PictureOptions picture = frontend_output.requested;
    picture.smoothing = picture.smoothing && smoothing_allowed();
    frontend_output.smoothing_applied = picture.smoothing;
    renderer_set_options(picture);
}

// The look input not yet in a drawn update, in angle units, held inside the
// pitch limit the game will clamp it to: what the game has not taken, plus what
// the update in progress took (it waits for the next tick before submitting).
SVECTOR pending_look() {
    if (host.input_context != InputContext::Gameplay)
        return {0, 0, 0};
    constexpr double angle_units_per_pixel = mouse_degrees_per_pixel * angle_units_per_turn / degrees_per_turn;
    const double x = host.input.pending.look_x * angle_units_per_pixel + host.look_remainder_x;
    const double y = host.input.pending.look_y * angle_units_per_pixel + host.look_remainder_y;
    s32 pitch = static_cast<s32>(std::trunc(y)) + host.look_consumed.pitch;
    const s32 yaw = -static_cast<s32>(std::trunc(x)) + host.look_consumed.yaw;
    if (frontend_output.last_camera) {
        const s32 at = frontend_output.last_camera->rotation.vx;
        pitch = std::clamp(at + pitch, -player_pitch_limit, player_pitch_limit) - at;
    }
    return {static_cast<s16>(pitch), static_cast<s16>(yaw), 0};
}

// The look this update applied: what the game took, but pitch no further than
// the camera actually tilted (the game clamps it at the limit).
SVECTOR look_applied(const std::optional<CameraPose> &camera) {
    if (!camera || !frontend_output.last_camera)
        return {0, 0, 0};
    const int tilted = angle_delta(frontend_output.last_camera->rotation.vx, camera->rotation.vx);
    const s32 pitch = std::clamp(host.look_consumed.pitch, std::min(0, tilted), std::max(0, tilted));
    return {static_cast<s16>(pitch), static_cast<s16>(host.look_consumed.yaw), 0};
}

#if KF_EXT
// KF_CAPTURE=<file.bmp>: the game's picture after KF_CAPTURE_FRAME frames
// (default 100), at the target's size, top row first, then exit. For automated
// visual checks without focus.
void capture_picture_if_requested() {
    static const char *path = std::getenv("KF_CAPTURE");
    static const long target = [] {
        const char *frame = std::getenv("KF_CAPTURE_FRAME");
        return frame ? std::atol(frame) : 100L;
    }();
    static long frames;
    if (!path || ++frames < target)
        return;
    const std::optional<Image> picture = renderer_capture();
    if (!picture)
        host_fail("Cannot capture the picture.");
    const std::uint32_t row = picture->width * 3u + 3u & ~3u;
    const std::uint32_t bytes = row * picture->height;
    std::array<unsigned char, 54> header{'B', 'M'};
    const auto put32 = [&](std::size_t at, std::uint32_t value) {
        for (std::size_t i = 0; i < 4; ++i)
            header[at + i] = static_cast<unsigned char>(value >> (8 * i));
    };
    put32(2, 54 + bytes);
    put32(10, 54);
    put32(14, 40);
    put32(18, picture->width);
    put32(22, static_cast<std::uint32_t>(-static_cast<std::int32_t>(picture->height)));   // negative: top row first
    header[26] = 1;
    header[28] = 24;
    put32(34, bytes);
    if (FILE *file = std::fopen(path, "wb")) {
        std::fwrite(header.data(), 1, header.size(), file);
        std::vector<unsigned char> line(row);
        for (std::uint32_t y = 0; y < picture->height; ++y) {
            for (std::size_t x = 0; x < picture->width; ++x) {
                const unsigned char *pixel = &picture->rgba[(static_cast<std::size_t>(y) * picture->width + x) * 4];
                line[x * 3] = pixel[2];
                line[x * 3 + 1] = pixel[1];
                line[x * 3 + 2] = pixel[0];
            }
            std::fwrite(line.data(), 1, row, file);
        }
        std::fclose(file);
        std::printf("[capture] saved %s (%ux%u)\n", path, picture->width, picture->height);
    }
    host_shutdown();
    std::exit(0);
}
#endif
} // namespace

static bool frontend_wants_frames() {
    return frontend_output.world_frame && frontend_output.smoothing_applied && !host.paused;
}

static Uint64 frontend_next_present_ns() { return frontend_output.next_present_ns; }

static void frontend_present(Uint64 now) {
    int width = 0, height = 0;
    SDL_GetWindowSizeInPixels(host.window, &width, &height);
    ImDrawData *overlay = nullptr;
#if KF_EXT
    overlay = ext::build_overlay(width, height);
#endif
    renderer_present(now, pending_look(), width, height, overlay);
#if KF_EXT
    capture_picture_if_requested();
#endif
    frontend_output.next_present_ns = now + smooth_frame_period_ns();
}

static void frontend_submit(const FaceList *faces) {
    if (frontend_output.smoothing_applied != (frontend_output.requested.smoothing && smoothing_allowed()))
        apply_frontend_options();
    const std::optional<CameraPose> &camera = frame_camera;
    auto &lights = frontend_output.lights;
    lights.clear();
    for (int i = 0; i < host.frame_light_count; ++i) {
        const auto &light = host.frame_lights[i];
        lights.push_back({light.x, light.y, light.z, light.radius, light.r, light.g, light.b, light.flicker, light.seed});
    }
    const std::span<const PartMove> parts = camera ? vertex_carry.parts() : std::span<const PartMove>{};
    GameFrame frame{{faces->faces, faces->count}, faces->style, camera, lights, parts, look_applied(camera)};
    const Uint64 now = host_clock_ns();
    renderer_submit(frame, now);
    frontend_output.world_frame = camera.has_value();
    if (camera)
        frontend_output.last_camera = camera;
    host.look_consumed = {0, 0};
    frontend_present(now);
}

TextureStore &host_textures() { return renderer_textures(); }

int host_extra_columns() { return renderer_extra_columns(); }

bool host_capture_frame(Image &image) {
    std::optional<Image> picture = renderer_capture();
    if (!picture)
        return false;
    image = std::move(*picture);
    return true;
}

bool host_restore_frame(const Image &image) {
    return renderer_restore(image);
}

bool host_set_render_target(int logical_width, int scale) {
    return renderer_set_target(logical_width, scale);
}

void host_set_picture_options(const PictureOptions &options) {
    frontend_output.requested = options;
    apply_frontend_options();
}
}
