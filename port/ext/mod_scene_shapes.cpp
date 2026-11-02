#include "mod_scene_shapes.h"

#include <kf/renderer/renderer.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace kf::ext {
namespace {
// Corners sit at most this far from the view on each axis, and fit an SVECTOR with room to spare.
constexpr double reach = 16000;
// Corners are rounded to whole units, and the game's integer transform may differ by a unit: the
// clip keeps this margin past the renderer's near plane.
constexpr double near_margin = 4;
constexpr double near_limit = render_near_depth + near_margin;
constexpr double rotation_unit = 4096;

using Vec3 = std::array<double, 3>;

Vec3 operator+(const Vec3 &a, const Vec3 &b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 operator-(const Vec3 &a, const Vec3 &b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 operator*(const Vec3 &a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }

double dot(const Vec3 &a, const Vec3 &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

Vec3 cross(const Vec3 &a, const Vec3 &b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

// Row i of the view rotation: the world direction of camera axis i (the matrix is orthonormal).
Vec3 axis(const SceneView &view, int row) {
    const auto &m = view.rotation.m[static_cast<std::size_t>(row)];
    return {m[0] / rotation_unit, m[1] / rotation_unit, m[2] / rotation_unit};
}

// A world point relative to the view position: the 64-bit difference is exact in a double.
Vec3 relative(const SceneView &view, const WorldPoint &point) {
    return {static_cast<double>(static_cast<std::int64_t>(point[0]) - view.position.vx),
            static_cast<double>(static_cast<std::int64_t>(point[1]) - view.position.vy),
            static_cast<double>(static_cast<std::int64_t>(point[2]) - view.position.vz)};
}

// The part of the segment, as parameters 0..1 along it, that is still open.
struct Span {
    double lo = 0, hi = 1;
    [[nodiscard]] bool empty() const { return !(lo < hi); }
};

// Keeps the part of the segment a + (b - a) t where that linear value is at least least.
void clip_at_least(Span &span, double a, double b, double least) {
    const double slope = b - a;
    if (slope == 0) {
        if (a < least)
            span.hi = span.lo;
        return;
    }
    const double at = (least - a) / slope;
    if (slope > 0)
        span.lo = std::max(span.lo, at);
    else
        span.hi = std::min(span.hi, at);
}

// Keeps the part of the segment where the value stays within limit either side of zero.
void clip_within(Span &span, double a, double b, double limit) {
    clip_at_least(span, a, b, -limit);
    clip_at_least(span, -a, -b, -limit);
}

Vec3 along(const Vec3 &a, const Vec3 &b, double t) { return a + (b - a) * t; }

double depth_of(const SceneView &view, const Vec3 &relative_point) {
    return dot(axis(view, 2), relative_point);
}

// Rounds a corner to the game's units, or none if it lies past the reach or the renderer's near plane.
std::optional<ViewOffset> whole_offset(const SceneView &view, const Vec3 &point) {
    ViewOffset out{};
    for (std::size_t i = 0; i < 3; ++i) {
        const double rounded = std::round(point[i]);
        if (!(std::abs(rounded) <= reach))
            return std::nullopt;
        out[i] = static_cast<s16>(rounded);
    }
    const Vec3 whole{static_cast<double>(out[0]), static_cast<double>(out[1]), static_cast<double>(out[2])};
    if (!(depth_of(view, whole) > render_near_depth))
        return std::nullopt;
    return out;
}

std::optional<SceneQuad> quad_of(const SceneView &view, const std::array<Vec3, 4> &corners, const Vec3 &centre) {
    SceneQuad quad;
    for (std::size_t i = 0; i < 4; ++i) {
        const auto offset = whole_offset(view, corners[i]);
        if (!offset)
            return std::nullopt;
        quad.corners[i] = *offset;
    }
    const auto middle = whole_offset(view, centre);
    if (!middle)
        return std::nullopt;
    quad.centre = *middle;
    return quad;
}

// The line as a quad, turned about its own axis so that it faces the camera: its sides are across the
// line and across the view direction, so they lie in the plane perpendicular to the view and keep
// each end's depth.
std::optional<SceneQuad> line_quad(const SceneView &view, const SceneLine &line) {
    const double half = line.width / 2;
    const double bound = reach - half - 1;   // a side adds at most half to each coordinate
    const Vec3 a = relative(view, line.from), b = relative(view, line.to);
    Span span;
    for (std::size_t i = 0; i < 3; ++i)
        clip_within(span, a[i], b[i], bound);
    clip_at_least(span, depth_of(view, a), depth_of(view, b), near_limit);
    if (span.empty())
        return std::nullopt;
    const Vec3 from = along(a, b, span.lo), to = along(a, b, span.hi);
    // Both ends rounded to the same unit: nothing to draw.
    const auto near_end = whole_offset(view, from), far_end = whole_offset(view, to);
    if (!near_end || !far_end || *near_end == *far_end)
        return std::nullopt;
    const Vec3 forward = axis(view, 2);
    Vec3 side = cross(to - from, forward);
    if (dot(side, side) == 0)
        side = axis(view, 0);   // the line points straight at the camera: any side across the view will do
    side = side * (half / std::sqrt(dot(side, side)));
    const std::array<Vec3, 4> corners = {from - side, from + side, to - side, to + side};
    return quad_of(view, corners, (from + to) * 0.5);
}

std::optional<SceneQuad> marker_quad(const SceneView &view, const SceneMarker &marker) {
    const double half = marker.size / 2;
    const double bound = reach - half * std::sqrt(2.0) - 1;   // the corners lie within the square's diagonal
    const Vec3 centre = relative(view, marker.centre);
    for (std::size_t i = 0; i < 3; ++i) {
        if (!(std::abs(centre[i]) <= bound))
            return std::nullopt;
    }
    if (!(depth_of(view, centre) >= near_limit))
        return std::nullopt;
    const Vec3 right = axis(view, 0) * half, up = axis(view, 1) * half;
    const std::array<Vec3, 4> corners = {centre - right - up, centre + right - up, centre - right + up,
                                         centre + right + up};
    return quad_of(view, corners, centre);
}
} // namespace

std::optional<SceneQuad> scene_quad(const SceneView &view, const SceneShape &shape) {
    if (const auto *line = std::get_if<SceneLine>(&shape))
        return line_quad(view, *line);
    return marker_quad(view, std::get<SceneMarker>(shape));
}

} // namespace kf::ext
