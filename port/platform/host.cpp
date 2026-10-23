#include <kf/platform/host.h>
#include <kf/platform/language_runtime.h>
#include <kf/platform/saves.h>
#include <kf/audio/sound.h>
#include <kf/platform/input.h>
#include <kf/platform/controls.h>
#include <kf/renderer/renderer.h>
#include <kf/renderer/projection.h>
#include <kf/lib/fixed_math.h>
#include <SDL3/SDL.h>
#if KF_EXT
#include <kf/ext/ext.h>
#include <GLES3/gl3.h>
#endif
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
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
    SDL_GLContext context;
    SDL_Gamepad *gamepad;
    Renderer renderer;
    std::vector<DrawFace> frame_faces;
    InputState input;
    InputContext input_context;
    std::array<SDL_Keycode, SDL_SCANCODE_COUNT> pressed_keys;
    Uint64 epoch, paused_ns, pause_start;
    double look_remainder_x, look_remainder_y;
    bool focused, mouse_captured, resume_mouse_capture;
    bool paused, overlay_was_open, cursor_released, recapture_mouse;
    Projection clip_projection;
    struct EntityPose { u16 tag; float x, y, z; s32 yaw; s8 yaw_sign; };
    std::vector<EntityPose> frame_poses;
    std::vector<DrawFace> clipped_faces;
    u32 quick_pressed;
    bool opening_load_pressed;
    bool run_toggled;
    struct FrameLight {
        float x, y, z, radius, r, g, b, flicker;
        u32 seed;
    } frame_lights[render_light_capacity];
    int frame_light_count;
};
static HostState host;
static constexpr Uint64 ns_per_second = 1000000000;
static constexpr Uint64 ns_per_millisecond = 1000000;
static constexpr Uint64 clock_ticks_per_second = 60;
static constexpr Uint64 unfocused_poll_interval_ns = 16 * ns_per_millisecond;
static constexpr Uint64 maximum_wait_slice_ns = 8 * ns_per_millisecond;
static constexpr u32 gamepad_axis_control_first = 100;
static constexpr u32 gamepad_axis_positive_first = gamepad_axis_control_first + 1;
static constexpr int gamepad_axis_deadzone = 16000;
static constexpr std::size_t renderer_error_capacity = 2048;
static constexpr double mouse_degrees_per_pixel = 0.12;
static constexpr double angle_units_per_turn = 4096;
static constexpr double degrees_per_turn = 360;

#if KF_EXT
static void capture_frame_if_requested(int width, int height);
#endif

static void present_retained_frame() {
    int width = 0, height = 0;
    SDL_GetWindowSizeInPixels(host.window, &width, &height);
    if (width > 0 && height > 0) {
        renderer_present_retained(&host.renderer, width, height);
#if KF_EXT
        ext::draw(width, height);
        capture_frame_if_requested(width, height);
#endif
        SDL_GL_SwapWindow(host.window);
    }
}

static void platform_yield(Uint64 nanoseconds) {
#ifdef __EMSCRIPTEN__
    emscripten_sleep(static_cast<unsigned>((nanoseconds + ns_per_millisecond - 1) / ns_per_millisecond));
#else
    SDL_DelayNS(nanoseconds);
#endif
}

