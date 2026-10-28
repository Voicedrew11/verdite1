#include <kf/platform/online_weapon_hold.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <numeric>

namespace kf {
namespace {

constexpr float pi = std::numbers::pi_v<float>;
constexpr s32 attack_blend_phase = 600;    // the arm turns into the swing over this, and back at its end
constexpr s32 attack_phase_end = 4096;     // KF_WEAPON_ATTACK_PHASE_END
constexpr float upper_arm_follow = 0.7f;   // of the forearm's lift the upper arm takes through a swing
constexpr float fixed_one = 4096.0f;
constexpr int axis_iterations = 24;
// At rest the blade points ahead and up, its flat to the side.
constexpr OnlineVector rest_blade = {0.0f, -0.9f, -1.0f};
constexpr OnlineVector rest_side = {1.0f, 0.0f, 0.0f};
constexpr OnlineVector hanging = {0.0f, 1.0f, 0.0f};

OnlineVector operator+(OnlineVector a, OnlineVector b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
OnlineVector operator-(OnlineVector a, OnlineVector b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
OnlineVector operator*(OnlineVector a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(OnlineVector a, OnlineVector b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
OnlineVector cross(OnlineVector a, OnlineVector b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float length(OnlineVector a) { return std::sqrt(dot(a, a)); }
OnlineVector unit(OnlineVector a) { return a * (1.0f / length(a)); }
OnlineVector vector(const SVECTOR &v) {
    return {static_cast<float>(v.vx), static_cast<float>(v.vy), static_cast<float>(v.vz)};
}

OnlineVector rotate(const OnlineRotation &r, OnlineVector v) { return {dot(r[0], v), dot(r[1], v), dot(r[2], v)}; }

OnlineRotation transposed(const OnlineRotation &r) {
    return {{{r[0].x, r[1].x, r[2].x}, {r[0].y, r[1].y, r[2].y}, {r[0].z, r[1].z, r[2].z}}};
}

// The rotation whose columns are these axes.
OnlineRotation from_columns(OnlineVector x, OnlineVector y, OnlineVector z) {
    return transposed({x, y, z});
}

// An orthonormal frame from three points: a to b, then toward c.
OnlineRotation frame(OnlineVector a, OnlineVector b, OnlineVector c) {
    const auto x = unit(b - a);
    const auto z = unit(cross(x, c - a));
    return from_columns(x, cross(z, x), z);
}

// A frame whose first axis is first and whose second is toward second.
OnlineRotation basis(OnlineVector first, OnlineVector second) {
    const auto x = unit(first);
    const auto y = unit(second - x * dot(second, x));
    return from_columns(x, y, cross(x, y));
}

OnlineRotation about_x(float angle) {
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    return {{{1, 0, 0}, {0, c, -s}, {0, s, c}}};
}

// The shortest rotation taking one unit direction onto another.
OnlineRotation turned_onto(OnlineVector from, OnlineVector onto) {
    const auto axis = cross(from, onto);
    const float s = length(axis);
    const float c = dot(from, onto);
    if (s < 1e-6f)
        return c > 0.0f ? OnlineRotation{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}} : about_x(pi);
    const auto k = axis * (1.0f / s);
    const float t = 1.0f - c;
    return {{{t * k.x * k.x + c, t * k.x * k.y - s * k.z, t * k.x * k.z + s * k.y},
             {t * k.x * k.y + s * k.z, t * k.y * k.y + c, t * k.y * k.z - s * k.x},
             {t * k.x * k.z - s * k.y, t * k.y * k.z + s * k.x, t * k.z * k.z + c}}};
}

struct Quaternion {
    float w, x, y, z;
};

Quaternion quaternion(const OnlineRotation &r) {
    const float trace = r[0].x + r[1].y + r[2].z;
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        return {s / 4.0f, (r[2].y - r[1].z) / s, (r[0].z - r[2].x) / s, (r[1].x - r[0].y) / s};
    }
    if (r[0].x > r[1].y && r[0].x > r[2].z) {
        const float s = std::sqrt(1.0f + r[0].x - r[1].y - r[2].z) * 2.0f;
        return {(r[2].y - r[1].z) / s, s / 4.0f, (r[0].y + r[1].x) / s, (r[0].z + r[2].x) / s};
    }
    if (r[1].y > r[2].z) {
        const float s = std::sqrt(1.0f + r[1].y - r[0].x - r[2].z) * 2.0f;
        return {(r[0].z - r[2].x) / s, (r[0].y + r[1].x) / s, s / 4.0f, (r[1].z + r[2].y) / s};
    }
    const float s = std::sqrt(1.0f + r[2].z - r[0].x - r[1].y) * 2.0f;
    return {(r[1].x - r[0].y) / s, (r[0].z + r[2].x) / s, (r[1].z + r[2].y) / s, s / 4.0f};
}

OnlineRotation rotation(Quaternion q) {
    const float n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    q = {q.w / n, q.x / n, q.y / n, q.z / n};
    return {{{1 - 2 * (q.y * q.y + q.z * q.z), 2 * (q.x * q.y - q.z * q.w), 2 * (q.x * q.z + q.y * q.w)},
             {2 * (q.x * q.y + q.z * q.w), 1 - 2 * (q.x * q.x + q.z * q.z), 2 * (q.y * q.z - q.x * q.w)},
             {2 * (q.x * q.z - q.y * q.w), 2 * (q.y * q.z + q.x * q.w), 1 - 2 * (q.x * q.x + q.y * q.y)}}};
}

// Between two rotations, the short way round.
OnlineRotation slerp(const OnlineRotation &from, const OnlineRotation &to, float t) {
    const auto a = quaternion(from);
    auto b = quaternion(to);
    float cosine = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    if (cosine < 0.0f) {
        b = {-b.w, -b.x, -b.y, -b.z};
        cosine = -cosine;
    }
    float wa = 1.0f - t;
    float wb = t;
    if (cosine < 0.9995f) {
        const float angle = std::acos(cosine);
        wa = std::sin((1.0f - t) * angle) / std::sin(angle);
        wb = std::sin(t * angle) / std::sin(angle);
    }
    return rotation({wa * a.w + wb * b.w, wa * a.x + wb * b.x, wa * a.y + wb * b.y, wa * a.z + wb * b.z});
}

// The model's closed pieces: each vertex's piece, numbered by its first vertex.
std::vector<u16> pieces(std::size_t vertex_count, std::span<const OnlineModelFace> faces) {
    std::vector<u16> root(vertex_count);
    std::iota(root.begin(), root.end(), u16{0});
    const auto find = [&root](u16 v) {
        while (root[v] != v) {
            root[v] = root[root[v]];
            v = root[v];
        }
        return v;
    };
    for (const auto &face : faces)
        for (u8 i = 1; i < face.corners; ++i) {
            const u16 a = find(face.vertices[0]);
            const u16 b = find(face.vertices[i]);
            root[std::max(a, b)] = std::min(a, b);
        }
    for (std::size_t v = 0; v < vertex_count; ++v)
        root[v] = find(static_cast<u16>(v));
    return root;
}

OnlineVector centroid(std::span<const OnlineVector> points, std::span<const u16> which) {
    OnlineVector sum{};
    for (const u16 i : which)
        sum = sum + points[i];
    return sum * (1.0f / static_cast<float>(which.size()));
}

// The direction the points spread along most, signed toward lean.
OnlineVector long_axis(std::span<const OnlineVector> points, std::span<const u16> which, OnlineVector lean) {
    const auto centre = centroid(points, which);
    OnlineRotation spread{};
    for (const u16 i : which) {
        const auto d = points[i] - centre;
        spread[0] = spread[0] + d * d.x;
        spread[1] = spread[1] + d * d.y;
        spread[2] = spread[2] + d * d.z;
    }
    auto axis = unit(lean);
    for (int i = 0; i < axis_iterations; ++i)
        axis = unit(rotate(spread, axis));
    return dot(axis, lean) < 0.0f ? axis * -1.0f : axis;
}

u16 farthest(std::span<const OnlineVector> points, std::span<const u16> which, OnlineVector from) {
    return *std::max_element(which.begin(), which.end(),
        [&](u16 a, u16 b) { return length(points[a] - from) < length(points[b] - from); });
}

// Three of the vertices to follow: two far apart, the third far from the line between them.
std::array<u16, 3> followed(std::span<const OnlineVector> points, std::span<const u16> held) {
    const u16 first = farthest(points, held, centroid(points, held));
    const u16 second = farthest(points, held, points[first]);
    const auto line = unit(points[second] - points[first]);
    const u16 third = *std::max_element(held.begin(), held.end(), [&](u16 a, u16 b) {
        const auto off = [&](u16 v) {
            const auto d = points[v] - points[first];
            return length(d - line * dot(d, line));
        };
        return off(a) < off(b);
    });
    return {first, second, third};
}

OnlineRotation followed_frame(const OnlineWeaponHold &hold, std::span<const SVECTOR, 3> vertices) {
    return frame(rotate(hold.view, vector(vertices[0])), rotate(hold.view, vector(vertices[1])),
        rotate(hold.view, vector(vertices[2])));
}

// The upper arm through a swing, lifted forward by most of the forearm's angle from hanging down.
OnlineRotation upper_arm_following(const OnlineRotation &forearm) {
    const auto d = rotate(forearm, hanging);
    float angle = std::atan2(-d.z, d.y);
    if (angle < -pi / 4.0f)
        angle += 2.0f * pi;
    return about_x(-upper_arm_follow * angle);
}

}

std::optional<OnlineWeaponHold> online_weapon_hold(std::span<const SVECTOR> hit_vertices,
    std::span<const OnlineModelFace> faces, const OnlineRotation &view) {
    if (hit_vertices.empty() || hit_vertices.size() > UINT16_MAX)
        return std::nullopt;
    for (const auto &face : faces) {
        if (face.corners < 3 || face.corners > 4)
            return std::nullopt;
        for (u8 i = 0; i < face.corners; ++i)
            if (face.vertices[i] >= hit_vertices.size())
                return std::nullopt;
    }
    std::vector<OnlineVector> points;
    points.reserve(hit_vertices.size());
    for (const auto &v : hit_vertices)
        points.push_back(vector(v));

    // The forearm reaches the elbow, the model's origin; the rest is held.
    const auto piece = pieces(points.size(), faces);
    std::size_t elbow = 0;
    for (std::size_t v = 1; v < points.size(); ++v)
        if (length(points[v]) < length(points[elbow]))
            elbow = v;
    OnlineWeaponHold hold{};
    hold.view = view;
    std::vector<u16> held;
    for (std::size_t v = 0; v < points.size(); ++v)
        (piece[v] == piece[elbow] ? hold.forearm : held).push_back(static_cast<u16>(v));
    if (held.size() < 3 || hold.forearm.size() < 2)
        return std::nullopt;

    const auto forearm_centre = centroid(points, hold.forearm);
    const auto axis = long_axis(points, hold.forearm, centroid(points, held) - forearm_centre);
    float reach = 0.0f;
    for (const u16 v : hold.forearm)
        reach = std::max(reach, dot(points[v] - forearm_centre, axis));
    const auto wrist = forearm_centre + axis * reach;
    hold.wrist = {static_cast<s16>(std::lround(wrist.x)), static_cast<s16>(std::lround(wrist.y)),
        static_cast<s16>(std::lround(wrist.z)), 0};

    hold.followed = followed(points, held);
    const std::array<SVECTOR, 3> at_hit = {
        hit_vertices[hold.followed[0]], hit_vertices[hold.followed[1]], hit_vertices[hold.followed[2]]};
    hold.hit_frame = followed_frame(hold, at_hit);
    if (!std::isfinite(hold.hit_frame[0].x + hold.hit_frame[1].y + hold.hit_frame[2].z))
        return std::nullopt;
    hold.wrist_from_followed = rotate(view, wrist - points[hold.followed[0]]);
    // The blade: from the wrist to the held vertex furthest from it.
    const auto blade = rotate(view, points[farthest(points, held, wrist)] - wrist);
    hold.rest = online_compose(basis(rest_blade, rest_side),
        transposed(basis(blade, rotate(view, {1.0f, 0.0f, 0.0f}))));
    return hold;
}

OnlineWeaponArm online_weapon_arm_at_rest(const OnlineWeaponHold &hold, const OnlineRotation &upper_arm,
    const OnlineRotation &forearm) {
    return {upper_arm, forearm, hold.rest};
}

OnlineWeaponArm online_weapon_arm_swinging(const OnlineWeaponHold &hold, const OnlineWeaponArm &rest,
    std::span<const SVECTOR, 3> followed_vertices, float attack_weight) {
    const auto turn = online_compose(followed_frame(hold, followed_vertices), transposed(hold.hit_frame));
    const auto wrist = rotate(hold.view, vector(followed_vertices[0])) + rotate(turn, hold.wrist_from_followed);
    const auto forearm = slerp(rest.forearm, turned_onto(hanging, unit(wrist)), attack_weight);
    return {slerp(rest.upper_arm, upper_arm_following(forearm), attack_weight), forearm,
        slerp(rest.held, turn, attack_weight)};
}

float online_attack_weight(s16 attack_phase) {
    if (attack_phase < 0)
        return 0.0f;
    const s32 into = std::min<s32>(attack_phase, attack_phase_end - attack_phase);
    return std::clamp(static_cast<float>(into) / attack_blend_phase, 0.0f, 1.0f);
}

OnlineRotation online_compose(const OnlineRotation &outer, const OnlineRotation &inner) {
    const auto columns = transposed(inner);
    OnlineRotation out{};
    for (std::size_t r = 0; r < 3; ++r)
        out[r] = {dot(outer[r], columns[0]), dot(outer[r], columns[1]), dot(outer[r], columns[2])};
    return out;
}

OnlineRotation online_rotation(const MATRIX &matrix) {
    OnlineRotation out{};
    for (std::size_t r = 0; r < 3; ++r)
        out[r] = {matrix.m[r][0] / fixed_one, matrix.m[r][1] / fixed_one, matrix.m[r][2] / fixed_one};
    return out;
}

MATRIX online_matrix(const OnlineRotation &rotation) {
    MATRIX out{};
    for (std::size_t r = 0; r < 3; ++r) {
        out.m[r][0] = static_cast<s16>(std::lround(rotation[r].x * fixed_one));
        out.m[r][1] = static_cast<s16>(std::lround(rotation[r].y * fixed_one));
        out.m[r][2] = static_cast<s16>(std::lround(rotation[r].z * fixed_one));
    }
    return out;
}

}
