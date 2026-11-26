#include "corner_occlusion.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace kf::ext {
namespace {

Point3 operator+(Point3 a, Point3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Point3 operator-(Point3 a, Point3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Point3 operator*(Point3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(Point3 a, Point3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Point3 cross(Point3 a, Point3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

// Off the face by more than the 4 units neighbouring cells' faces overlap, and in from the corner's edges.
constexpr float lift = 8.0f;
constexpr float pull = 16.0f;
constexpr float nearest_hit = 0.5f;
constexpr std::size_t ray_count = 32;

// Cosine-weighted directions about +z, spread on a golden-angle spiral, so a plain average is the
// cosine-weighted occlusion.
const std::array<Point3, ray_count> &hemisphere_rays() {
    static const std::array<Point3, ray_count> rays = [] {
        std::array<Point3, ray_count> out{};
        const float golden_angle = 3.14159265f * (3.0f - std::sqrt(5.0f));
        for (std::size_t i = 0; i < ray_count; ++i) {
            const float share = (static_cast<float>(i) + 0.5f) / static_cast<float>(ray_count);
            const float radius = std::sqrt(share);
            const float angle = golden_angle * static_cast<float>(i);
            out[i] = {radius * std::cos(angle), radius * std::sin(angle), std::sqrt(1.0f - share)};
        }
        return out;
    }();
    return rays;
}

// Distance along direction to the triangle, either side facing; reach or more when it is missed.
float hit_distance(Point3 origin, Point3 direction, const OccluderTriangle &t, float reach) {
    const Point3 p = cross(direction, t.edge2);
    const float determinant = dot(t.edge1, p);
    if (std::fabs(determinant) < 1e-6f)
        return reach;
    const float inverse = 1.0f / determinant;
    const Point3 s = origin - t.origin;
    const float u = dot(s, p) * inverse;
    if (u < 0.0f || u > 1.0f)
        return reach;
    const Point3 q = cross(s, t.edge1);
    const float v = dot(direction, q) * inverse;
    if (v < 0.0f || u + v > 1.0f)
        return reach;
    const float distance = dot(t.edge2, q) * inverse;
    return distance >= nearest_hit ? distance : reach;
}

float box_distance(Point3 point, const OccluderTriangle &t) {
    const Point3 outside{std::max({t.low.x - point.x, 0.0f, point.x - t.high.x}),
                         std::max({t.low.y - point.y, 0.0f, point.y - t.high.y}),
                         std::max({t.low.z - point.z, 0.0f, point.z - t.high.z})};
    return std::sqrt(dot(outside, outside));
}

// Some of the triangle lies above the plane through origin.
bool above(Point3 origin, Point3 normal, const OccluderTriangle &t) {
    const float a = dot(t.origin - origin, normal);
    return a > 0.0f || a + dot(t.edge1, normal) > 0.0f || a + dot(t.edge2, normal) > 0.0f;
}

} // namespace

OccluderTriangle occluder_triangle(Point3 a, Point3 b, Point3 c) {
    return {a, b - a, c - a, {std::min({a.x, b.x, c.x}), std::min({a.y, b.y, c.y}), std::min({a.z, b.z, c.z})},
            {std::max({a.x, b.x, c.x}), std::max({a.y, b.y, c.y}), std::max({a.z, b.z, c.z})}};
}

float corner_occlusion(Point3 corner, Point3 normal, Point3 inside, std::span<const OccluderTriangle> triangles,
                       float reach) {
    const Point3 toward = inside - corner;
    const float toward_length = std::sqrt(dot(toward, toward));
    Point3 origin = corner + normal * lift;
    if (toward_length > 0.0f)
        origin = origin + toward * (std::min(pull, toward_length * 0.5f) / toward_length);

    // Reused between calls on each thread, so a cell's corners allocate nothing after the first.
    thread_local std::vector<const OccluderTriangle *> candidates;
    candidates.clear();
    for (const OccluderTriangle &t : triangles)
        if (box_distance(origin, t) < reach && above(origin, normal, t))
            candidates.push_back(&t);
    if (candidates.empty())
        return 0.0f;

    // A frame about the normal: any axis not along it gives the first tangent.
    const Point3 axis = std::fabs(normal.x) < 0.9f ? Point3{1, 0, 0} : Point3{0, 1, 0};
    Point3 tangent = cross(axis, normal);
    tangent = tangent * (1.0f / std::sqrt(dot(tangent, tangent)));
    const Point3 bitangent = cross(normal, tangent);

    float blocked = 0.0f;
    for (const Point3 &ray : hemisphere_rays()) {
        const Point3 direction = tangent * ray.x + bitangent * ray.y + normal * ray.z;
        float nearest = reach;
        for (const OccluderTriangle *t : candidates)
            nearest = std::min(nearest, hit_distance(origin, direction, *t, nearest));
        blocked += 1.0f - nearest / reach;
    }
    return blocked / static_cast<float>(ray_count);
}

} // namespace kf::ext
