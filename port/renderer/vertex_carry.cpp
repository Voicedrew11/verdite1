#include "vertex_carry.h"

#include "model_transform.h"

#include <kf/lib/fixed_math.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace kf {
namespace {
// Farther in one update is a warp or a new occupant, as Verdite's model carry judged.
constexpr double step_limit = 2000;
constexpr double turn_limit = 0x300 * 6.28318530717958647692 / 4096;   // angle units to radians
constexpr double fixed_one = 4096;

// The model-to-world transform of a model the game projects with model (its
// model-to-camera transform) from camera: the view's transpose applied to it.
std::array<double, 12> model_to_world(const CameraPose &camera, const MATRIX &model) {
    MATRIX view{};
    matrix_set_rotation_xyz(camera.rotation, view);
    const std::array<double, 3> eye = {static_cast<double>(camera.position.vx), static_cast<double>(camera.position.vy),
                                       static_cast<double>(camera.position.vz)};
    std::array<double, 12> out{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            double sum = 0;
            for (int k = 0; k < 3; ++k)
                sum += view.m[k][r] * static_cast<double>(model.m[k][c]);
            out[r * 4 + c] = sum / (fixed_one * fixed_one);
        }
        double moved = 0;
        for (int k = 0; k < 3; ++k)
            moved += view.m[k][r] * static_cast<double>(model.t[k]);
        out[r * 4 + 3] = moved / fixed_one + eye[r];
    }
    return out;
}

bool within_one_step(const std::array<double, 12> &before, const std::array<double, 12> &now) {
    for (const int r : {0, 1, 2})
        if (std::abs(now[r * 4 + 3] - before[r * 4 + 3]) > step_limit)
            return false;
    return turn_between(split_transform(before), split_transform(now)) <= turn_limit;
}

std::array<float, 12> to_float(const std::array<double, 12> &m) {
    std::array<float, 12> out{};
    std::transform(m.begin(), m.end(), out.begin(), [](double v) { return static_cast<float>(v); });
    return out;
}
} // namespace

void VertexCarry::begin_update() {
    if (current_.camera)
        previous_ = std::move(current_);
    current_ = Update{};
}

void VertexCarry::seen_from(const CameraPose &camera) { current_.camera = camera; }

u32 VertexCarry::next_key(u16 tag) {
    u16 &drawn = current_.drawn[tag];
    const u32 key = static_cast<u32>(tag) << 16 | drawn;
    drawn = static_cast<u16>(drawn + 1);
    return key;
}

const VertexCarry::Drawing *VertexCarry::drawn_before(u32 key, u32 identity) const {
    const auto found = previous_.drawings.find(key);
    if (found == previous_.drawings.end() || found->second.identity != identity)
        return nullptr;
    return &found->second;
}

CarriedModel VertexCarry::carry_model(PartOwner owner, const MATRIX &model, std::span<const SVECTOR> vertices) {
    if (owner.tag == 0 || !current_.camera)
        return {};
    const u32 key = next_key(owner.tag);
    Drawing drawing;
    drawing.identity = owner.identity;
    drawing.to_world = model_to_world(*current_.camera, model);
    drawing.vertices.assign(vertices.begin(), vertices.end());
    const Drawing *before = drawn_before(key, owner.identity);
    CarriedModel out;
    if (before && !vertices.empty() && before->vertices.size() == vertices.size() &&
        current_.parts.size() < std::numeric_limits<u16>::max() && within_one_step(before->to_world, drawing.to_world)) {
        model_back_.resize(vertices.size());
        for (std::size_t i = 0; i < vertices.size(); ++i) {
            const SVECTOR &from = before->vertices[i], &to = vertices[i];
            model_back_[i] = {static_cast<float>(from.vx - to.vx), static_cast<float>(from.vy - to.vy),
                              static_cast<float>(from.vz - to.vz)};
        }
        current_.parts.emplace_back(ModelMove{to_float(before->to_world), to_float(drawing.to_world)});
        out = {static_cast<u16>(current_.parts.size()), model_back_};
    }
    current_.drawings.insert_or_assign(key, std::move(drawing));
    return out;
}

std::span<const std::array<float, 2>> VertexCarry::carry_screen(PartOwner owner,
                                                                 std::span<const std::array<float, 2>> points) {
    if (owner.tag == 0 || !current_.camera)
        return {};
    const u32 key = next_key(owner.tag);
    Drawing drawing;
    drawing.identity = owner.identity;
    drawing.points.assign(points.begin(), points.end());
    const Drawing *before = drawn_before(key, owner.identity);
    std::span<const std::array<float, 2>> out;
    if (before && !points.empty() && before->points.size() == points.size()) {
        screen_back_.resize(points.size());
        for (std::size_t i = 0; i < points.size(); ++i)
            screen_back_[i] = {before->points[i][0] - points[i][0], before->points[i][1] - points[i][1]};
        out = screen_back_;
    }
    current_.drawings.insert_or_assign(key, std::move(drawing));
    return out;
}

u16 VertexCarry::add_dial(float pivot_x, float pivot_y) {
    if (!current_.camera || current_.parts.size() >= std::numeric_limits<u16>::max())
        return 0;
    current_.parts.emplace_back(DialTurn{pivot_x, pivot_y});
    return static_cast<u16>(current_.parts.size());
}

std::span<const PartMove> VertexCarry::parts() const { return current_.parts; }
} // namespace kf
