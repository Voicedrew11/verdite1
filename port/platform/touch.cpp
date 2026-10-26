#include <kf/platform/touch.h>

#include <kf/platform/controls.h>
#include <kf/platform/input.h>
#include <kf/platform/menu_pointer.h>
#include <verdite/touch.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace kf {
namespace {

// A drag the safe area's height long turns the view this far.
constexpr double look_degrees_per_height = 110;
// A stick pushed this far holds its direction's button, when analog movement is off.
constexpr float stick_button_throw = 0.5f;
// A gamepad axis past this hides the controls (a resting stick drifts below it).
constexpr Sint16 gamepad_axis_in_use = 16000;
// A button takes a finger a little outside the circle drawn.
constexpr float button_reach = 1.25f;

enum class Edge : u8 { Left, Right };

// A control of KF1's. Buttons are circles placed from an edge in heights, so
// they keep their place at the thumbs on any width; a stick rests there.
// Sticks, look areas and taps take the zone given.
struct Kf1Control {
    verdite::TouchKind kind;
    TouchShape shape;
    const char *label;
    Action action;          // Action::count: none
    TouchCommand command;
    Edge edge;
    float x, y;             // centre: heights from the edge, fraction of the height down
    float radius;           // of the height
    verdite::TouchArea zone;
};

using verdite::TouchKind;
constexpr Action no_action = Action::count;

// The first row that holds a point wins: buttons before the stick and the look area.
constexpr std::array<Kf1Control, 10> walking_controls = {{
    {TouchKind::Button, TouchShape::Button, "Attack", Action::attack, TouchCommand::None, Edge::Right, 0.22f, 0.74f, 0.13f, {}},
    {TouchKind::Button, TouchShape::Button, "Examine", Action::interact, TouchCommand::None, Edge::Right, 0.20f, 0.44f, 0.09f, {}},
    {TouchKind::Button, TouchShape::Button, "Magic", Action::magic, TouchCommand::None, Edge::Right, 0.50f, 0.84f, 0.09f, {}},
    {TouchKind::Button, TouchShape::Button, "Run", Action::run, TouchCommand::None, Edge::Right, 0.50f, 0.58f, 0.08f, {}},
    {TouchKind::Button, TouchShape::Button, "Menu", Action::inventory, TouchCommand::None, Edge::Right, 0.56f, 0.09f, 0.06f, {}},
    {TouchKind::Button, TouchShape::Button, "Pause", Action::pause, TouchCommand::None, Edge::Right, 0.40f, 0.09f, 0.06f, {}},
    {TouchKind::Button, TouchShape::Button, "Map", no_action, TouchCommand::FullMap, Edge::Right, 0.24f, 0.09f, 0.06f, {}},
    {TouchKind::Button, TouchShape::Gear, nullptr, no_action, TouchCommand::Settings, Edge::Right, 0.08f, 0.09f, 0.06f, {}},
    {TouchKind::Stick, TouchShape::Stick, nullptr, no_action, TouchCommand::None, Edge::Left, 0.32f, 0.68f, 0.13f, {0, 0, 0.45f, 1}},
    {TouchKind::Drag, TouchShape::Button, nullptr, no_action, TouchCommand::None, Edge::Left, 0, 0, 0, {0.45f, 0, 0.55f, 1}},
}};
// The full map is up: only its button, to close it.
constexpr std::array<Kf1Control, 1> full_map_controls = {walking_controls[6]};
constexpr std::array<Kf1Control, 1> tap_controls = {{
    {TouchKind::Tap, TouchShape::Button, nullptr, Action::confirm, TouchCommand::None, Edge::Left, 0, 0, 0, {0, 0, 1, 1}},
}};

enum class Layout : u8 { None, Walking, FullMap, Tap, Menu };

std::span<const Kf1Control> layout_controls(Layout layout) {
    switch (layout) {
    case Layout::Walking: return walking_controls;
    case Layout::FullMap: return full_map_controls;
    case Layout::Tap:
    case Layout::Menu: return tap_controls;
    case Layout::None: break;
    }
    return {};
}

bool drawn(Layout layout) { return layout == Layout::Walking || layout == Layout::FullMap; }

struct SafeArea {
    float x_px = 0, y_px = 0, width_px = 0, height_px = 0;
    bool operator==(const SafeArea &) const = default;
};

struct TouchScreen {
    verdite::TouchControls controls;
    Layout layout = Layout::None;
    SafeArea area;
    bool classic = false;
    std::vector<const Kf1Control *> rows;   // as laid out, in the controls' order
    std::vector<bool> held_before;
    std::optional<bool> wanted;             // the player is on the touch screen
    std::optional<SDL_FingerID> menu_finger;
    u32 held = 0, pressed = 0;
    RawSticks sticks{};
    TouchCommand command = TouchCommand::None;
    std::vector<TouchShown> shown;
};
TouchScreen screen;

bool wanted() {
    if (!screen.wanted) {
        int count = 0;
        SDL_TouchID *devices = SDL_GetTouchDevices(&count);
        SDL_free(devices);
        screen.wanted = count > 0;
    }
    return *screen.wanted;
}

void set_wanted(bool on) {
    if (wanted() == on)
        return;
    screen.wanted = on;
    if (!on)
        screen.controls.release_all();
}

Layout layout_for(TouchCover cover) {
    switch (cover) {
    case TouchCover::Overlay: return Layout::None;
    case TouchCover::FullMap: return classic_mode ? Layout::None : Layout::FullMap;
    case TouchCover::None: break;
    }
    switch (host_input_context()) {
    case InputContext::Gameplay: return Layout::Walking;
    case InputContext::Menu: return Layout::Menu;
    case InputContext::Opening:
    case InputContext::Scripted: break;
    }
    return Layout::Tap;
}

float centre_x(const Kf1Control &control, const SafeArea &area) {
    const float from_edge = control.x * area.height_px / area.width_px;
    return control.edge == Edge::Left ? from_edge : 1 - from_edge;
}

verdite::TouchArea zone_of(const Kf1Control &control, const SafeArea &area) {
    if (control.kind != TouchKind::Button)
        return control.zone;
    const float reach = control.radius * button_reach;
    const float half_width = reach * area.height_px / area.width_px;
    return {centre_x(control, area) - half_width, control.y - reach, 2 * half_width, 2 * reach};
}

void lay_out(Layout layout, const SafeArea &area) {
    if (layout == screen.layout && area == screen.area && classic_mode == screen.classic)
        return;
    screen.layout = layout;
    screen.area = area;
    screen.classic = classic_mode;
    screen.rows.clear();
    std::vector<verdite::TouchControl> controls;
    for (const Kf1Control &control : layout_controls(layout)) {
        if (control.command == TouchCommand::FullMap && classic_mode)
            continue;   // no automap in Classic mode
        controls.push_back({static_cast<std::uint32_t>(screen.rows.size()), control.kind, zone_of(control, area),
                            control.radius});
        screen.rows.push_back(&control);
    }
    screen.controls.set_safe_area(area.x_px, area.y_px, area.width_px, area.height_px);
    screen.controls.set_layout(controls);
    screen.held_before.assign(screen.rows.size(), false);
    screen.held = screen.pressed = 0;
    screen.sticks = {};
}

SafeArea safe_area(SDL_Window *window) {
    int width_px = 0, height_px = 0;
    SDL_Rect rect{};
    if (!SDL_GetWindowSizeInPixels(window, &width_px, &height_px) || width_px <= 0 || height_px <= 0)
        return {};
    const float density = SDL_GetWindowPixelDensity(window);
    if (!SDL_GetWindowSafeArea(window, &rect) || rect.w <= 0 || rect.h <= 0 || density <= 0)
        return {0, 0, static_cast<float>(width_px), static_cast<float>(height_px)};
    return {static_cast<float>(rect.x) * density, static_cast<float>(rect.y) * density,
            static_cast<float>(rect.w) * density, static_cast<float>(rect.h) * density};
}

u32 stick_buttons(verdite::StickAxes stick) {
    u32 buttons = 0;
    if (stick.y < -stick_button_throw)
        buttons |= action_bit(Action::forward);
    if (stick.y > stick_button_throw)
        buttons |= action_bit(Action::backward);
    if (stick.x < -stick_button_throw)
        buttons |= action_bit(Action::turn_left);
    if (stick.x > stick_button_throw)
        buttons |= action_bit(Action::turn_right);
    return buttons;
}

void show(const Kf1Control &control, const verdite::TouchState &state) {
    const SafeArea &area = screen.area;
    TouchShown shown{control.shape, control.label, 0, 0, control.radius * area.height_px, state.held, 0, 0};
    if (control.kind == TouchKind::Stick && state.held) {
        shown.x_px = area.x_px + state.centre_x * area.width_px;
        shown.y_px = area.y_px + state.centre_y * area.height_px;
    } else {
        shown.x_px = area.x_px + centre_x(control, area) * area.width_px;
        shown.y_px = area.y_px + control.y * area.height_px;
    }
    shown.knob_x_px = shown.x_px + state.stick.x * shown.radius_px;
    shown.knob_y_px = shown.y_px + state.stick.y * shown.radius_px;
    screen.shown.push_back(shown);
}

// The frame's controls as the game's actions, the stick, the look and commands.
TouchLook read_frame() {
    const auto frame = screen.controls.take_frame();
    TouchLook look{0, 0};
    screen.held = 0;
    screen.sticks = {};
    screen.shown.clear();
    for (std::size_t i = 0; i < frame.size() && i < screen.rows.size(); ++i) {
        const Kf1Control &control = *screen.rows[i];
        const verdite::TouchState &state = frame[i];
        const bool pressed = state.held && !screen.held_before[i];
        screen.held_before[i] = state.held;
        const u32 bit = control.action == no_action ? 0 : action_bit(control.action);
        if (control.kind == TouchKind::Button && state.held)
            screen.held |= bit;
        if ((control.kind == TouchKind::Button && pressed) || state.tapped)
            screen.pressed |= bit;
        if (pressed && control.command != TouchCommand::None)
            screen.command = control.command;
        if (control.kind == TouchKind::Stick) {
            screen.sticks.left_x = state.stick.x;
            screen.sticks.left_y = state.stick.y;
        }
        if (control.kind == TouchKind::Drag && screen.area.height_px > 0) {
            look.yaw_degrees += state.drag_x_px / screen.area.height_px * look_degrees_per_height;
            look.pitch_degrees += state.drag_y_px / screen.area.height_px * look_degrees_per_height;
        }
        if (drawn(screen.layout) && wanted() && control.kind != TouchKind::Drag)
            show(control, state);
    }
    if (!host_analog_owns_left_stick())
        screen.held |= stick_buttons({screen.sticks.left_x, screen.sticks.left_y});
    return look;
}

struct FingerAt { float x_px, y_px, x_points, y_points; };

FingerAt finger_at(SDL_Window *window, const SDL_TouchFingerEvent &finger) {
    int width_px = 0, height_px = 0, width = 0, height = 0;
    if (!SDL_GetWindowSizeInPixels(window, &width_px, &height_px) || !SDL_GetWindowSize(window, &width, &height))
        return {-1, -1, -1, -1};
    return {finger.x * static_cast<float>(width_px), finger.y * static_cast<float>(height_px),
            finger.x * static_cast<float>(width), finger.y * static_cast<float>(height)};
}

void finger_event(SDL_Window *window, const SDL_Event &event) {
    const SDL_TouchFingerEvent &finger = event.tfinger;
    // A mouse made into touches (SDL's hint) is the mouse.
    if (finger.touchID == SDL_MOUSE_TOUCHID || !window)
        return;
    const FingerAt at = finger_at(window, finger);
    const std::uint64_t at_ms = finger.timestamp / 1'000'000;
    const bool menu_finger = screen.menu_finger == finger.fingerID;
    switch (event.type) {
    case SDL_EVENT_FINGER_DOWN:
        set_wanted(true);
        if (screen.layout == Layout::Menu && host_menu_has_rows()) {
            screen.menu_finger = finger.fingerID;
            host_menu_touch_down(window, at.x_points, at.y_points);
        } else if (screen.layout != Layout::None) {
            screen.controls.finger_down(finger.fingerID, at.x_px, at.y_px, at_ms);
        }
        break;
    case SDL_EVENT_FINGER_MOTION:
        if (menu_finger)
            host_menu_touch_moved(window, at.x_points, at.y_points);
        else
            screen.controls.finger_moved(finger.fingerID, at.x_px, at.y_px);
        break;
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED:
        if (menu_finger) {
            screen.menu_finger.reset();
            if (event.type == SDL_EVENT_FINGER_UP)
                host_menu_touch_up(window, at.x_points, at.y_points);
            else
                host_menu_pointer_gone();
        } else {
            screen.controls.finger_up(finger.fingerID, event.type == SDL_EVENT_FINGER_UP ? at_ms : UINT64_MAX);
        }
        break;
    default:
        break;
    }
}

} // namespace