static void bind_inputs() {
    auto *input = &host.input;
    struct KeyBinding { SDL_Keycode code; Action action; };
    constexpr std::array<KeyBinding, 22> keys = {
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
        KeyBinding{SDLK_F2, Action::quick_save}, KeyBinding{SDLK_F4, Action::quick_load}
    };
    for (const auto &key : keys)
        input_bind(input, {InputDevice::keyboard, static_cast<u32>(key.code)}, key.action);
    input_bind(input, {InputDevice::mouse, SDL_BUTTON_LEFT}, Action::attack);
    input_bind(input, {InputDevice::mouse, SDL_BUTTON_RIGHT}, Action::magic);
    struct ButtonBinding { SDL_GamepadButton code; Action action; };
    constexpr std::array<ButtonBinding, 13> buttons = {
        ButtonBinding{SDL_GAMEPAD_BUTTON_DPAD_UP, Action::forward}, ButtonBinding{SDL_GAMEPAD_BUTTON_DPAD_DOWN, Action::backward},
        ButtonBinding{SDL_GAMEPAD_BUTTON_DPAD_LEFT, Action::turn_left}, ButtonBinding{SDL_GAMEPAD_BUTTON_DPAD_RIGHT, Action::turn_right},
        ButtonBinding{SDL_GAMEPAD_BUTTON_NORTH, Action::attack}, ButtonBinding{SDL_GAMEPAD_BUTTON_WEST, Action::magic},
        ButtonBinding{SDL_GAMEPAD_BUTTON_EAST, Action::interact}, ButtonBinding{SDL_GAMEPAD_BUTTON_SOUTH, Action::back},
        ButtonBinding{SDL_GAMEPAD_BUTTON_START, Action::inventory}, ButtonBinding{SDL_GAMEPAD_BUTTON_BACK, Action::pause},
        ButtonBinding{SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, Action::strafe_left},
        ButtonBinding{SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, Action::strafe_right},
        ButtonBinding{SDL_GAMEPAD_BUTTON_LEFT_STICK, Action::run}
    };
    for (const auto &button : buttons)
        input_bind(input, {InputDevice::gamepad, static_cast<u32>(button.code)}, button.action);
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

bool host_start() {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "%s\n", SDL_GetError());
        return false;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    host.window = SDL_CreateWindow("King's Field", 960, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (host.window)
        host.context = SDL_GL_CreateContext(host.window);
    if (!host.window || !host.context) {
        std::fprintf(stderr, "%s\n", SDL_GetError());
        host_shutdown();
        return false;
    }
    // All frame pacing uses the absolute clock below, not a second swap-interval wait.
    SDL_GL_SetSwapInterval(0);
    std::array<char, renderer_error_capacity> error{};
    if (!renderer_init(&host.renderer, error.data(), error.size())) {
        std::fprintf(stderr, "%s\n", error.data());
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
    ext::start(host.window, host.context);
#endif
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
    if (host.context) {
        renderer_release(&host.renderer);
        SDL_GL_DestroyContext(host.context);
    }
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
    if (host.window) {
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
        // Use the layout/remap, without Shift/Caps changing letter bindings.
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
            present_retained_frame();
        break;
    default: break;
    }
}

static bool smooth_redraw_paused();

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
// KF_CAPTURE=<file.bmp>: after KF_CAPTURE_FRAME game frames (default 100), save
// the finished window image and exit. For automated visual checks without focus.
static void capture_frame_if_requested(int width, int height) {
    static const char *path = std::getenv("KF_CAPTURE");
    static const long target = std::getenv("KF_CAPTURE_FRAME") ? std::atol(std::getenv("KF_CAPTURE_FRAME")) : 100;
    static long frames;
    if (!path || ++frames < target)
        return;
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    if (FILE *file = std::fopen(path, "wb")) {
        const std::uint32_t row = static_cast<std::uint32_t>(width) * 3u + 3u & ~3u;
        const std::uint32_t image = row * static_cast<std::uint32_t>(height);
        unsigned char header[54] {'B', 'M'};
        const auto put32 = [&](int at, std::uint32_t value) {
            for (int i = 0; i < 4; ++i)
                header[at + i] = static_cast<unsigned char>(value >> (8 * i));
        };
        put32(2, 54 + image);
        put32(10, 54);
        put32(14, 40);
        put32(18, static_cast<std::uint32_t>(width));
        put32(22, static_cast<std::uint32_t>(height));   // bottom-up, like glReadPixels
        header[26] = 1;
        header[28] = 24;
        put32(34, image);
        std::fwrite(header, 1, sizeof header, file);
        std::vector<unsigned char> line(row);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const unsigned char *pixel = &pixels[(static_cast<std::size_t>(y) * width + x) * 4];
                line[x * 3] = pixel[2];
                line[x * 3 + 1] = pixel[1];
                line[x * 3 + 2] = pixel[0];
            }
            std::fwrite(line.data(), 1, row, file);
        }
        std::fclose(file);
        std::printf("[capture] saved %s (%dx%d)\n", path, width, height);
    }
    host_shutdown();
    std::exit(0);
}

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
        if (ext::redraw_while_paused() && host.focused && !smooth_redraw_paused())
            present_retained_frame();
