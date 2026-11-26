#include <kf/platform/bindings.h>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_mouse.h>

namespace kf {
namespace {
constexpr InputBinding key(SDL_Keycode code, Action action) {
    return {{InputDevice::keyboard, code}, action, false};
}
constexpr InputBinding mouse_button(int button, Action action) {
    return {{InputDevice::mouse, static_cast<u32>(button)}, action, false};
}
constexpr InputBinding pad_button(SDL_GamepadButton button, Action action) {
    return {{InputDevice::gamepad, static_cast<u32>(button)}, action, false};
}
constexpr InputBinding pad_axis(u32 code, Action action) {
    return {{InputDevice::gamepad, code}, action, false};
}

constexpr std::array<InputBinding, 23> key_defaults{
    key(SDLK_W, Action::forward), key(SDLK_UP, Action::forward),
    key(SDLK_S, Action::backward), key(SDLK_DOWN, Action::backward),
    key(SDLK_A, Action::strafe_left), key(SDLK_D, Action::strafe_right),
    key(SDLK_LEFT, Action::turn_left), key(SDLK_RIGHT, Action::turn_right),
    key(SDLK_PAGEUP, Action::look_up), key(SDLK_PAGEDOWN, Action::look_down),
    key(SDLK_F, Action::interact), key(SDLK_RETURN, Action::confirm),
    key(SDLK_SPACE, Action::attack), key(SDLK_Q, Action::magic),
    key(SDLK_TAB, Action::inventory), key(SDLK_BACKSPACE, Action::back),
    key(SDLK_P, Action::pause),
    key(SDLK_LSHIFT, Action::run), key(SDLK_RSHIFT, Action::run),
    key(SDLK_F2, Action::quick_save), key(SDLK_F4, Action::quick_load),
    key(SDLK_R, Action::compare_language), key(SDLK_M, Action::full_map)};
constexpr std::array<InputBinding, 2> mouse_defaults{
    mouse_button(SDL_BUTTON_LEFT, Action::attack), mouse_button(SDL_BUTTON_RIGHT, Action::magic)};
constexpr std::array<InputBinding, 10> pad_common{
    pad_button(SDL_GAMEPAD_BUTTON_DPAD_UP, Action::forward),
    pad_button(SDL_GAMEPAD_BUTTON_DPAD_DOWN, Action::backward),
    pad_button(SDL_GAMEPAD_BUTTON_DPAD_LEFT, Action::turn_left),
    pad_button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, Action::turn_right),
    pad_button(SDL_GAMEPAD_BUTTON_START, Action::inventory),
    pad_button(SDL_GAMEPAD_BUTTON_BACK, Action::pause),
    pad_button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, Action::strafe_left),
    pad_button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, Action::strafe_right),
    pad_button(SDL_GAMEPAD_BUTTON_LEFT_STICK, Action::run),
    pad_button(SDL_GAMEPAD_BUTTON_RIGHT_STICK, Action::compare_language)};
// The face buttons by position (SDL's south is Cross on a PlayStation pad).
// Japanese is this disc's own; West is the US King's Field's: Square attacks,
// Cross examines and confirms, Circle opens the menu and backs out, Triangle
// uses the item or spell.
constexpr std::array<InputBinding, 4> japanese_faces{
    pad_button(SDL_GAMEPAD_BUTTON_NORTH, Action::attack),
    pad_button(SDL_GAMEPAD_BUTTON_WEST, Action::magic),
    pad_button(SDL_GAMEPAD_BUTTON_EAST, Action::interact),
    pad_button(SDL_GAMEPAD_BUTTON_SOUTH, Action::back)};
constexpr std::array<InputBinding, 4> west_faces{
    pad_button(SDL_GAMEPAD_BUTTON_WEST, Action::attack),
    pad_button(SDL_GAMEPAD_BUTTON_NORTH, Action::magic),
    pad_button(SDL_GAMEPAD_BUTTON_SOUTH, Action::interact),
    pad_button(SDL_GAMEPAD_BUTTON_EAST, Action::back)};
