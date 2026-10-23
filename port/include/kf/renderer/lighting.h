#ifndef KF_RENDERER_LIGHTING_H
#define KF_RENDERER_LIGHTING_H

#include <kf/lib/geometry_types.h>

namespace kf {
struct LightColor { s32 r, g, b; };
struct LightingEnvironment {
    MATRIX color_matrix;
    LightColor ambient;
    LightColor fog;
};
CVECTOR render_light_normal(const LightingEnvironment &environment, const MATRIX &lights,
    const SVECTOR &normal, CVECTOR base, s32 depth_cue = 0);
CVECTOR render_fog_color(const LightingEnvironment &environment, CVECTOR color, s32 depth_cue);

// PC extension (fog drawn per pixel): a vertex colour as the game cued it toward
// the fog colour, and the same colour before the cue, so a renderer can apply
// the cue itself at each pixel instead.
struct DepthCuedColor {
    CVECTOR cued;
    CVECTOR clear;
};
DepthCuedColor render_light_normal_cued(const LightingEnvironment &environment, const MATRIX &lights,
    const SVECTOR &normal, CVECTOR base, s32 depth_cue);
DepthCuedColor render_fog_color_cued(const LightingEnvironment &environment, CVECTOR color, s32 depth_cue);
}

#endif // KF_RENDERER_LIGHTING_H
