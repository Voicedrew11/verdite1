#include "view_smoothing.h"

#include "view_rotation.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace kf {
namespace {
constexpr std::uint64_t ns_per_ms = 1'000'000;
constexpr std::uint64_t interval_min_ns = 16 * ns_per_ms;
constexpr std::uint64_t interval_max_ns = 100 * ns_per_ms;
constexpr std::uint64_t interval_default_ns = 50 * ns_per_ms;
constexpr s32 snap_distance = 4000;          // two map tiles in one update: a warp
constexpr int snap_angle = 1024;             // a quarter turn in one update
constexpr double radians_per_angle_unit = 6.28318530717958647692 / 4096.0;
// Game lights ignore their immediate surroundings as shadow casters.
constexpr float game_light_shadow_near = 1200.0f;

using Matrix3 = std::array<std::array<float, 3>, 3>;
using Vector3 = std::array<float, 3>;

int angle_delta(int from, int to) {
    constexpr int half_turn = 0x800, wrap_mask = 0xfff;   // 4096 angle units per turn
    return ((to - from + half_turn) & wrap_mask) - half_turn;
}

Matrix3 to_float(const Rotation3 &m) {
    Matrix3 out{};
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            out[row][col] = static_cast<float>(m[row][col]);
    return out;
}

Matrix3 multiply(const Matrix3 &a, const Matrix3 &b) {
    Matrix3 out{};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            out[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c];
    return out;
}

Vector3 transform(const Matrix3 &m, const Vector3 &v) {
    return {m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2], m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
            m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2]};
}

Matrix3 transposed(const Matrix3 &m) {
    Matrix3 out{};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            out[r][c] = m[c][r];
    return out;
}

Vector3 add(const Vector3 &a, const Vector3 &b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vector3 subtract(const Vector3 &a, const Vector3 &b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vector3 scaled(const Vector3 &v, float s) { return {v[0] * s, v[1] * s, v[2] * s}; }

Motion motion_of(const Matrix3 &rotation, const Vector3 &translation) {
    Motion out{};
    for (std::size_t r = 0; r < 3; ++r) {
        out.to_shown[r * 4] = rotation[r][0];
        out.to_shown[r * 4 + 1] = rotation[r][1];
        out.to_shown[r * 4 + 2] = rotation[r][2];
        out.to_shown[r * 4 + 3] = translation[r];
    }
    return out;
}

const Motion identity_motion = motion_of({{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}}, {0, 0, 0});

Vector3 position_of(const VECTOR &v) {
    return {static_cast<float>(v.vx), static_cast<float>(v.vy), static_cast<float>(v.vz)};
}

bool is_jump(const CameraPose &from, const CameraPose &to) {
    return std::abs(to.position.vx - from.position.vx) > snap_distance ||
           std::abs(to.position.vy - from.position.vy) > snap_distance ||
           std::abs(to.position.vz - from.position.vz) > snap_distance ||
           std::abs(angle_delta(from.rotation.vy, to.rotation.vy)) > snap_angle ||
           std::abs(angle_delta(from.rotation.vx, to.rotation.vx)) > snap_angle;
}

s16 blended_angle(s16 from, s16 to, double alpha, s16 look_applied, s16 pending) {
    const double turn = angle_delta(from, to) * alpha + look_applied * (1.0 - alpha) + pending;
    return static_cast<s16>(from + static_cast<int>(std::lround(turn)));
}

void fill_camera(ShownView &view, const Projection &projection, const Matrix3 &rotation, const Vector3 &position) {
    view.projection_distance = projection.distance;
    view.center_x = static_cast<float>(projection.center_x);
    view.center_y = static_cast<float>(projection.center_y);
    const Matrix3 to_world = transposed(rotation);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            view.camera_to_world[r * 3 + c] = to_world[r][c];
    view.camera_world = position;
}

// A model part placed by to_world (model to world), from model space to the shown camera's.
Motion model_motion(const std::array<double, 12> &to_world, const Matrix3 &shown_view, const Vector3 &shown_position) {
    Matrix3 linear{};
    Vector3 position{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c)
            linear[r][c] = static_cast<float>(to_world[r * 4 + c]);
        position[r] = static_cast<float>(to_world[r * 4 + 3]);
    }
    return motion_of(multiply(shown_view, linear), transform(shown_view, subtract(position, shown_position)));
}

// The compass needle turned about its pivot on screen by turn (angle units),
// as the game turns it: its angle is the view's negated yaw.
Motion dial_motion(const DialTurn &dial, int turn) {
    const double angle = -turn * radians_per_angle_unit;
    const auto c = static_cast<float>(std::cos(angle)), s = static_cast<float>(std::sin(angle));
    const float x = dial.pivot_x, y = dial.pivot_y;
    return motion_of({{{c, -s, 0}, {s, c, 0}, {0, 0, 1}}}, {x - c * x + s * y, y - s * x - c * y, 0});
}
} // namespace