constexpr std::array<InputBinding, 2> trigger_defaults{
    pad_axis(pad_axis_positive(SDL_GAMEPAD_AXIS_LEFT_TRIGGER), Action::look_up),
    pad_axis(pad_axis_positive(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), Action::look_down)};
// Axis directions use distinct controls so releasing a stick cannot release a held D-pad.
constexpr std::array<InputBinding, 6> fixed{
    key(SDLK_ESCAPE, Action::pause_or_back), key(SDLK_AC_BACK, Action::pause_or_back),
    pad_axis(pad_axis_negative(SDL_GAMEPAD_AXIS_LEFTX), Action::turn_left),
    pad_axis(pad_axis_positive(SDL_GAMEPAD_AXIS_LEFTX), Action::turn_right),
    pad_axis(pad_axis_negative(SDL_GAMEPAD_AXIS_LEFTY), Action::forward),
    pad_axis(pad_axis_positive(SDL_GAMEPAD_AXIS_LEFTY), Action::backward)};
// The fixed controls and both groups full still fit the input table.
static_assert(fixed.size() + 2 * action_count * controls_per_action <= input_binding_capacity);

// Every control the changed lists of one group hold.
struct ControlSet {
    std::array<Control, action_count * controls_per_action> list{};
    std::size_t count = 0;
    bool has(Control control) const {
        for (std::size_t i = 0; i < count; ++i)
            if (list[i] == control)
                return true;
        return false;
    }
    void add(Control control) {
        if (count < list.size() && !has(control))
            list[count++] = control;
    }
};

// Appends control unless the list has it already or is full.
bool add(ActionControls &controls, Control control) {
    if (controls.count == controls.list.size() || controls.has(control))
        return false;
    controls.list[controls.count++] = control;
    return true;
}
void add_all(GroupBindings &bindings, std::span<const InputBinding> table) {
    for (const auto &binding : table)
        add(bindings[static_cast<std::size_t>(binding.action)], binding.control);
}

// Whether a default list is a face button's, which PadLayout moves: a change equal to it under
// one layout differs from it under the other.
bool follows_layout(const ActionControls &defaults) {
    for (const auto &binding : west_faces)
        if (defaults.has(binding.control))
            return true;
    return false;
}

// Keys that type a letter, a digit or a backspace on the title's join and name pages.
bool types_on_title(u32 code) {
    return (code >= SDLK_A && code <= SDLK_Z) || (code >= SDLK_0 && code <= SDLK_9)
        || (code >= SDLK_KP_1 && code <= SDLK_KP_9) || code == SDLK_KP_0 || code == SDLK_BACKSPACE;
}

// A way through the menus: any one usable control on any of its actions is enough.
struct Way {
    BindingWay way;
    std::span<const Action> actions;
};
constexpr std::array<Action, 2> confirm_actions{Action::confirm, Action::interact};
constexpr std::array<Action, 3> back_actions{Action::back, Action::inventory,
                                             Action::pause_or_back};
// In play, Back opens the menu as the inventory action's Start does (pause_or_back pauses there).
constexpr std::array<Action, 2> menu_actions{Action::inventory, Action::back};
// Escape always backs out on the keys, so only the pad checks a way back.
constexpr std::array<Way, 2> key_ways{
    {{BindingWay::confirm, confirm_actions}, {BindingWay::menu, menu_actions}}};
constexpr std::array<Way, 3> pad_ways{{{BindingWay::confirm, confirm_actions},
                                       {BindingWay::back, back_actions},
                                       {BindingWay::menu, menu_actions}}};

bool usable(BindingGroup group, BindingWay way, Control control) {
    if (group == BindingGroup::pad)
        return control.device == InputDevice::gamepad;
    if (control.device != InputDevice::keyboard)
        return false;
    return way != BindingWay::confirm || !types_on_title(control.code);
}
bool way_open(const GroupBindings &bindings, BindingGroup group, const Way &way) {
    for (const auto action : way.actions)
        for (const auto control : bindings[static_cast<std::size_t>(action)].controls())
            if (usable(group, way.way, control))
                return true;
    return false;
}
} // namespace

