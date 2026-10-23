#ifndef KF_RENDERER_VIEW_ROTATION_H
#define KF_RENDERER_VIEW_ROTATION_H
// The game builds its view rotation in 12-bit fixed point (matrix_set_rotation_xyz),
// so the matrix is a little off a rotation, and off differently at each angle:
// as the view turns, corners move against each other and textures warble. The
// renderer undoes the game's matrix with its inverse (not its transpose) and
// sees the world through the exact rotation instead.
#include <kf/lib/geometry_types.h>

#include <array>

namespace kf {
using Rotation3 = std::array<std::array<double, 3>, 3>;   // row-major

// The rotation the angles (4096 units a turn) stand for, in matrix_set_rotation_xyz's order.
Rotation3 exact_view_rotation(const SVECTOR &angles);
// The inverse of the game's own matrix for the angles: from camera space as
// the game projected it to world directions.
Rotation3 game_view_inverse(const SVECTOR &angles);
} // namespace kf

#endif // KF_RENDERER_VIEW_ROTATION_H
