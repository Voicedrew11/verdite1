#include "view_rotation.h"

#include <kf/lib/fixed_math.h>

#include <cmath>

namespace kf {
namespace {
constexpr double radians_per_angle_unit = 6.28318530717958647692 / 4096.0;
constexpr double fixed_one = 4096.0;
} // namespace

Rotation3 exact_view_rotation(const SVECTOR &angles) {
    const double x = angles.vx * radians_per_angle_unit, y = angles.vy * radians_per_angle_unit,
                 z = angles.vz * radians_per_angle_unit;
    const double cx = std::cos(x), cy = std::cos(y), cz = std::cos(z);
    const double sx = std::sin(x), sy = std::sin(y), sz = std::sin(z);
    return {{{cz * cy, -sz * cy, sy},
             {sz * cx + cz * sy * sx, cz * cx - sz * sy * sx, -cy * sx},
             {sz * sx - cz * sy * cx, cz * sx + sz * sy * cx, cy * cx}}};
}

Rotation3 game_view_inverse(const SVECTOR &angles) {
    MATRIX fixed{};
    matrix_set_rotation_xyz(angles, fixed);
    Rotation3 m{};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            m[r][c] = fixed.m[r][c] / fixed_one;
    // The adjugate over the determinant; the matrix is within a few 4096ths of a rotation.
    const double determinant = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                               m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                               m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    Rotation3 out{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            const int c1 = (c + 1) % 3, c2 = (c + 2) % 3, r1 = (r + 1) % 3, r2 = (r + 2) % 3;
            out[r][c] = (m[c1][r1] * m[c2][r2] - m[c1][r2] * m[c2][r1]) / determinant;
        }
    }
    return out;
}
} // namespace kf
