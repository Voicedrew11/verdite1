#ifndef KF_RENDERER_MODEL_TRANSFORM_H
#define KF_RENDERER_MODEL_TRANSFORM_H
// A model-to-world transform split the way the game builds one: a rotation,
// a scale along each model axis (an effect's scale_x, scale_y, scale_z, which
// may be 0 or negative) and a position. Blending two of them turns the
// rotation along the shorter arc and blends scale and position in a line, so a
// model in between keeps its shape where a blend of the matrices would shrink it.
#include <array>
#include <span>

namespace kf {
struct ModelTransform {
    std::array<double, 4> rotation{1, 0, 0, 0};   // unit quaternion w, x, y, z
    std::array<double, 3> scale{1, 1, 1};
    std::array<double, 3> position{};
};

// matrix: row-major 3 x 4, the rotation scaled along its columns, then the position.
ModelTransform split_transform(std::span<const double, 12> matrix);
// The transform at alpha (0: from, 1: to), as a row-major 3 x 4 matrix.
std::array<double, 12> blend_transforms(const ModelTransform &from, const ModelTransform &to, double alpha);
// The angle between the two rotations, in radians.
double turn_between(const ModelTransform &from, const ModelTransform &to);
} // namespace kf

#endif // KF_RENDERER_MODEL_TRANSFORM_H
