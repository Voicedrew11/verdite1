#include <verdite/stick.h>

#include <algorithm>
#include <cmath>

namespace verdite {
namespace {

// A stall longer than this counts as this long, so the ramp does not jump.
constexpr float longest_update_s = 0.1f;
constexpr float ramp_fall_rate = 3.0f;

// 0 at the deadzone's edge, 1 at full deflection, then curved.
float rescale(float magnitude, StickShape shape) {
    if (!std::isfinite(magnitude) || magnitude <= shape.deadzone)
        return 0.0f;
    const float unit = std::clamp((magnitude - shape.deadzone) / (1.0f - shape.deadzone), 0.0f, 1.0f);
    return std::pow(unit, shape.curve);
}

} // namespace

StickAxes shape_stick(StickAxes raw, StickShape shape) {
    const float magnitude = std::hypot(raw.x, raw.y);
    const float shaped = rescale(magnitude, shape);
    if (shaped == 0.0f)
        return {};
    const float scale = shaped / magnitude;
    return {raw.x * scale, raw.y * scale};
}

float shape_axis(float raw, StickShape shape) {
    const float shaped = rescale(std::fabs(raw), shape);
    return raw < 0.0f ? -shaped : shaped;
}

float LookAcceleration::update(float magnitude, float elapsed_s, LookAccelerationShape shape) {
    const float dt = std::isfinite(elapsed_s) ? std::clamp(elapsed_s, 0.0f, longest_update_s) : 0.0f;
    const float ramp_s = std::max(shape.ramp_s, 0.0f);
    held_s_ += magnitude >= shape.threshold ? dt : -dt * ramp_fall_rate;
    held_s_ = std::clamp(held_s_, 0.0f, ramp_s);
    const float t = ramp_s == 0.0f ? (magnitude >= shape.threshold ? 1.0f : 0.0f) : held_s_ / ramp_s;
    return 1.0f + (shape.max_multiplier - 1.0f) * t;
}

void LookAcceleration::reset() { held_s_ = 0.0f; }

int StepCarry::take(float wanted, int limit) {
    if (!std::isfinite(wanted))
        wanted = 0.0f;
    const float total = wanted + carry_;
    const float whole = std::clamp(std::trunc(total), static_cast<float>(-limit), static_cast<float>(limit));
    carry_ = std::fabs(whole) == static_cast<float>(limit) ? 0.0f : total - whole;
    return static_cast<int>(whole);
}

void StepCarry::reset() { carry_ = 0.0f; }

} // namespace verdite
