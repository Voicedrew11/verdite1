#include <kf/platform/controls.h>

#include <cmath>

namespace kf {
namespace {
// In Action's order.
constexpr std::array<std::string_view, 21> action_names{
    "forward", "backward", "strafe_left", "strafe_right", "turn_left", "turn_right", "look_up", "look_down",
    "attack", "magic", "interact", "inventory", "pause", "pause_or_back", "confirm", "back", "run",
    "quick_save", "quick_load", "compare_language", "full_map"};
static_assert(action_names.size() == static_cast<std::size_t>(Action::count));
} // namespace

u32 action_bit(Action action) {
    return 1u << static_cast<unsigned>(action);
}
std::string_view action_name(Action action) {
    const auto index = static_cast<std::size_t>(action);
    return index < action_names.size() ? action_names[index] : std::string_view{};
}
std::optional<Action> action_from_name(std::string_view name) {
    for (std::size_t i = 0; i < action_names.size(); ++i)
        if (action_names[i] == name)
            return static_cast<Action>(i);
    return std::nullopt;
}
bool input_pressed(const InputFrame *frame, Action action) {
    return (frame->pressed & action_bit(action)) != 0;
}
static void update_held(InputState *input) {
    u32 held = 0;
    for (std::size_t i = 0; i < input->binding_count; ++i)
        if (input->bindings[i].down)
            held |= action_bit(input->bindings[i].action);
    input->pending.pressed |= held & ~input->pending.held;
    input->pending.released |= input->pending.held & ~held;
    input->pending.held = held;
}
bool input_bind(InputState *input, Control control, Action action) {
    if (action >= Action::count)
        return false;
    for (std::size_t i = 0; i < input->binding_count; ++i) {
        auto *binding = &input->bindings[i];
        if (binding->control.device == control.device && binding->control.code == control.code) {
            binding->action = action;
            update_held(input);
            return true;
        }
    }
    if (input->binding_count == input->bindings.size())
        return false;
    input->bindings[input->binding_count++] = {control, action, false};
    return true;
}
void input_button(InputState *input, Control control, bool down) {
    for (std::size_t i = 0; i < input->binding_count; ++i) {
        auto *binding = &input->bindings[i];
        if (binding->control.device == control.device && binding->control.code == control.code)
            binding->down = down;
    }
    update_held(input);
}
void input_motion(InputState *input, double x, double y) {
    if (std::isfinite(x) && std::isfinite(y)) {
        input->pending.look_x = std::fmax(-1e6, std::fmin(1e6, input->pending.look_x + x));
        input->pending.look_y = std::fmax(-1e6, std::fmin(1e6, input->pending.look_y + y));
    }
}
void input_disconnect(InputState *input, InputDevice device) {
    for (std::size_t i = 0; i < input->binding_count; ++i)
        if (input->bindings[i].control.device == device)
            input->bindings[i].down = false;
    update_held(input);
}
void input_clear(InputState *input) {
    for (std::size_t i = 0; i < input->binding_count; ++i)
        input->bindings[i].down = false;
    input->pending = {};
}
InputFrame input_take(InputState *input) {
    const auto result = input->pending;
    input->pending.pressed = input->pending.released = 0;
    input->pending.look_x = input->pending.look_y = 0;
    return result;
}
} // namespace kf
