#ifndef KF_RENDERER_WORLD_TO_PICTURE_H
#define KF_RENDERER_WORLD_TO_PICTURE_H
// Where a world point lands on the PS1's 320 x 240 picture, through the camera as a frame shows it
// (ui.world_to_screen). Pure: no backend, no game state.
#include <array>
#include <optional>

namespace kf {

struct PictureCamera {
    bool world = false;                       // false: a menu or a 2D update, nothing to project through
    std::array<float, 9> camera_to_world{1, 0, 0, 0, 1, 0, 0, 0, 1};   // row-major
    std::array<double, 3> camera_world{};     // world units
    double projection_distance = 200;
    double center_x = 160, center_y = 120;
};

// The picture position of a world point, or none behind the camera (depth 0 or less) or with no
// world camera.
std::optional<std::array<double, 2>> world_to_picture(const PictureCamera &camera, const std::array<double, 3> &world);

} // namespace kf

#endif // KF_RENDERER_WORLD_TO_PICTURE_H
