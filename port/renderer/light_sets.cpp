#include "light_sets.h"

#include <array>
#include <cmath>
#include <optional>
#include <variant>

namespace kf {
namespace {
constexpr float matrix_unity = 4096.0f;
// A model scaled to nothing has no rotation to undo.
constexpr float smallest_determinant = 1e-9f;

using Matrix3 = std::array<std::array<float, 3>, 3>;
constexpr Matrix3 identity{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};

Matrix3 from_game(const MATRIX &m) {
    Matrix3 out{};
    for (std::size_t r = 0; r < 3; ++r)
        for (std::size_t c = 0; c < 3; ++c)
            out[r][c] = static_cast<float>(m.m[r][c]) / matrix_unity;
    return out;
}

Matrix3 linear_part(const Motion &motion) {
    Matrix3 out{};
    for (std::size_t r = 0; r < 3; ++r)
        for (std::size_t c = 0; c < 3; ++c)
            out[r][c] = motion.to_shown[r * 4 + c];
    return out;
}

Matrix3 multiply(const Matrix3 &a, const Matrix3 &b) {
    Matrix3 out{};
    for (std::size_t r = 0; r < 3; ++r)
        for (std::size_t c = 0; c < 3; ++c)
            out[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c];
    return out;
}

std::optional<Matrix3> inverse(const Matrix3 &m) {
    const float c00 = m[1][1] * m[2][2] - m[1][2] * m[2][1];
    const float c01 = m[1][2] * m[2][0] - m[1][0] * m[2][2];
    const float c02 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
    const float determinant = m[0][0] * c00 + m[0][1] * c01 + m[0][2] * c02;
    if (!std::isfinite(determinant) || std::abs(determinant) < smallest_determinant)
        return std::nullopt;
    const float s = 1.0f / determinant;
    return Matrix3{{{c00 * s, (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * s, (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * s},
                    {c01 * s, (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * s, (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * s},
                    {c02 * s, (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * s, (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * s}}};
}

std::array<float, 9> flattened(const Matrix3 &m) {
    return {m[0][0], m[0][1], m[0][2], m[1][0], m[1][1], m[1][2], m[2][0], m[2][1], m[2][2]};
}

// The light dots of a model drawn by `drawn` (model to the update's camera,
// as moved to the shown camera) and shown by `shown` (model to the shown
// camera): the game's dots, turned by what the model turned in between.
Matrix3 turned_lights(const Matrix3 &lights, const Matrix3 &drawn, const Matrix3 &shown) {
    const std::optional<Matrix3> undo = inverse(drawn);
    return undo ? multiply(lights, multiply(*undo, shown)) : lights;
}
} // namespace

std::vector<u16> light_set_parts(std::size_t set_count, std::span<const DrawFace> faces,
                                 std::span<const PartMove> parts) {
    std::vector<u16> out(set_count, 0);
    for (const DrawFace &face : faces) {
        if (face.light_set == 0 || face.light_set > set_count || face.part == 0 || face.part > parts.size())
            continue;
        if (std::holds_alternative<ModelMove>(parts[face.part - 1]))
            out[face.light_set - 1] = face.part;
    }
    return out;
}

std::vector<ShownLightSet> shown_light_sets(std::span<const LightSet> sets, std::span<const u16> parts,
                                            std::span<const Motion> motions) {
    std::vector<ShownLightSet> out;
    out.reserve(sets.size());
    const Matrix3 camera_change = motions.empty() ? identity : linear_part(motions.front());
    for (std::size_t i = 0; i < sets.size(); ++i) {
        const LightSet &set = sets[i];
        const Matrix3 lights = from_game(set.lights);
        const Matrix3 drawn = multiply(camera_change, from_game(set.model));
        const u16 part = i < parts.size() ? parts[i] : 0;
        const bool carried = part != 0 && part < motions.size();
        const Matrix3 shown = carried ? linear_part(motions[part]) : drawn;
        ShownLightSet shown_set{};
        shown_set.dots_from_model = flattened(carried ? turned_lights(lights, drawn, shown) : lights);
        shown_set.color_matrix = flattened(from_game(set.color_matrix));
        shown_set.ambient = {static_cast<float>(set.ambient.r), static_cast<float>(set.ambient.g),
                             static_cast<float>(set.ambient.b)};
        shown_set.normal_to_camera = flattened(shown);
        out.push_back(shown_set);
    }
    return out;
}
} // namespace kf
