#ifndef VERDITE_STICK_H
#define VERDITE_STICK_H
// Analog sticks shaped for a game that steers by per-update velocities: the
// deadzones, response curves and look acceleration a player tunes, and the
// carrying that keeps a small deflection from rounding to nothing at a low
// update rate. Game-agnostic: the game hands in raw axes and its own rates.

namespace verdite {

// One stick, each axis -1..1 (right and down positive, as SDL reports them).
struct StickAxes {
    float x = 0.0f;
    float y = 0.0f;
};

struct StickShape {
    float deadzone = 0.15f;   // of full deflection; inside it the stick is centred
    float curve = 1.0f;       // response exponent: 1 linear, above 1 finer near the centre
};

// Radial deadzone and curve: the direction kept, the magnitude rescaled so the
// edge of the deadzone is 0 and full deflection is 1.
StickAxes shape_stick(StickAxes raw, StickShape shape);

// One axis on its own deadzone and curve, for a stick whose axes do different
// jobs (walk and turn on one stick): a thumb a little off forward walks straight.
float shape_axis(float raw, StickShape shape);

// The look ramp: a multiplier that rises from 1 to max_multiplier over ramp_s
// while the stick is held past threshold, and falls three times as fast.
struct LookAccelerationShape {
    float max_multiplier = 2.2f;
    float ramp_s = 0.5f;
    float threshold = 0.8f;   // shaped magnitude
};

class LookAcceleration {
public:
    // Once a game update; elapsed_s is clamped, so a stall does not jump the ramp.
    float update(float magnitude, float elapsed_s, LookAccelerationShape shape);
    void reset();

private:
    float held_s_ = 0.0f;
};

// A wanted per-update amount as whole units, the fraction carried to the next
// update: at 15 updates a second a third of the turn rate would otherwise lose
// a third of itself to rounding.
class StepCarry {
public:
    int take(float wanted, int limit);
    void reset();

private:
    float carry_ = 0.0f;
};

} // namespace verdite

#endif // VERDITE_STICK_H