void host_touch_event(SDL_Window *window, const SDL_Event &event) {
    switch (event.type) {
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED:
        finger_event(window, event);
        break;
    case SDL_EVENT_KEY_DOWN:
        // The Android back button is the phone's own, not a keyboard.
        if (event.key.key != SDLK_AC_BACK)
            set_wanted(false);
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (event.motion.which != SDL_TOUCH_MOUSEID)
            set_wanted(false);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (event.button.which != SDL_TOUCH_MOUSEID)
            set_wanted(false);
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        set_wanted(false);
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (event.gaxis.value > gamepad_axis_in_use || event.gaxis.value < -gamepad_axis_in_use)
            set_wanted(false);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
        screen.controls.release_all();
        screen.menu_finger.reset();
        break;
    default:
        break;
    }
}

TouchLook host_touch_poll(SDL_Window *window, TouchCover cover) {
    if (!window)
        return {0, 0};
    const Layout layout = layout_for(cover);
    if (layout != screen.layout)
        screen.menu_finger.reset();
    lay_out(layout, safe_area(window));
    return read_frame();
}

TouchActions host_touch_take_actions() {
    const TouchActions actions{screen.held | screen.pressed, screen.pressed};
    screen.pressed = 0;
    return actions;
}

u32 host_touch_held_actions() { return screen.held; }

RawSticks host_touch_sticks() { return screen.sticks; }

TouchCommand host_touch_take_command() { return std::exchange(screen.command, TouchCommand::None); }

std::span<const TouchShown> host_touch_shown() { return screen.shown; }

} // namespace kf
