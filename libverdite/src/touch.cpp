#include <verdite/touch.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace verdite {
namespace {

// A tap is a touch lifted this soon, this near (of the height) where it landed.
constexpr std::uint64_t tap_ms = 300;
constexpr float tap_slop = 0.05f;
// A D-pad's centre, as a share of its radius, points nowhere.
constexpr float dpad_dead = 0.3f;

// The eight ways from east, a quarter turn at a time clockwise (y is down).
constexpr std::array<StickAxes, 8> dpad_ways = {
    StickAxes{1, 0}, StickAxes{1, 1}, StickAxes{0, 1}, StickAxes{-1, 1},
    StickAxes{-1, 0}, StickAxes{-1, -1}, StickAxes{0, -1}, StickAxes{1, -1}};

bool holds_one_finger(TouchKind kind) { return kind != TouchKind::Button; }

} // namespace

void TouchControls::set_safe_area(float x_px, float y_px, float width_px, float height_px) {
    area_x_px_ = x_px;
    area_y_px_ = y_px;
    area_width_px_ = std::max(width_px, 1.0f);
    area_height_px_ = std::max(height_px, 1.0f);
}

void TouchControls::set_layout(std::span<const TouchControl> controls) {
    controls_.assign(controls.begin(), controls.end());
    states_.assign(controls_.size(), TouchState{});
    frame_.clear();
    fingers_.clear();
}

void TouchControls::finger_down(std::uint64_t finger, float x_px, float y_px, std::uint64_t at_ms) {
    std::erase_if(fingers_, [finger](const Finger &each) { return each.id == finger; });
    const auto control = control_at(x_px, y_px);
    if (!control)
        return;
    fingers_.push_back({finger, *control, x_px, y_px, x_px, y_px, at_ms, false});
    TouchState &state = states_[*control];
    state.held = true;
    if (controls_[*control].kind == TouchKind::Stick) {
        state.stick = {};
        state.centre_x = (x_px - area_x_px_) / area_width_px_;
        state.centre_y = (y_px - area_y_px_) / area_height_px_;
    }
    aim(fingers_.back());
}

void TouchControls::finger_moved(std::uint64_t finger, float x_px, float y_px) {
    const auto it = std::find_if(fingers_.begin(), fingers_.end(), [finger](const Finger &each) { return each.id == finger; });
    if (it == fingers_.end())
        return;
    TouchState &state = states_[it->control];
    if (controls_[it->control].kind == TouchKind::Drag) {
        state.drag_x_px += x_px - it->last_x_px;
        state.drag_y_px += y_px - it->last_y_px;
    }
    it->last_x_px = x_px;
    it->last_y_px = y_px;
    if (std::hypot(x_px - it->down_x_px, y_px - it->down_y_px) > tap_slop * area_height_px_)
        it->strayed = true;
    aim(*it);
}

void TouchControls::finger_up(std::uint64_t finger, std::uint64_t at_ms) {
    const auto it = std::find_if(fingers_.begin(), fingers_.end(), [finger](const Finger &each) { return each.id == finger; });
    if (it == fingers_.end())
        return;
    const std::size_t control = it->control;
    if (controls_[control].kind == TouchKind::Tap && !it->strayed && at_ms - it->down_ms <= tap_ms)
        states_[control].tapped = true;
    fingers_.erase(it);
    refresh_held(control);
}

void TouchControls::release_all() {
    fingers_.clear();
    std::fill(states_.begin(), states_.end(), TouchState{});
}

std::span<const TouchState> TouchControls::take_frame() {
    frame_ = states_;
    for (TouchState &state : states_) {
        state.drag_x_px = state.drag_y_px = 0;
        state.tapped = false;
    }
    return frame_;
}

bool TouchControls::any_held() const { return !fingers_.empty(); }

std::optional<std::size_t> TouchControls::control_at(float x_px, float y_px) const {
    const float x = (x_px - area_x_px_) / area_width_px_;
    const float y = (y_px - area_y_px_) / area_height_px_;
    for (std::size_t i = 0; i < controls_.size(); ++i) {
        const TouchArea &zone = controls_[i].zone;
        const bool inside = x >= zone.x && x < zone.x + zone.width && y >= zone.y && y < zone.y + zone.height;
        if (inside && !(holds_one_finger(controls_[i].kind) && states_[i].held))
            return i;
    }
    return std::nullopt;
}

// A stick's throw from where it was centred, or a D-pad's way from its middle.
void TouchControls::aim(const Finger &finger) {
    const TouchControl &control = controls_[finger.control];
    TouchState &state = states_[finger.control];
    const float radius_px = std::max(control.radius * area_height_px_, 1.0f);
    if (control.kind == TouchKind::Stick) {
        const float x = (finger.last_x_px - finger.down_x_px) / radius_px;
        const float y = (finger.last_y_px - finger.down_y_px) / radius_px;
        const float length = std::hypot(x, y);
        const float scale = length > 1.0f ? 1.0f / length : 1.0f;
        state.stick = {x * scale, y * scale};
    } else if (control.kind == TouchKind::Dpad) {
        const float centre_x = area_x_px_ + (control.zone.x + control.zone.width / 2) * area_width_px_;
        const float centre_y = area_y_px_ + (control.zone.y + control.zone.height / 2) * area_height_px_;
        const float x = finger.last_x_px - centre_x;
        const float y = finger.last_y_px - centre_y;
        if (std::hypot(x, y) < dpad_dead * radius_px) {
            state.stick = {};
            return;
        }
        const auto eighth = static_cast<long>(std::lround(std::atan2(y, x) / (std::numbers::pi_v<float> / 4)));
        state.stick = dpad_ways[static_cast<std::size_t>((eighth % 8 + 8) % 8)];
    }
}

void TouchControls::refresh_held(std::size_t control) {
    TouchState &state = states_[control];
    state.held = std::any_of(fingers_.begin(), fingers_.end(), [control](const Finger &each) { return each.control == control; });
    if (!state.held)
        state.stick = {};
}

} // namespace verdite
