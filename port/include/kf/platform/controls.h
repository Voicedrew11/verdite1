#ifndef KF_PLATFORM_CONTROLS_H
#define KF_PLATFORM_CONTROLS_H

// Collect platform controls; the original game still owns their gameplay meaning.
#include <kf/lib/types.h>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace kf {
inline constexpr unsigned input_binding_capacity = 192;
enum class Action : u8 {
    forward,
    backward,
    strafe_left,
    strafe_right,
    turn_left,
    turn_right,
    look_up,
    look_down,
    attack,
    magic,
    interact,
    inventory,
    pause,
    pause_or_back,
    confirm,
    back,
    run,
    quick_save,
    quick_load,
    compare_language,
    full_map,   // no game meaning: for mods (kf.action_down)
    count
};
enum class InputDevice : u8 { keyboard, mouse, gamepad };
struct Control {
    InputDevice device;
    u32 code;
    friend bool operator==(const Control &, const Control &) = default;
};
struct InputFrame {
    u32 held, pressed, released;
    double look_x, look_y;
};
struct InputBinding {
    Control control;
    Action action;
    bool down;
};
struct InputState {
    std::array<InputBinding, input_binding_capacity> bindings;
    std::size_t binding_count;
    InputFrame pending;
};

u32 action_bit(Action action);
// An action as mods and settings.json name it ("attack", "pause_or_back"); empty past the last.
std::string_view action_name(Action action);
// The action with this name, matched exactly; nothing for an unknown name.
std::optional<Action> action_from_name(std::string_view name);
bool input_pressed(const InputFrame *frame, Action action);
bool input_bind(InputState *input, Control control, Action action);
// Replaces every binding with these, in order. A control held now stays held only if it keeps
// its action; any other starts released, so a key held through the change neither sticks nor
// presses its new action. The same bindings again change nothing. False, with nothing changed,
// for more than the capacity, an action past count or a control listed twice.
[[nodiscard]] bool input_set_bindings(InputState *input, std::span<const InputBinding> bindings);
void input_button(InputState *input, Control control, bool down);
void input_motion(InputState *input, double x, double y);
void input_disconnect(InputState *input, InputDevice device);
void input_clear(InputState *input);
InputFrame input_take(InputState *input);
} // namespace kf

#endif // KF_PLATFORM_CONTROLS_H
