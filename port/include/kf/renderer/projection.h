#ifndef KF_RENDERER_PROJECTION_H
#define KF_RENDERER_PROJECTION_H

#include <kf/lib/geometry_types.h>

#include <array>

namespace kf {
struct Projection {
    // Original fog setters always use this reference, even for weapon views.
    static constexpr s32 fog_reference_distance = 200;
    s32 center_x = 160;
    s32 center_y = 120;
    u16 distance = 200;
    s32 fog_near = 11000;
};

struct ProjectedPoint {
    s16 x, y;
    s32 depth;
    s32 fog;
    // PC extension: the same position before truncation to whole pixels, and the
    // camera-space depth for perspective-correct texturing. Game logic keeps
    // using x/y/depth; only drawn vertices read these.
    float fx, fy, w;
    // Camera-space position, so the host can re-project between game updates.
    float cx, cy, cz;
};

VECTOR render_transform_point(const MATRIX &model, const SVECTOR &point);
// PC extension: the same point with the fraction the fixed-point rotation
// drops, for drawn vertices only.
std::array<double, 3> render_transform_point_exact(const MATRIX &model, const SVECTOR &point);
void render_place_model(MATRIX &model, const MATRIX &view, const SVECTOR &position);
ProjectedPoint render_project_point(const MATRIX &model, const Projection &projection,
    const SVECTOR &point);
// The depth cue's slope (the GTE's DQA) for a fog start distance: a point at
// depth z gets the cue ((H / z in Q16) x slope + depth_cue_bias) >> 12, held
// to 0..4096.
inline constexpr s32 depth_cue_bias = 0x1400000;
s16 render_fog_slope(const Projection &projection);
}

#endif // KF_RENDERER_PROJECTION_H
