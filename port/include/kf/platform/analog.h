#ifndef KF_PLATFORM_ANALOG_H
#define KF_PLATFORM_ANALOG_H

// Analog movement and look from the gamepad's sticks. The game's player update
// asks for each velocity it steers; while a stick is centred the answer is
// empty and the game's own D-pad branches run, so its decay carries a released
// stick as it carries a released button.
#include <kf/lib/types.h>

#include <optional>

namespace kf {

// Twin stick: the left stick walks and strafes, the right turns and looks.
// Tank (one stick): the left walks and turns, L1 and R1 still strafe.
enum class StickMode : u8 { Twin, Tank };

struct AnalogOptions {
    bool enabled = true;
    bool look = true;           // the right stick turns and looks (twin)
    bool move = true;           // the left stick walks
    StickMode mode = StickMode::Twin;
    float turn_sensitivity = 1.0f;   // 1: full deflection is the D-pad's rate
    float pitch_sensitivity = 1.0f;
    float move_sensitivity = 1.0f;
    float look_deadzone = 0.15f;
    float move_deadzone = 0.15f;
    float look_curve = 1.35f;
    float move_curve = 1.0f;
    bool invert_turn = false;
    bool invert_pitch = false;
    bool invert_strafe = false;
    bool invert_forward = false;
    // The look eases toward the stick by the game's own step, as a held D-pad does.
    bool ease = true;
    // The look stops dead on release instead of the game's ease out.
    bool instant_stop = false;
    bool look_acceleration = false;
    float acceleration_max = 2.2f;
    float acceleration_time_s = 0.5f;
};
void host_set_analog_options(const AnalogOptions &options);

// The velocities the player update steers, signed as its buttons push them:
// forward is Up, strafe is R1, turn is Left, pitch is R2 (looks down).
enum class AnalogAxis : u8 { Forward, Strafe, Turn, Pitch };
// The D-pad's rates on an axis this update, in the game's units.
struct AnalogRates { s32 limit, accel; };
// The axis's new velocity from the sticks: full deflection is `limit` times the
// sensitivity (whole units, the fraction carried to the next update), the look
// axes eased from `velocity` by `accel`. Empty while the stick is centred.
// Asked once per axis per game update.
std::optional<s32> host_analog_velocity(AnalogAxis axis, s32 velocity, AnalogRates rates);
// True while the left stick drives the player, so it is not also the D-pad.
bool host_analog_owns_left_stick();

struct RawSticks { float left_x, left_y, right_x, right_y; };   // -1..1, right and down positive
// The connected gamepad's sticks, centred with no pad, no focus or the overlay open.
RawSticks host_gamepad_sticks();

} // namespace kf

#endif // KF_PLATFORM_ANALOG_H
