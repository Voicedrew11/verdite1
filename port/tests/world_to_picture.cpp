#include "../renderer/world_to_picture.h"

#include <doctest/doctest.h>

#include <optional>
#include <stdexcept>

using namespace kf;

namespace {

// The value, or a test failure: the optional is checked before it is read.
template <typename T>
T checked(const std::optional<T> &value) {
    if (!value)
        throw std::runtime_error("expected a value");
    return *value;
}

// The camera at the origin, looking down +z, with the game's 200-unit projection.
PictureCamera level_camera() {
    PictureCamera camera;
    camera.world = true;
    return camera;
}

} // namespace

TEST_CASE("a point straight ahead of an identity camera lands on the picture's centre") {
    const auto point = checked(world_to_picture(level_camera(), {0, 0, 1000}));
    CHECK(point[0] == doctest::Approx(160));
    CHECK(point[1] == doctest::Approx(120));
}

TEST_CASE("a point projects with the projection distance over its depth, y down") {
    const auto right = checked(world_to_picture(level_camera(), {100, 0, 100}));
    const auto up = checked(world_to_picture(level_camera(), {0, -50, 100}));
    CHECK(right[0] == doctest::Approx(360));   // 160 + 100 * 200 / 100
    CHECK(up[1] == doctest::Approx(20));       // 120 - 50 * 200 / 100
}

TEST_CASE("points at or behind the camera have no picture position") {
    CHECK_FALSE(world_to_picture(level_camera(), {0, 0, 0}).has_value());
    CHECK_FALSE(world_to_picture(level_camera(), {0, 0, -100}).has_value());
}

TEST_CASE("a frame with no world camera has no picture position") {
    PictureCamera camera = level_camera();
    camera.world = false;
    CHECK_FALSE(world_to_picture(camera, {0, 0, 1000}).has_value());
}

TEST_CASE("the camera's position is subtracted before the rotation") {
    PictureCamera camera = level_camera();
    camera.camera_world = {1000, 0, 0};
    const auto point = checked(world_to_picture(camera, {1100, 0, 100}));
    CHECK(point[0] == doctest::Approx(360));
    CHECK(point[1] == doctest::Approx(120));
}

TEST_CASE("a turned camera looks down world +x, with its own right along world -z") {
    PictureCamera camera = level_camera();
    // camera_to_world is the transpose of the view rotation: its columns are the camera axes in world space.
    camera.camera_to_world = {0, 0, 1, 0, 1, 0, -1, 0, 0};
    CHECK(checked(world_to_picture(camera, {100, 0, 0}))[0] == doctest::Approx(160));
    CHECK(checked(world_to_picture(camera, {100, 0, -50}))[0] == doctest::Approx(260));   // 50 to the camera's right
    CHECK_FALSE(world_to_picture(camera, {0, 0, -50}).has_value());                    // beside the camera
}
