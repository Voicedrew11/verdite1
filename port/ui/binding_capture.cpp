// The capture behind the F1 menu's binding rows: it waits for a key, mouse button, pad button or
// trigger and adds it through binding_add. Events reach the capture before the menu's own keys, so
// Escape and F1 can cancel it; the menu takes no pad input while it waits, and after a pad capture
// until that control is let go (binding_capture_busy).
#include "internal.hpp"
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace kf::ext {
namespace {
constexpr std::uint64_t capture_wait_ms = 5000;   // a capture nothing answers gives up
constexpr std::uint64_t release_wait_ms = 2000;   // pad navigation comes back by then at the latest

constexpr const char *unchanged_note = "Unchanged.";
constexpr const char *timeout_note = "Nothing pressed: unchanged.";
constexpr const char *wants_key_note = "Press a key or mouse button.";
constexpr const char *wants_pad_note = "Press a pad button or trigger.";
constexpr const char *reserved_note = "F11 and Alt+Enter switch fullscreen; they cannot be set.";

enum class Stage : std::uint8_t { idle, waiting, releasing };

struct Capture {
    Stage stage = Stage::idle;
    BindingRow row{};
    std::uint64_t started_ms = 0;
    SDL_Keycode pending_modifier = SDLK_UNKNOWN;   // bound when let go, if nothing else came first
    Control releasing{};   // the pad control just bound, which holds navigation off until let go
    std::uint64_t releasing_since_ms = 0;
    std::array<Sint16, 2> trigger_value{};   // left, right: as last seen, in any stage
    // Taken going down, in any stage, until let go: no repeat reaches the menu as a new press.
    std::bitset<SDL_SCANCODE_COUNT> keys_taken;
    // Left, right: bound by a capture and still past the deadzone, however long the releasing stage lasts.
    std::array<bool, 2> triggers_taken{};
};
Capture capture;

bool same_row(BindingRow a, BindingRow b) { return a.group == b.group && a.action == b.action; }

void go_idle() {
    capture.stage = Stage::idle;
    capture.pending_modifier = SDLK_UNKNOWN;
}

// Ends any capture; a waiting one's row gets the note, if there is one.
void cancel(const char *note) {
    if (capture.stage == Stage::idle)
        return;
    const bool waiting = capture.stage == Stage::waiting;
    go_idle();
    if (waiting && note)
        binding_note(capture.row, note);
}

void finish_key_or_mouse(Control control) {
    binding_add(capture.row, control);
    go_idle();
}

// The pad still holds the control, so the menu takes no pad input until it is let go.
void finish_pad(Control control) {
    binding_add(capture.row, control);
    capture.stage = Stage::releasing;
    capture.releasing = control;
    capture.releasing_since_ms = SDL_GetTicks();
}

bool is_modifier(SDL_Keycode key) {
    switch (key) {
    case SDLK_LCTRL: case SDLK_RCTRL: case SDLK_LSHIFT: case SDLK_RSHIFT:
    case SDLK_LALT: case SDLK_RALT: case SDLK_LGUI: case SDLK_RGUI:
        return true;
    default:
        return false;
    }
}

// The keycode the game matches for this key (host.cpp's process_keyboard_event).
SDL_Keycode event_key(const SDL_KeyboardEvent &key) {
    const auto scancode = static_cast<u32>(key.scancode);
    return bound_key(scancode, SDL_GetKeyFromScancode(key.scancode, SDL_KMOD_NONE, false));
}

std::optional<std::size_t> trigger_slot(Uint8 axis) {
    if (axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER)
        return 0;
    if (axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)
        return 1;
    return std::nullopt;
}

bool on_key_down(const SDL_KeyboardEvent &key) {
    if (key.repeat)
        return true;
    if (key.key == SDLK_ESCAPE) {
        cancel(unchanged_note);
        return true;
    }
    if (key.key == SDLK_F1) {   // the overlay closes it as usual
        cancel(nullptr);
        return false;
    }
    if (key.key == SDLK_F11 || (key.key == SDLK_RETURN && (key.mod & SDL_KMOD_ALT) != 0)) {
        capture.pending_modifier = SDLK_UNKNOWN;
        binding_note(capture.row, reserved_note);
        return false;
    }
    if (capture.row.group == BindingGroup::pad) {
        binding_note(capture.row, wants_pad_note);
        return true;
    }
    const SDL_Keycode code = event_key(key);
    if (is_modifier(code)) {   // Alt+Enter and AltGr must not bind on the way down
        capture.pending_modifier = code;
        return true;
    }
    capture.pending_modifier = SDLK_UNKNOWN;
    const Control control{InputDevice::keyboard, code};
    if (code != SDLK_UNKNOWN && control_bindable(BindingGroup::keys, control))
        finish_key_or_mouse(control);
    return true;
}

// The menu and the game must see every key let go, so none is taken.
bool on_key_up(const SDL_KeyboardEvent &key) {
    if (capture.pending_modifier != SDLK_UNKNOWN && event_key(key) == capture.pending_modifier)
        finish_key_or_mouse({InputDevice::keyboard, capture.pending_modifier});
    return false;
}

bool on_mouse_down(const SDL_MouseButtonEvent &button) {
    if (capture.row.group == BindingGroup::pad)   // the menu's: a press elsewhere on it
        return false;
    if (button.which == SDL_TOUCH_MOUSEID) {
        cancel(unchanged_note);
        return true;
    }
    const Control control{InputDevice::mouse, button.button};
    if (control_bindable(BindingGroup::keys, control))
        finish_key_or_mouse(control);
    return true;
}

bool on_pad_button(const SDL_GamepadButtonEvent &button) {
    if (capture.row.group == BindingGroup::keys) {
        binding_note(capture.row, wants_key_note);
        return true;
    }
    const Control control{InputDevice::gamepad, button.button};
    if (control_bindable(BindingGroup::pad, control))
        finish_pad(control);
    return true;
}

// Only a trigger pushed past the deadzone counts, so one held when the capture began must be let go first.
bool on_pad_axis(const SDL_GamepadAxisEvent &axis) {
    const std::optional<std::size_t> slot = trigger_slot(axis.axis);
    if (!slot || capture.trigger_value[*slot] > pad_axis_deadzone || axis.value <= pad_axis_deadzone)
        return false;
    if (capture.row.group == BindingGroup::keys) {
        binding_note(capture.row, wants_key_note);
        return false;
    }
    finish_pad({InputDevice::gamepad, pad_axis_positive(axis.axis)});
    capture.triggers_taken[*slot] = true;
    return true;
}

// The host never saw the bound control go down (a trigger's motion past the deadzone is taken by
// held_takes), so its letting go is passed on as a no-op.
bool on_releasing(const SDL_Event &event) {
    const bool button_up = event.type == SDL_EVENT_GAMEPAD_BUTTON_UP &&
                           capture.releasing == Control{InputDevice::gamepad, event.gbutton.button};
    const bool trigger_down = event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION &&
                              capture.releasing == Control{InputDevice::gamepad, pad_axis_positive(event.gaxis.axis)} &&
                              event.gaxis.value <= pad_axis_deadzone;
    if (button_up || trigger_down || event.type == SDL_EVENT_GAMEPAD_REMOVED)
        go_idle();
    return false;
}

bool capture_takes(const SDL_Event &event) {
    if (capture.stage == Stage::idle)
        return false;
    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        cancel(nullptr);
        return false;
    }
    if (capture.stage == Stage::releasing)
        return on_releasing(event);
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
        return on_key_down(event.key);
    case SDL_EVENT_KEY_UP:
        return on_key_up(event.key);
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        return on_mouse_down(event.button);
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        return on_pad_button(event.gbutton);
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        return on_pad_axis(event.gaxis);
    default:
        return false;
    }
}

