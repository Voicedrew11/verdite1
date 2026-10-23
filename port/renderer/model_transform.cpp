#include "model_transform.h"

#include <algorithm>
#include <cmath>

namespace kf {
namespace {
using Vector3 = std::array<double, 3>;
using Quaternion = std::array<double, 4>;
// A column shorter than this is a model axis scaled to nothing.
constexpr double degenerate_length = 1e-6;

double dot(const Vector3 &a, const Vector3 &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vector3 cross(const Vector3 &a, const Vector3 &b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
Vector3 scaled(const Vector3 &v, double s) { return {v[0] * s, v[1] * s, v[2] * s}; }

Vector3 column(std::span<const double, 12> matrix, int c) {
    return {matrix[c], matrix[4 + c], matrix[8 + c]};
}

// The rotation's columns from the scaled ones: an axis scaled to nothing takes
// the direction its neighbours imply.
std::array<Vector3, 3> unit_columns(std::array<Vector3, 3> columns, Vector3 &scale) {
    std::array<bool, 3> degenerate{};
    for (int c = 0; c < 3; ++c) {
        scale[c] = std::sqrt(dot(columns[c], columns[c]));
        degenerate[c] = scale[c] < degenerate_length;
        if (!degenerate[c])
            columns[c] = scaled(columns[c], 1.0 / scale[c]);
    }
    const auto count = std::count(degenerate.begin(), degenerate.end(), true);
    if (count > 1)
        return {Vector3{1, 0, 0}, Vector3{0, 1, 0}, Vector3{0, 0, 1}};
    for (int c = 0; c < 3; ++c) {
        if (degenerate[c]) {
            columns[c] = cross(columns[(c + 1) % 3], columns[(c + 2) % 3]);
            scale[c] = 0;
        }
    }
    // A mirrored model keeps a proper rotation with a negative scale.
    if (dot(cross(columns[0], columns[1]), columns[2]) < 0) {
        columns[0] = scaled(columns[0], -1);
        scale[0] = -scale[0];
    }
    return columns;
}

// Shepperd's method, from the rotation's columns.
Quaternion quaternion_of(const std::array<Vector3, 3> &c) {
    const double m00 = c[0][0], m11 = c[1][1], m22 = c[2][2];
    const double m01 = c[1][0], m10 = c[0][1], m02 = c[2][0], m20 = c[0][2], m12 = c[2][1], m21 = c[1][2];
    const double trace = m00 + m11 + m22;
    Quaternion q{};
    if (trace > 0) {
        const double s = std::sqrt(trace + 1.0) * 2;
        q = {0.25 * s, (m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s};
    } else if (m00 > m11 && m00 > m22) {
        const double s = std::sqrt(1.0 + m00 - m11 - m22) * 2;
        q = {(m21 - m12) / s, 0.25 * s, (m01 + m10) / s, (m02 + m20) / s};
    } else if (m11 > m22) {
        const double s = std::sqrt(1.0 + m11 - m00 - m22) * 2;
        q = {(m02 - m20) / s, (m01 + m10) / s, 0.25 * s, (m12 + m21) / s};
    } else {
        const double s = std::sqrt(1.0 + m22 - m00 - m11) * 2;
        q = {(m10 - m01) / s, (m02 + m20) / s, (m12 + m21) / s, 0.25 * s};
    }
    const double length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (double &part : q)
        part /= length;
    return q;
}

double quaternion_dot(const Quaternion &a, const Quaternion &b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
}

Quaternion slerp(const Quaternion &from, Quaternion to, double alpha) {
    double cosine = quaternion_dot(from, to);
    if (cosine < 0) {
        for (double &part : to)
            part = -part;
        cosine = -cosine;
    }
    double a = 1 - alpha, b = alpha;
    // Nearly the same rotation: a straight blend is exact enough and stable.
    if (cosine < 0.9995) {
        const double angle = std::acos(cosine);
        const double sine = std::sin(angle);
        a = std::sin((1 - alpha) * angle) / sine;
        b = std::sin(alpha * angle) / sine;
    }
    Quaternion out{};
    for (int i = 0; i < 4; ++i)
        out[i] = from[i] * a + to[i] * b;
    const double length = std::sqrt(quaternion_dot(out, out));
    for (double &part : out)
        part /= length;
    return out;
}
} // namespace

ModelTransform split_transform(std::span<const double, 12> matrix) {
    ModelTransform out;
    const std::array<Vector3, 3> rotation =
        unit_columns({column(matrix, 0), column(matrix, 1), column(matrix, 2)}, out.scale);
    out.rotation = quaternion_of(rotation);
    out.position = {matrix[3], matrix[7], matrix[11]};
    return out;
}

std::array<double, 12> blend_transforms(const ModelTransform &from, const ModelTransform &to, double alpha) {
    const auto [w, x, y, z] = slerp(from.rotation, to.rotation, alpha);
    const std::array<std::array<double, 3>, 3> rotation = {{
        {1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)},
        {2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)},
        {2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)},
    }};
    std::array<double, 12> out{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c)
            out[r * 4 + c] = rotation[r][c] * (from.scale[c] + (to.scale[c] - from.scale[c]) * alpha);
        out[r * 4 + 3] = from.position[r] + (to.position[r] - from.position[r]) * alpha;
    }
    return out;
}

double turn_between(const ModelTransform &from, const ModelTransform &to) {
    const double cosine = std::min(1.0, std::abs(quaternion_dot(from.rotation, to.rotation)));
    return 2 * std::acos(cosine);
}
} // namespace kf
