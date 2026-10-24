#include <kf/platform/analog.h>

#include <kf/platform/input.h>
#include <verdite/stick.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace kf {
namespace {

constexpr float sensitivity_min = 0.1f;
constexpr float sensitivity_max = 2.0f;
// How far past the D-pad's rate the look may go: the sensitivity times the ramp.
constexpr s32 look_overspeed = 4;
// Tank reads each axis of the left stick on its own deadzone, so a thumb a
// little off forward walks straight and a sideways push turns in place.
constexpr float tank_deadzone = 0.3f;
constexpr float tank_turn_curve = 2.0f;
constexpr float vblanks_per_second = 60.0f;

struct AxisState {
    verdite::StepCarry carry;
    bool active = false;
};

struct AnalogState {
    AnalogOptions options;
    std::array<AxisState, 4> axes;
    verdite::LookAcceleration acceleration;
    float look_multiplier = 1.0f;
};
AnalogState analog;

// The sticks as the four velocities' buttons push them, each -1..1.
struct Deflection {
    std::array<float, 4> by_axis{};
    float look_magnitude = 0.0f;
};

float &at(Deflection &d, AnalogAxis axis) { return d.by_axis[static_cast<std::size_t>(axis)]; }

Deflection deflection() {
    const AnalogOptions &o = analog.options;
    Deflection d;
    if (!o.enabled)
        return d;
    const RawSticks raw = host_gamepad_sticks();
    if (o.move && o.mode == StickMode::Tank) {
        at(d, AnalogAxis::Forward) = -verdite::shape_axis(raw.left_y, {tank_deadzone, o.move_curve});
        at(d, AnalogAxis::Turn) = -verdite::shape_axis(raw.left_x, {tank_deadzone, tank_turn_curve});
    } else if (o.move) {
        const auto left = verdite::shape_stick({raw.left_x, raw.left_y}, {o.move_deadzone, o.move_curve});
        at(d, AnalogAxis::Forward) = -left.y;
        at(d, AnalogAxis::Strafe) = left.x;
    }
    if (o.look && o.mode == StickMode::Twin) {
        const auto right = verdite::shape_stick({raw.right_x, raw.right_y}, {o.look_deadzone, o.look_curve});
        at(d, AnalogAxis::Turn) = -right.x;
        at(d, AnalogAxis::Pitch) = right.y;
        d.look_magnitude = std::hypot(right.x, right.y);
    }
    if (o.invert_forward)
        at(d, AnalogAxis::Forward) *= -1.0f;
    if (o.invert_strafe)
        at(d, AnalogAxis::Strafe) *= -1.0f;
    if (o.invert_turn)
        at(d, AnalogAxis::Turn) *= -1.0f;
    if (o.invert_pitch)
        at(d, AnalogAxis::Pitch) *= -1.0f;
    return d;
}

bool is_look(AnalogAxis axis) { return axis == AnalogAxis::Turn || axis == AnalogAxis::Pitch; }

// Tank's turn is the D-pad's: released, the game's decay takes it.
bool look_stops_dead() {
    const AnalogOptions &o = analog.options;
    return o.mode == StickMode::Twin && (o.instant_stop || !o.ease);
}

float sensitivity(AnalogAxis axis) {
    const AnalogOptions &o = analog.options;
    switch (axis) {
    case AnalogAxis::Turn: return o.turn_sensitivity;
    case AnalogAxis::Pitch: return o.pitch_sensitivity;
    case AnalogAxis::Forward:
    case AnalogAxis::Strafe: break;
    }
    return o.move_sensitivity;
}

void update_look_multiplier(float look_magnitude) {
    const AnalogOptions &o = analog.options;
    if (!o.look_acceleration) {
        analog.acceleration.reset();
        analog.look_multiplier = 1.0f;
        return;
    }
    const float update_s = static_cast<float>(world_update_vblanks) / vblanks_per_second;
    analog.look_multiplier =
        analog.acceleration.update(look_magnitude, update_s, {o.acceleration_max, o.acceleration_time_s, 0.8f});
}

} // namespace

void host_set_analog_options(const AnalogOptions &options) {
    AnalogOptions o = options;
    o.turn_sensitivity = std::clamp(o.turn_sensitivity, sensitivity_min, sensitivity_max);
    o.pitch_sensitivity = std::clamp(o.pitch_sensitivity, sensitivity_min, sensitivity_max);
    o.move_sensitivity = std::clamp(o.move_sensitivity, sensitivity_min, sensitivity_max);
    o.look_deadzone = std::clamp(o.look_deadzone, 0.0f, 0.9f);
    o.move_deadzone = std::clamp(o.move_deadzone, 0.0f, 0.9f);
    o.look_curve = std::clamp(o.look_curve, 0.25f, 4.0f);
    o.move_curve = std::clamp(o.move_curve, 0.25f, 4.0f);
    o.acceleration_max = std::clamp(o.acceleration_max, 1.0f, 4.0f);
    o.acceleration_time_s = std::clamp(o.acceleration_time_s, 0.0f, 3.0f);
    analog.options = o;
}

bool host_analog_owns_left_stick() { return analog.options.enabled && analog.options.move; }

std::optional<s32> host_analog_velocity(AnalogAxis axis, s32 velocity, AnalogRates rates) {
    Deflection d = deflection();
    if (axis == AnalogAxis::Turn)
        update_look_multiplier(d.look_magnitude);
    AxisState &state = analog.axes[static_cast<std::size_t>(axis)];
    const float amount = at(d, axis);
    if (amount == 0.0f) {
        state.carry.reset();
        const bool was_active = std::exchange(state.active, false);
        if (was_active && is_look(axis) && look_stops_dead())
            return 0;
        return std::nullopt;
    }
    state.active = true;
    const float wanted = amount * static_cast<float>(rates.limit) * sensitivity(axis);
    if (!is_look(axis))
        return state.carry.take(wanted, rates.limit);
    const s32 target = state.carry.take(wanted * analog.look_multiplier, rates.limit * look_overspeed);
    const bool eased = analog.options.ease || analog.options.mode == StickMode::Tank;
    if (!eased)
        return target;
    return std::clamp(target, velocity - rates.accel, velocity + rates.accel);
}

} // namespace kf
