#include "world_to_picture.h"

namespace kf {

std::optional<std::array<double, 2>> world_to_picture(const PictureCamera &camera, const std::array<double, 3> &world) {
    if (!camera.world)
        return std::nullopt;
    // Camera space is the rotation (the transpose of camera_to_world) applied to the offset from the camera.
    const std::array<double, 3> offset{world[0] - camera.camera_world[0], world[1] - camera.camera_world[1],
                                       world[2] - camera.camera_world[2]};
    const auto &m = camera.camera_to_world;
    const auto row = [&m, &offset](std::size_t axis) {
        return m[axis] * offset[0] + m[3 + axis] * offset[1] + m[6 + axis] * offset[2];
    };
    const double depth = row(2);
    if (!(depth > 0))
        return std::nullopt;
    const double scale = camera.projection_distance / depth;
    return std::array<double, 2>{camera.center_x + row(0) * scale, camera.center_y + row(1) * scale};
}

} // namespace kf