std::optional<std::size_t> key_slot(const SDL_KeyboardEvent &key) {
    const auto slot = static_cast<std::size_t>(key.scancode);
    if (slot >= capture.keys_taken.size())
        return std::nullopt;
    return slot;
}

// What the capture took going down stays taken until let go, after the capture too. The menu never
// saw a taken key go down, so its first repeat would be a press: a listen again, or a move. The
// host's table binds a taken trigger at once, so its motion would press the new action. The
// letting go itself is passed on: the menu and the host already have it up.
bool held_takes(const SDL_Event &event) {
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN: {
        const std::optional<std::size_t> slot = key_slot(event.key);
        return event.key.repeat && slot && capture.keys_taken.test(*slot);
    }
    case SDL_EVENT_KEY_UP:
        if (const std::optional<std::size_t> slot = key_slot(event.key))
            capture.keys_taken.reset(*slot);
        return false;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        const std::optional<std::size_t> slot = trigger_slot(event.gaxis.axis);
        if (!slot || !capture.triggers_taken[*slot])
            return false;
        if (event.gaxis.value > pad_axis_deadzone)
            return true;
        capture.triggers_taken[*slot] = false;
        return false;
    }
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        capture.keys_taken.reset();
        return false;
    case SDL_EVENT_GAMEPAD_REMOVED:
        capture.triggers_taken = {};
        return false;
    default:
        return false;
    }
}

void remember_taken_key(const SDL_Event &event) {
    if (event.type != SDL_EVENT_KEY_DOWN)
        return;
    if (const std::optional<std::size_t> slot = key_slot(event.key))
        capture.keys_taken.set(*slot);
}

void remember_trigger(const SDL_Event &event) {
    if (event.type != SDL_EVENT_GAMEPAD_AXIS_MOTION)
        return;
    if (const std::optional<std::size_t> slot = trigger_slot(event.gaxis.axis))
        capture.trigger_value[*slot] = event.gaxis.value;
}

} // namespace

void binding_capture_start(BindingRow row) {
    capture.stage = Stage::waiting;
    capture.row = row;
    capture.started_ms = SDL_GetTicks();
    capture.pending_modifier = SDLK_UNKNOWN;
}

bool binding_capture_waiting(BindingRow row) {
    return capture.stage == Stage::waiting && same_row(capture.row, row);
}

bool binding_capture_busy() { return capture.stage != Stage::idle; }

int binding_capture_seconds_left() {
    if (capture.stage != Stage::waiting)
        return 0;
    const std::uint64_t waited_ms = SDL_GetTicks() - capture.started_ms;
    const std::uint64_t left_ms = waited_ms < capture_wait_ms ? capture_wait_ms - waited_ms : 0;
    return static_cast<int>((left_ms + 999) / 1000);
}

bool binding_capture_event(const SDL_Event &event) {
    const bool taken = held_takes(event) || capture_takes(event);
    if (taken)
        remember_taken_key(event);
    remember_trigger(event);
    return taken;
}

void binding_capture_frame() {
    const std::uint64_t now_ms = SDL_GetTicks();
    if (capture.stage == Stage::releasing && now_ms - capture.releasing_since_ms >= release_wait_ms)
        go_idle();
    if (capture.stage == Stage::waiting && now_ms - capture.started_ms >= capture_wait_ms)
        cancel(timeout_note);
}

void binding_capture_cancel() { cancel(nullptr); }
} // namespace kf::ext
