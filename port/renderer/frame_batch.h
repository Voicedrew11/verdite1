#ifndef KF_RENDERER_FRAME_BATCH_H
#define KF_RENDERER_FRAME_BATCH_H
// Turns one game update's faces into what a backend draws: triangles in the
// game's draw order, cut into runs that share a draw state. Knows no graphics
// API; the frontend uploads the result once per update.
#include <kf/renderer/backend.h>

#include "texture_layers.h"

#include <optional>
#include <span>
#include <vector>

namespace kf {
struct FrameBatch {
    std::vector<GpuVertex> vertices;
    std::vector<DrawRun> runs;
};

struct BatchRules {
    BlendMode initial_blend = BlendMode::average;
    bool depth_buffer = false;
    // World frames: faces that reach behind the camera are cut at the near
    // plane and their new corners projected with this. None: 2D frames.
    std::optional<Projection> near_clip;
    bool subpixel = true;   // new corners keep their fractions
    std::size_t part_count = 0;   // GameFrame::parts; a face of a part past them has none
};

// Faces are drawn farthest first by the game's sort depth, later submissions
// first among equal depths, as the PlayStation's ordering table did. Faces
// that are not a triangle or quad, or whose texture gets no layer, are dropped.
FrameBatch batch_faces(std::span<const DrawFace> faces, const BatchRules &rules, TextureLayers &layers);
} // namespace kf

#endif // KF_RENDERER_FRAME_BATCH_H
