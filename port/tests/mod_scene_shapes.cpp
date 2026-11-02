#include "../ext/mod_scene_shapes.h"

#include <kf/renderer/renderer.h>

#include <doctest/doctest.h>

#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <stdexcept>

using namespace kf::ext;

namespace {

constexpr s32 s32_min = std::numeric_limits<s32>::min();
constexpr s32 s32_max = std::numeric_limits<s32>::max();

// The game's identity rotation: 4096 is one.
MATRIX identity_matrix() {
    MATRIX m{};
    for (std::size_t i = 0; i < 3; ++i)
        m.m[i][i] = 4096;
    return m;
}

SceneView view_at(s32 x, s32 y, s32 z) { return SceneView{VECTOR{x, y, z}, identity_matrix()}; }

// The value, or a test failure: the optional is checked before it is read.
template <typename T>
T checked(const std::optional<T> &value) {
    if (!value)
        throw std::runtime_error("expected a value");
    return *value;
}

SceneQuad expect_quad(const SceneView &view, const SceneShape &shape) { return checked(scene_quad(view, shape)); }

// Every corner and the centre fit the game's vectors and lie in front of the renderer's near plane.
void check_drawable(const SceneQuad &quad) {
    for (const ViewOffset &corner : quad.corners) {
        CHECK(corner[2] > kf::render_near_depth);
        CHECK(std::abs(static_cast<int>(corner[0])) <= 16000);
        CHECK(std::abs(static_cast<int>(corner[1])) <= 16000);
    }
    CHECK(quad.centre[2] > kf::render_near_depth);
}

SceneLine line(WorldPoint from, WorldPoint to, double width = 10) {
    SceneLine out;
    out.from = from;
    out.to = to;
    out.width = width;
    return out;
}

SceneMarker marker(WorldPoint centre, double size = 100) {
    SceneMarker out;
    out.centre = centre;
    out.size = size;
    return out;
}

} // namespace

TEST_CASE("a marker in front is a square facing the camera around its centre") {
    const SceneQuad quad = expect_quad(view_at(0, 0, 0), marker({0, 0, 1000}, 100));
    CHECK(quad.centre == ViewOffset{0, 0, 1000});
    CHECK(quad.corners[0] == ViewOffset{-50, -50, 1000});
    CHECK(quad.corners[1] == ViewOffset{50, -50, 1000});
    CHECK(quad.corners[2] == ViewOffset{-50, 50, 1000});
    CHECK(quad.corners[3] == ViewOffset{50, 50, 1000});
}

TEST_CASE("a marker behind the camera, at the near plane, or past the reach is dropped") {
    const SceneView view = view_at(0, 0, 0);
    CHECK_FALSE(scene_quad(view, marker({0, 0, -1000})).has_value());
    CHECK_FALSE(scene_quad(view, marker({0, 0, 10})).has_value());
    CHECK_FALSE(scene_quad(view, marker({20000, 0, 1000})).has_value());
}

TEST_CASE("a line in front is a quad across it, its sides in the plane facing the camera") {
    const SceneQuad quad = expect_quad(view_at(0, 0, 0), line({-100, 0, 1000}, {100, 0, 1000}, 20));
    CHECK(quad.corners[0] == ViewOffset{-100, 10, 1000});
    CHECK(quad.corners[1] == ViewOffset{-100, -10, 1000});
    CHECK(quad.corners[2] == ViewOffset{100, 10, 1000});
    CHECK(quad.corners[3] == ViewOffset{100, -10, 1000});
    check_drawable(quad);
}

TEST_CASE("a line pointing at the camera still gets a side") {
    const SceneQuad quad = expect_quad(view_at(0, 0, 0), line({0, 0, 1000}, {0, 0, 2000}, 20));
    CHECK(quad.corners[0] == ViewOffset{-10, 0, 1000});
    CHECK(quad.corners[1] == ViewOffset{10, 0, 1000});
    check_drawable(quad);
}

