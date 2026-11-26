#include "../ext/corner_occlusion.h"

#include <doctest/doctest.h>

#include <vector>

using namespace kf::ext;

namespace {

constexpr float reach = 1000.0f;
constexpr Point3 up{0, 0, 1};

// A floor rectangle at height z; a wall in the plane x, from the floor up to height.
void add_floor(std::vector<OccluderTriangle> &out, float x0, float y0, float x1, float y1, float z) {
    out.push_back(occluder_triangle({x0, y0, z}, {x1, y0, z}, {x0, y1, z}));
    out.push_back(occluder_triangle({x1, y0, z}, {x1, y1, z}, {x0, y1, z}));
}
void add_wall(std::vector<OccluderTriangle> &out, float x, float y0, float y1, float height) {
    out.push_back(occluder_triangle({x, y0, 0}, {x, y1, 0}, {x, y0, height}));
    out.push_back(occluder_triangle({x, y1, 0}, {x, y1, height}, {x, y0, height}));
}

} // namespace

TEST_CASE("corner occlusion: an open floor is not occluded by itself") {
    std::vector<OccluderTriangle> scene;
    add_floor(scene, -2000, -2000, 2000, 2000, 0);
    CHECK(corner_occlusion({0, 0, 0}, up, {100, 100, 0}, scene, reach) == 0.0f);
}

TEST_CASE("corner occlusion: a floor corner against a wall is darkened, less than closed") {
    std::vector<OccluderTriangle> scene;
    add_floor(scene, 0, -2000, 2000, 2000, 0);
    add_wall(scene, 0, -2000, 2000, 3000);
    const float crease = corner_occlusion({0, 0, 0}, up, {500, 0, 0}, scene, reach);
    CHECK(crease > 0.2f);
    CHECK(crease < 0.6f);
    // Halfway out it is lighter, and past reach the wall no longer counts.
    const float halfway = corner_occlusion({500, 0, 0}, up, {600, 0, 0}, scene, reach);
    CHECK(halfway > 0.0f);
    CHECK(halfway < crease);
    CHECK(corner_occlusion({1500, 0, 0}, up, {1600, 0, 0}, scene, reach) == 0.0f);
}

TEST_CASE("corner occlusion: a neighbour's face 4 units proud of this one does not occlude it") {
    std::vector<OccluderTriangle> scene;
    add_floor(scene, -2000, -2000, 0, 2000, 0);
    add_floor(scene, -4, -2000, 2000, 2000, 4);
    CHECK(corner_occlusion({-100, 0, 0}, up, {-200, 0, 0}, scene, reach) == 0.0f);
}