#endif
        platform_yield(unfocused_poll_interval_ns);
    }
}

std::uint64_t host_clock_ns() {
    const auto now = host.paused ? host.pause_start : SDL_GetTicksNS();
    return now - host.epoch - host.paused_ns;
}

std::uint64_t host_clock_tick() {
    const auto elapsed = host_clock_ns();
    return elapsed / ns_per_second * clock_ticks_per_second + elapsed % ns_per_second * clock_ticks_per_second / ns_per_second;
}

static bool smooth_frames_wanted();
static Uint64 smooth_next_present_ns();
static void smooth_present_now(Uint64 now);

static void wait_until_ns(std::uint64_t target) {
    for (;;) {
        host_poll();
        const auto now = host_clock_ns();
        if (now >= target)
            return;
        Uint64 slice = std::min<Uint64>(target - now, maximum_wait_slice_ns);
        if (smooth_frames_wanted()) {
            if (now >= smooth_next_present_ns()) {
                smooth_present_now(now);
                continue;
            }
            slice = std::min<Uint64>(slice, smooth_next_present_ns() - now);
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
    return buttons;
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
    return {-dx, dy};
}

Renderer *host_renderer() { return &host.renderer; }

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

void host_begin_frame() {
    menu_preview_building = {};
    host.frame_faces.clear();
    host.frame_light_count = 0;
    host.frame_poses.clear();
    render_entity_tag = 0;
}

void host_report_entity(u16 tag, s32 x, s32 y, s32 z, s32 yaw, int yaw_sign) {
    host.frame_poses.push_back({tag, static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), yaw,
                                static_cast<s8>(yaw_sign)});
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

// Turns the frame's lights into renderer lights: strength, flicker (animated on
// every presented frame) and an optional camera change for smoothed frames.
static void apply_lights(const HostState::FrameLight *lights, int count, const float (*rotate)[3] = nullptr,
    const float *translate = nullptr) {
    RenderLight out[render_light_capacity];
    const double seconds = SDL_GetTicksNS() / 1e9;
    for (int i = 0; i < count; ++i) {
        const auto &light = lights[i];
        float position[3] = {light.x, light.y, light.z};
        if (rotate) {
            float world[3];
            for (int r = 0; r < 3; ++r)
                world[r] = rotate[r][0] * light.x + rotate[r][1] * light.y + rotate[r][2] * light.z + translate[r];
            std::copy_n(world, 3, position);
        }
        // Two incommensurate sines per light read as an irregular flame flicker.
        const double phase = (light.seed % 997) * 0.37;
        const double wave = 0.5 * std::sin(seconds * 9.1 + phase) + 0.3 * std::sin(seconds * 23.7 + phase * 1.7)
            + 0.2 * std::sin(seconds * 3.3 + phase * 0.3);
        const float intensity = render_light_strength * static_cast<float>(1.0 + light.flicker * 0.18 * wave);
        // Game lights (torches, spells) ignore their immediate surroundings as casters.
        out[i] = {position[0], position[1], position[2], light.radius,
                  light.r * intensity, light.g * intensity, light.b * intensity, 1200.0f};
    }
    // Nearest lights first: the renderer gives shadow maps to the leading ones.
    std::sort(out, out + count, [](const RenderLight &a, const RenderLight &b) {
        return a.x * a.x + a.y * a.y + a.z * a.z < b.x * b.x + b.y * b.y + b.z * b.z;
    });
    renderer_set_lights(&host.renderer, out, count);
}

// Faces that reach behind the camera's near plane are cut at it in camera space
// and their new corners projected, instead of being dropped (as the PlayStation
// did) or drawn from saturated coordinates.
static bool face_crosses_near(const DrawFace &face) {
    const unsigned count = static_cast<unsigned>(face.shape);
    if (count != 3 && count != 4)
        return false;
    bool crossing = false;
    for (unsigned v = 0; v < count; ++v) {
        if (face.vertices[v].cz == 0)
            return false;   // camera-fixed (menus, weapon): not clippable
        const Vertex &vertex = face.vertices[v];
        // Also corners projected past the saturated screen range: re-projecting
        // them from camera space without the clamp keeps the face's shape.
        crossing = crossing || vertex.cz < render_near_depth || vertex.x <= -1024.0f || vertex.x >= 1023.0f
            || vertex.y <= -1024.0f || vertex.y >= 1023.0f;
    }
    return crossing;
}

static void clip_face_near(const DrawFace &face, std::vector<DrawFace> &out) {
    const Projection &projection = host.clip_projection;
    const float near_depth = render_near_depth;
    // Outline order: quads are drawn as (0,1,2) and (1,3,2).
    const unsigned outline_quad[] = {0, 1, 3, 2}, outline_triangle[] = {0, 1, 2};
    const unsigned count = static_cast<unsigned>(face.shape);
    const unsigned *outline = count == 4 ? outline_quad : outline_triangle;
    Vertex polygon[8];
    unsigned size = 0;
    const auto lerp = [](const Vertex &a, const Vertex &b, float t) {
        Vertex v = a;
        const auto mix = [t](float x, float y) { return x + (y - x) * t; };
        v.u = mix(a.u, b.u); v.v = mix(a.v, b.v);
        v.r = mix(a.r, b.r); v.g = mix(a.g, b.g); v.b = mix(a.b, b.b); v.a = mix(a.a, b.a);
        v.cx = mix(a.cx, b.cx); v.cy = mix(a.cy, b.cy); v.cz = mix(a.cz, b.cz);
        return v;
    };
    for (unsigned i = 0; i < count; ++i) {
        const Vertex &a = face.vertices[outline[i]], &b = face.vertices[outline[(i + 1) % count]];
        const bool a_in = a.cz >= near_depth, b_in = b.cz >= near_depth;
        if (a_in)
            polygon[size++] = a;
        if (a_in != b_in)
            polygon[size++] = lerp(a, b, (near_depth - a.cz) / (b.cz - a.cz));
    }
    if (size < 3)
        return;
    const bool perspective = face.vertices[0].w > 0;
    for (unsigned i = 0; i < size; ++i) {
        Vertex &v = polygon[i];
        const float scale = projection.distance / v.cz;
        v.x = v.cx * scale + static_cast<float>(projection.center_x);
        v.y = v.cy * scale + static_cast<float>(projection.center_y);
        if (!render_subpixel) {
            v.x = std::floor(v.x);
            v.y = std::floor(v.y);
        }
        v.w = perspective ? v.cz : 0.0f;
    }
    // Fan into triangles; the pieces stay adjacent and share the face's depth.
    for (unsigned i = 1; i + 1 < size; ++i) {
        DrawFace piece = face;
        piece.shape = FaceShape::Triangle;
        piece.vertices[0] = polygon[0];
        piece.vertices[1] = polygon[i];
        piece.vertices[2] = polygon[i + 1];
        out.push_back(piece);
    }
}

static void draw_and_swap(const FaceList *faces) {
    int width = 0, height = 0;
    SDL_GetWindowSizeInPixels(host.window, &width, &height);
    // A minimized window can have no drawable pixels. Still update the logical
    // target, so a one-shot modal is there when the window is restored.
#if KF_EXT
    // KF_PERF: log presented frames per second and the GPU time of the draw.
    static const bool perf = std::getenv("KF_PERF") != nullptr;
    static Uint64 perf_window_start, perf_draw_ns;
    static unsigned perf_frames;
    const Uint64 draw_start = perf ? SDL_GetTicksNS() : 0;
#endif
    FaceList clipped = *faces;
    bool any_crossing = false;
    for (std::size_t i = 0; i < faces->count && !any_crossing; ++i)
        any_crossing = face_crosses_near(faces->faces[i]);
    if (any_crossing) {
        host.clipped_faces.clear();
        for (std::size_t i = 0; i < faces->count; ++i) {
            if (face_crosses_near(faces->faces[i]))
                clip_face_near(faces->faces[i], host.clipped_faces);
            else
                host.clipped_faces.push_back(faces->faces[i]);
        }
        clipped.faces = host.clipped_faces.data();
        clipped.count = host.clipped_faces.size();
    }
    if (!renderer_draw_faces(&host.renderer, &clipped, width, height))
        host_fail("Cannot submit game render commands.");
#if KF_EXT
    if (perf) {
        for (GLenum gl_error; (gl_error = glGetError()) != GL_NO_ERROR;)
            std::printf("[perf] GL error 0x%04x after draw\n", gl_error);
        glFinish();
        const Uint64 now = SDL_GetTicksNS();
        perf_draw_ns += now - draw_start;
        ++perf_frames;
        if (!perf_window_start)
            perf_window_start = now;
        if (now - perf_window_start >= 2 * ns_per_second) {
            std::printf("[perf] %.1f fps, draw %.2f ms/frame, %zu faces, %d lights, target %dx%d\n",
                perf_frames * 1e9 / static_cast<double>(now - perf_window_start),
                perf_draw_ns / 1e6 / perf_frames, faces->count, host.renderer.light_count,
                host.renderer.logical_width * host.renderer.scale, render_height * host.renderer.scale);
            for (int l = 0; l < host.renderer.light_count; ++l) {
                const auto &light = host.renderer.lights[l];
                std::printf("[perf]   light %d at %.0f,%.0f,%.0f radius %.0f colour %.2f,%.2f,%.2f\n", l, light.x, light.y,
                    light.z, light.radius, light.r, light.g, light.b);
            }
            perf_window_start = now;
            perf_draw_ns = 0;
            perf_frames = 0;
        }
    }
#endif
    if (width > 0 && height > 0) {
#if KF_EXT
        ext::draw(width, height);
        capture_frame_if_requested(width, height);
#endif
        SDL_GL_SwapWindow(host.window);
    }
}

// ---- Frame smoothing (PC extension) ----
// The game updates and renders 20 times a second. Each world frame keeps its
// faces with camera-space positions; between updates the host re-projects them
// from a camera interpolated between the last two updates, as often as the
// display allows. Game logic and timing are untouched; the picture trails the
// game by one update. Camera-attached models (cz = 0) are not moved.
namespace {
struct SmoothCamera {
    VECTOR position;
    SVECTOR rotation;
    Projection projection;
};
struct FrameSmoothing {
    SmoothCamera frame{};            // camera of the world frame being built
    bool frame_has_camera = false;
    SmoothCamera previous{}, current{};
    bool have_current = false;
    Uint64 current_ns = 0, interval_ns = 0, next_present_ns = 0;
    DrawFace *faces = nullptr, *scratch = nullptr;
    std::size_t count = 0, capacity = 0;
    FrameStyle style{};
    HostState::FrameLight lights[render_light_capacity];
    int light_count = 0;
    bool active = false;             // the picture on screen is a smoothable world frame
    // Per entity tag: the current update's pose and the motion back to the
    // previous one (so the shown pose is current + (1 - alpha) * motion).
    struct EntityMotion { bool valid; float x, y, z, back_x, back_y, back_z, back_yaw; s8 yaw_sign; };
    std::vector<EntityMotion> motion;
    std::vector<HostState::EntityPose> poses;
    double refresh_hz = 0;
    Uint64 refresh_checked_ns = 0;
};
FrameSmoothing smooth;
constexpr Uint64 smooth_interval_min_ns = 16 * ns_per_millisecond;
constexpr Uint64 smooth_interval_max_ns = 100 * ns_per_millisecond;
constexpr Uint64 smooth_default_interval_ns = 50 * ns_per_millisecond;
constexpr s32 smooth_snap_distance = 4000;          // two map tiles in one update: a warp
constexpr int smooth_snap_angle = 1024;             // a quarter turn in one update
constexpr float smooth_near_depth = 16.0f;
constexpr float fixed_one = 4096.0f;

int angle_delta(int from, int to) {
    constexpr int half_turn = 0x800, wrap_mask = 0xfff;   // 4096 angle units per turn
    return ((to - from + half_turn) & wrap_mask) - half_turn;
}

void rotation_matrix(const SVECTOR &angles, float out[3][3]) {
    MATRIX fixed{};
    matrix_set_rotation_xyz(angles, fixed);
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            out[row][col] = fixed.m[row][col] / fixed_one;
}

// The renderer's camera for light occlusion: camera-to-world is the transpose
// of the view rotation.
void set_renderer_camera(const SVECTOR &rotation, float x, float y, float z) {
    float view[3][3];
    rotation_matrix(rotation, view);
    const float camera_to_world[9] = {view[0][0], view[1][0], view[2][0], view[0][1], view[1][1], view[2][1],
                                      view[0][2], view[1][2], view[2][2]};
    const float position[3] = {x, y, z};
    renderer_set_camera(&host.renderer, camera_to_world, position);
}

bool smoothing_applies() {
    return render_smoothing && (host.input_context == InputContext::Gameplay || host.input_context == InputContext::Scripted);
}

Uint64 smooth_frame_period_ns() {
    int limit = render_frame_limit;
    if (limit == 0) {
        const Uint64 now = SDL_GetTicksNS();
        if (!smooth.refresh_hz || now - smooth.refresh_checked_ns > ns_per_second) {
            const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(host.window));
            smooth.refresh_hz = mode && mode->refresh_rate > 0 ? mode->refresh_rate : 60.0;
            smooth.refresh_checked_ns = now;
        }
        return static_cast<Uint64>(ns_per_second / smooth.refresh_hz);
    }
    return limit < 0 ? 0 : ns_per_second / static_cast<Uint64>(limit);
}

void smooth_present(Uint64 now) {
    const double alpha = std::clamp(static_cast<double>(now - smooth.current_ns) / smooth.interval_ns, 0.0, 1.0);
    const auto &a = smooth.previous, &b = smooth.current;
    const auto lerp = [alpha](s32 from, s32 to) { return static_cast<float>(from + (to - from) * alpha); };
    const float shown_x = lerp(a.position.vx, b.position.vx);
    const float shown_y = lerp(a.position.vy, b.position.vy);
    const float shown_z = lerp(a.position.vz, b.position.vz);
    const SVECTOR shown_rotation = VECTOR{
        a.rotation.vx + static_cast<s32>(angle_delta(a.rotation.vx, b.rotation.vx) * alpha),
        a.rotation.vy + static_cast<s32>(angle_delta(a.rotation.vy, b.rotation.vy) * alpha),
        a.rotation.vz + static_cast<s32>(angle_delta(a.rotation.vz, b.rotation.vz) * alpha)}.narrowed();
    float view_now[3][3], view_shown[3][3];
    rotation_matrix(b.rotation, view_now);
    rotation_matrix(shown_rotation, view_shown);
    // Camera offset between the frame's camera and the shown one, in world units.
    const float offset[3] = {b.position.vx - shown_x, b.position.vy - shown_y, b.position.vz - shown_z};
    const float distance = b.projection.distance;
    std::size_t kept = 0;
    const float back = static_cast<float>(1.0 - alpha);
    for (std::size_t i = 0; i < smooth.count; ++i) {
        DrawFace face = smooth.faces[i];
        const FrameSmoothing::EntityMotion *moving = face.entity && face.entity < smooth.motion.size()
            && smooth.motion[face.entity].valid ? &smooth.motion[face.entity] : nullptr;
        float turn_cos = 1, turn_sin = 0;
        if (moving && moving->yaw_sign) {
            // The model applies R_y(sign * yaw); turning it further by t is R_y(sign * t).
            const float turn = moving->back_yaw * back * moving->yaw_sign;
            turn_cos = std::cos(turn);
            turn_sin = std::sin(turn);
        }
        bool visible = true;
        for (unsigned v = 0; v < static_cast<unsigned>(face.shape) && visible; ++v) {
            Vertex &vertex = face.vertices[v];
            if (vertex.cz == 0)
                continue;
            // camera' = V_shown * (V_now^T * camera + P_now - P_shown)
            float world[3];
            for (int r = 0; r < 3; ++r)
                world[r] = view_now[0][r] * vertex.cx + view_now[1][r] * vertex.cy + view_now[2][r] * vertex.cz + offset[r];
            if (moving) {
                // world[] is relative to the shown camera; so is the entity's origin.
                const float origin[3] = {moving->x - shown_x, moving->y - shown_y, moving->z - shown_z};
                const float rx = world[0] - origin[0], rz = world[2] - origin[2];
                // R_y(t) = [[cos, 0, -sin], [0, 1, 0], [sin, 0, cos]] (matrix_set_rotation_y).
                world[0] = origin[0] + turn_cos * rx - turn_sin * rz + moving->back_x * back;
                world[1] += moving->back_y * back;
                world[2] = origin[2] + turn_sin * rx + turn_cos * rz + moving->back_z * back;
            }
            float camera[3];
            for (int r = 0; r < 3; ++r)
                camera[r] = view_shown[r][0] * world[0] + view_shown[r][1] * world[1] + view_shown[r][2] * world[2];
            // Corners in front of the near plane are projected here; faces that
            // cross it are clipped (and re-projected) before drawing.
            const float scale = distance / std::max(camera[2], smooth_near_depth);
            float x = std::clamp(camera[0] * scale + b.projection.center_x, -1024.0f, 1023.0f);
            float y = std::clamp(camera[1] * scale + b.projection.center_y, -1024.0f, 1023.0f);
            if (!render_subpixel) {
                x = std::floor(x);
                y = std::floor(y);
            }
            vertex.x = x;
            vertex.y = y;
            if (vertex.w > 0)
                vertex.w = camera[2];
            vertex.cx = camera[0];
            vertex.cy = camera[1];
            vertex.cz = camera[2];
        }
        if (visible)
            smooth.scratch[kept++] = face;
    }
    // Lights move with the same camera change: combined = V_shown * V_now^T, then offset.
    float combined[3][3], shift[3];
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c)
            combined[r][c] = view_shown[r][0] * view_now[c][0] + view_shown[r][1] * view_now[c][1] + view_shown[r][2] * view_now[c][2];
        shift[r] = view_shown[r][0] * offset[0] + view_shown[r][1] * offset[1] + view_shown[r][2] * offset[2];
    }
    apply_lights(smooth.lights, smooth.light_count, combined, shift);
    set_renderer_camera(shown_rotation, shown_x, shown_y, shown_z);
    const FaceList list{smooth.scratch, kept, smooth.style};
    draw_and_swap(&list);
    const Uint64 period = smooth_frame_period_ns();
    smooth.next_present_ns = now + period;
}