bool ActionControls::has(Control control) const {
    for (const auto held : controls())
        if (held == control)
            return true;
    return false;
}
bool operator==(const ActionControls &a, const ActionControls &b) {
    if (a.count != b.count)
        return false;
    for (std::size_t i = 0; i < a.count; ++i)
        if (a.list[i] != b.list[i])
            return false;
    return true;
}

BindingGroup binding_group(Control control) {
    return control.device == InputDevice::gamepad ? BindingGroup::pad : BindingGroup::keys;
}
GroupBindings default_bindings(BindingGroup group, PadLayout layout) {
    GroupBindings bindings{};
    if (group == BindingGroup::keys) {
        add_all(bindings, key_defaults);
        add_all(bindings, mouse_defaults);
        return bindings;
    }
    add_all(bindings, pad_common);
    add_all(bindings, layout == PadLayout::Japanese ? std::span<const InputBinding>(japanese_faces)
                                                    : std::span<const InputBinding>(west_faces));
    add_all(bindings, trigger_defaults);
    return bindings;
}
std::span<const InputBinding> fixed_bindings() {
    return fixed;
}

GroupBindings bindings_resolve(const GroupBindings &defaults, const GroupChanges &changes) {
    GroupBindings resolved{};
    ControlSet claimed;
    for (std::size_t a = 0; a < action_count; ++a) {
        const auto &change = changes[a];
        if (!change)
            continue;
        for (const auto control : change->controls()) {
            if (!claimed.has(control) && add(resolved[a], control))
                claimed.add(control);
        }
    }
    for (std::size_t a = 0; a < action_count; ++a) {
        if (changes[a])
            continue;
        for (const auto control : defaults[a].controls())
            if (!claimed.has(control))
                add(resolved[a], control);
    }
    return resolved;
}
Bindings bindings_resolve(const BindingChanges &changes, PadLayout layout) {
    return {bindings_resolve(default_bindings(BindingGroup::keys, layout), changes.keys),
            bindings_resolve(default_bindings(BindingGroup::pad, layout), changes.pad)};
}

GroupChanges bindings_tidy(const GroupBindings &defaults, GroupChanges changes) {
    ControlSet claimed;
    for (auto &change : changes) {
        if (!change)
            continue;
        ActionControls kept;
        for (const auto control : change->controls())
            if (!claimed.has(control) && add(kept, control))
                claimed.add(control);
        change = kept;
    }
    for (std::size_t a = 0; a < action_count; ++a) {
        auto &change = changes[a];
        if (change && *change == defaults[a] && !follows_layout(defaults[a]))
            change.reset();
    }
    return changes;
}
GroupChanges bindings_set(const GroupBindings &defaults, GroupChanges changes, Action action,
                          const ActionControls &controls) {
    const auto index = static_cast<std::size_t>(action);
    if (index >= action_count)
        return bindings_tidy(defaults, changes);
    for (std::size_t a = 0; a < action_count; ++a) {
        auto &change = changes[a];
        if (a == index || !change)
            continue;
        ActionControls kept;
        for (const auto control : change->controls())
            if (!controls.has(control))
                add(kept, control);
        change = kept;
    }
    changes[index] = controls;
    return bindings_tidy(defaults, changes);
}

std::optional<BindingWay> bindings_missing_way(const GroupBindings &bindings, BindingGroup group) {
    const auto ways = group == BindingGroup::keys ? std::span<const Way>(key_ways)
                                                  : std::span<const Way>(pad_ways);
    for (const auto &way : ways)
        if (!way_open(bindings, group, way))
            return way.way;
    return std::nullopt;
}

u32 bound_key(u32 scancode, u32 layout_key) {
    // Letter shortcuts follow physical QWERTY positions in every layout.
    constexpr auto first = static_cast<u32>(SDL_SCANCODE_A);
    constexpr auto last = static_cast<u32>(SDL_SCANCODE_Z);
    if (scancode >= first && scancode <= last)
        return SDLK_A + (scancode - first);
    return layout_key;
}
} // namespace kf