void ViewSmoothing::take_update(const std::optional<CameraPose> &camera, SVECTOR look_applied,
                                std::span<const PartMove> parts, std::uint64_t at_ns) {
    const bool jump = !camera || !current_ || at_ns - current_ns_ > interval_max_ns || is_jump(*current_, *camera);
    interval_ns_ = jump ? interval_default_ns : std::clamp(at_ns - current_ns_, interval_min_ns, interval_max_ns);
    previous_ = jump ? camera : current_;
    current_ = camera;
    look_applied_ = jump ? SVECTOR{0, 0, 0} : look_applied;
    current_ns_ = at_ns;
    blending_ = !jump;

    parts_.clear();
    parts_.reserve(parts.size());
    for (const PartMove &part : parts) {
        if (const auto *dial = std::get_if<DialTurn>(&part)) {
            parts_.emplace_back(*dial);
            continue;
        }
        const auto &model = std::get<ModelMove>(part);
        ModelStep step;
        std::array<double, 12> before{};
        std::copy(model.before.begin(), model.before.end(), before.begin());
        std::copy(model.now.begin(), model.now.end(), step.drawn.begin());
        step.before = split_transform(before);
        step.now = split_transform(step.drawn);
        parts_.emplace_back(step);
    }
}

ShownView ViewSmoothing::view_as_drawn() const {
    ShownView view;
    view.motions.assign(parts_.size() + 1, identity_motion);
    if (current_)
        fill_camera(view, current_->projection, to_float(exact_view_rotation(current_->rotation)),
                    position_of(current_->position));
    return view;
}

ShownView ViewSmoothing::view_at(std::uint64_t now_ns, SVECTOR pending_look) const {
    if (!current_ || !previous_)
        return view_as_drawn();
    const CameraPose &a = *previous_, &b = *current_;
    const double alpha =
        std::clamp(static_cast<double>(now_ns - std::min(now_ns, current_ns_)) / static_cast<double>(interval_ns_), 0.0, 1.0);
    const Vector3 drawn_position = position_of(b.position);
    const Vector3 shown_position = add(position_of(a.position), scaled(subtract(drawn_position, position_of(a.position)),
                                                                       static_cast<float>(alpha)));
    const SVECTOR shown_rotation{blended_angle(a.rotation.vx, b.rotation.vx, alpha, look_applied_.vx, pending_look.vx),
                                 blended_angle(a.rotation.vy, b.rotation.vy, alpha, look_applied_.vy, pending_look.vy),
                                 blended_angle(a.rotation.vz, b.rotation.vz, alpha, look_applied_.vz, pending_look.vz)};
    return seen_from(b, shown_position, shown_rotation, alpha);
}

ShownView ViewSmoothing::view_exact() const {
    if (!current_)
        return view_as_drawn();
    return seen_from(*current_, position_of(current_->position), current_->rotation, 1.0);
}

ShownView ViewSmoothing::seen_from(const CameraPose &b, const std::array<float, 3> &shown_position,
                                   const SVECTOR &shown_rotation, double alpha) const {
    const Matrix3 shown_view = to_float(exact_view_rotation(shown_rotation));
    // Update camera space to world relative to the shown camera: V_b^-1 c + (P_b - P_s).
    const Matrix3 to_world = to_float(game_view_inverse(b.rotation));
    const Vector3 offset = subtract(position_of(b.position), shown_position);

    ShownView view;
    view.reproject = true;
    const bool blends = blending_ && alpha < 1.0;
    view.carry = blends ? static_cast<float>(1.0 - alpha) : 0.0f;
    fill_camera(view, b.projection, shown_view, shown_position);
    view.motions.reserve(parts_.size() + 1);
    view.motions.push_back(motion_of(multiply(shown_view, to_world), transform(shown_view, offset)));
    const int turned = angle_delta(b.rotation.vy, shown_rotation.vy);
    for (const PartStep &part : parts_) {
        if (const auto *dial = std::get_if<DialTurn>(&part)) {
            view.motions.push_back(dial_motion(*dial, turned));
            continue;
        }
        const auto &model = std::get<ModelStep>(part);
        const std::array<double, 12> shown = blends ? blend_transforms(model.before, model.now, alpha) : model.drawn;
        view.motions.push_back(model_motion(shown, shown_view, shown_position));
    }
    return view;
}

std::vector<RenderLight> shown_lights(std::span<const GameLight> lights, float strength, const Motion &camera_change,
                                      std::uint64_t now_ns) {
    const double seconds = static_cast<double>(now_ns) / 1e9;
    const auto &m = camera_change.to_shown;
    std::vector<RenderLight> out;
    out.reserve(lights.size());
    for (const GameLight &light : lights) {
        // Two incommensurate sines per light read as an irregular flame flicker.
        const double phase = (light.seed % 997) * 0.37;
        const double wave = 0.5 * std::sin(seconds * 9.1 + phase) + 0.3 * std::sin(seconds * 23.7 + phase * 1.7) +
                            0.2 * std::sin(seconds * 3.3 + phase * 0.3);
        const float intensity = strength * static_cast<float>(1.0 + light.flicker * 0.18 * wave);
        const float x = m[0] * light.x + m[1] * light.y + m[2] * light.z + m[3];
        const float y = m[4] * light.x + m[5] * light.y + m[6] * light.z + m[7];
        const float z = m[8] * light.x + m[9] * light.y + m[10] * light.z + m[11];
        out.push_back({x, y, z, light.radius, light.r * intensity, light.g * intensity, light.b * intensity,
                       game_light_shadow_near});
    }
    // The backend gives shadow maps to the leading lights.
    std::sort(out.begin(), out.end(), [](const RenderLight &a, const RenderLight &b) {
        return a.x * a.x + a.y * a.y + a.z * a.z < b.x * b.x + b.y * b.y + b.z * b.z;
    });
    return out;
}
} // namespace kf