// Takes ownership of a finished world frame; returns false if it is not one.
bool smooth_take_frame(const FaceList *faces) {
    if (!smoothing_applies() || !smooth.frame_has_camera || faces->faces != host.frame_faces.data()) {
        smooth.active = false;
        return false;
    }
    if (faces->count > smooth.capacity) {
        auto *grown = static_cast<DrawFace *>(std::realloc(smooth.faces, faces->count * sizeof(DrawFace)));
        auto *scratch = grown ? static_cast<DrawFace *>(std::realloc(smooth.scratch, faces->count * sizeof(DrawFace))) : nullptr;
        if (grown)
            smooth.faces = grown;
        if (!grown || !scratch) {
            smooth.active = false;
            return false;
        }
        smooth.scratch = scratch;
        smooth.capacity = faces->count;
    }
    std::copy_n(faces->faces, faces->count, smooth.faces);
    smooth.count = faces->count;
    smooth.light_count = host.frame_light_count;
    std::copy_n(host.frame_lights, host.frame_light_count, smooth.lights);
    smooth.style = faces->style;
    const Uint64 now = host_clock_ns();
    const SmoothCamera &next = smooth.frame;
    bool jump = !smooth.have_current || !smooth.active || now - smooth.current_ns > smooth_interval_max_ns;
    if (!jump) {
        const auto &last = smooth.current.position;
        jump = std::abs(next.position.vx - last.vx) > smooth_snap_distance ||
            std::abs(next.position.vy - last.vy) > smooth_snap_distance ||
            std::abs(next.position.vz - last.vz) > smooth_snap_distance ||
            std::abs(angle_delta(smooth.current.rotation.vy, next.rotation.vy)) > smooth_snap_angle ||
            std::abs(angle_delta(smooth.current.rotation.vx, next.rotation.vx)) > smooth_snap_angle;
    }
    smooth.interval_ns = jump ? smooth_default_interval_ns
        : std::clamp(now - smooth.current_ns, smooth_interval_min_ns, smooth_interval_max_ns);
    smooth.previous = jump ? next : smooth.current;
    smooth.current = next;
    // Entities: pair this update's poses with the last update's by tag.
    constexpr float entity_snap_distance = 3000.0f;   // farther in one update: a new occupant or a warp
    constexpr int entity_tags = 2048;
    smooth.motion.assign(entity_tags, {});
    std::vector<const HostState::EntityPose *> last(entity_tags, nullptr);
    if (!jump)
        for (const auto &pose : smooth.poses)
            if (pose.tag < entity_tags)
                last[pose.tag] = &pose;
    for (const auto &pose : host.frame_poses) {
        if (pose.tag >= entity_tags || !last[pose.tag])
            continue;
        const auto &before = *last[pose.tag];
        const float dx = before.x - pose.x, dy = before.y - pose.y, dz = before.z - pose.z;
        if (std::abs(dx) > entity_snap_distance || std::abs(dy) > entity_snap_distance || std::abs(dz) > entity_snap_distance)
            continue;
        const int turn = pose.yaw_sign ? angle_delta(pose.yaw, before.yaw) : 0;
        smooth.motion[pose.tag] = {true, pose.x, pose.y, pose.z, dx, dy, dz,
                                   static_cast<float>(turn) * 6.28318531f / 4096.0f, pose.yaw_sign};
    }
    smooth.poses = host.frame_poses;
    smooth.current_ns = now;
    smooth.have_current = true;
    smooth.active = true;
    smooth_present(now);
    return true;
}

