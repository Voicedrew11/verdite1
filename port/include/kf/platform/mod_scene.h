#ifndef KF_PLATFORM_MOD_SCENE_H
#define KF_PLATFORM_MOD_SCENE_H
// PC extension (kf.scene, docs/MODDING.md): the mods' lines and markers, as quads the game draws
// with the scene (render_mod_scene). Kept by port/ext/mod_scene.cpp; empty with no mods or in coop.
#include <kf/lib/geometry_types.h>
#include <kf/renderer/renderer.h>

#include <array>
#include <vector>

namespace kf {

struct ModSceneFace {
    std::array<SVECTOR, 4> corners{};   // relative to the view position, in strip order
    SVECTOR centre{};                   // for depth sorting
    CVECTOR colour{};                   // unlit, not fogged
    BlendMode blend = BlendMode::opaque;   // add for a translucent shape, its colour already scaled
};

// The shapes the mods drew, as the view stands. Shapes wholly out of view or behind the near plane
// are left out.
std::vector<ModSceneFace> mod_scene_faces(const VECTOR &view_position, const MATRIX &view_matrix);

} // namespace kf

#endif // KF_PLATFORM_MOD_SCENE_H