TEST_CASE("zero-length, wholly behind, and wholly past the reach lines are dropped") {
    const SceneView view = view_at(0, 0, 0);
    CHECK_FALSE(scene_quad(view, line({0, 0, 1000}, {0, 0, 1000})).has_value());
    CHECK_FALSE(scene_quad(view, line({0, 0, -500}, {50, 0, -900})).has_value());
    CHECK_FALSE(scene_quad(view, line({30000, 0, 1000}, {40000, 0, 1000})).has_value());
    CHECK_FALSE(scene_quad(view, line({0, 0, 5}, {0, 0, 10})).has_value());
}

TEST_CASE("a line crossing the near plane is cut to it, and its far end is kept") {
    const SceneQuad quad = expect_quad(view_at(0, 0, 0), line({0, 0, -100}, {0, 0, 1000}));
    check_drawable(quad);
    CHECK(quad.corners[2][2] == 1000);
    CHECK(quad.corners[0][2] == quad.corners[1][2]);
    CHECK(quad.corners[0][2] >= 36);
    CHECK(quad.corners[0][2] <= 40);
}

TEST_CASE("a line past the reach is cut to it") {
    const SceneQuad quad = expect_quad(view_at(0, 0, 0), line({0, 0, 1000}, {30000, 0, 1000}, 10));
    check_drawable(quad);
    CHECK(quad.corners[2][0] <= 16000);
    CHECK(quad.corners[2][0] >= 15900);
}

TEST_CASE("a line across the whole range is cut to the reach, and a point across it is dropped") {
    const SceneQuad from_the_end = expect_quad(view_at(s32_min, 0, 0), line({s32_min, 0, 1000}, {s32_max, 0, 1000}));
    check_drawable(from_the_end);
    CHECK(from_the_end.corners[2][0] >= 15900);
    CHECK(from_the_end.corners[2][0] <= 16000);
    const SceneQuad through_the_view = expect_quad(view_at(0, 0, 0), line({s32_min, 0, 1000}, {s32_max, 0, 1000}));
    check_drawable(through_the_view);
    CHECK(through_the_view.centre[0] == 0);
    CHECK_FALSE(scene_quad(view_at(s32_max, 0, 0), marker({s32_min, 0, 1000})).has_value());
}

TEST_CASE("coordinates are differenced in 64 bits, so a view at the range's end does not wrap") {
    const SceneQuad quad = expect_quad(view_at(s32_min, s32_min, s32_min), marker({s32_min + 100, s32_min, s32_min + 1000}));
    CHECK(quad.centre == ViewOffset{100, 0, 1000});
    const SceneQuad wide = expect_quad(view_at(s32_max, 0, 0), line({s32_max - 5, 0, 1000}, {s32_max, 0, 1000}));
    CHECK(wide.centre == ViewOffset{-3, 0, 1000});
}

TEST_CASE("a rotated view puts the camera's forward axis along world +x") {
    MATRIX rotation{};
    rotation.m = {{{0, 0, -4096}, {0, 4096, 0}, {4096, 0, 0}}};
    const SceneView view{VECTOR{0, 0, 0}, rotation};
    CHECK(expect_quad(view, marker({500, 0, 0}, 100)).centre == ViewOffset{500, 0, 0});
    CHECK_FALSE(scene_quad(view, marker({0, 0, 500})).has_value());
    CHECK_FALSE(scene_quad(view, marker({-500, 0, 0})).has_value());
}

TEST_CASE("over many shapes, every corner that comes out is in reach and in front of the near plane") {
    std::uint32_t seed = 12345;
    const auto next = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<s32>(seed >> 8) - (1 << 23);   // about +-8e6 world units
    };
    int drawn = 0;
    for (int i = 0; i < 5000; ++i) {
        // Shapes within about 20000 units of the view, so most of them reach the picture's range.
        const s32 vx = next(), vy = next(), vz = next();
        const SceneView view = view_at(vx, vy, vz);
        const WorldPoint a{vx + next() / 400, vy + next() / 400, vz + next() / 400};
        const WorldPoint b{a[0] + next() / 400, a[1] + next() / 400, a[2] + next() / 400};
        if (const auto quad = scene_quad(view, line(a, b, 1.0 + (i % 1000)))) {
            check_drawable(*quad);
            ++drawn;
        }
        if (const auto quad = scene_quad(view, marker(a, 16.0 + (i % 4000)))) {
            check_drawable(*quad);
            ++drawn;
        }
    }
    CHECK(drawn > 0);
}