bool smooth_wants_frames() {
    return smooth.active && smoothing_applies() && !host.paused;
}
}

static bool smooth_frames_wanted() { return smooth_wants_frames(); }
// Paused under the overlay: draw the kept world frame again so graphics option
// changes show immediately. Returns false when there is no such frame.
static bool smooth_redraw_paused() {
    if (!smooth.active || !render_smoothing || smooth.count == 0)
        return false;
    smooth_present(host_clock_ns());
    return true;
}
static Uint64 smooth_next_present_ns() { return smooth.next_present_ns; }
static void smooth_present_now(Uint64 now) { smooth_present(now); }

void host_frame_camera(const VECTOR &position, const SVECTOR &rotation, const Projection &projection) {
    host.clip_projection = projection;
    smooth.frame = {position, rotation, projection};
    smooth.frame_has_camera = true;
    // Unsmoothed frames are drawn from this camera; smoothed ones set their own.
    set_renderer_camera(rotation, static_cast<float>(position.vx), static_cast<float>(position.vy),
        static_cast<float>(position.vz));
    renderer_set_projection_distance(&host.renderer, static_cast<float>(projection.distance));
}

void host_light_occluders(const u8 *solid, int columns, int rows, float tile_size) {
    renderer_set_occluders(&host.renderer, solid, columns, rows, tile_size);
}

void host_present_faces(const FaceList *faces) {
    menu_preview_shown = menu_preview_building;
#if KF_EXT
    ext::game_frame(static_cast<int>(host.input_context));
#endif
    const bool smoothed = smooth_take_frame(faces);
    smooth.frame_has_camera = false;
    if (!smoothed) {
        apply_lights(host.frame_lights, host.frame_light_count);
        draw_and_swap(faces);
    }
}
}
