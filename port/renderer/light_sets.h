#ifndef KF_RENDERER_LIGHT_SETS_H
#define KF_RENDERER_LIGHT_SETS_H
// The game's lighting of each run of faces (LightSet), as a displayed frame
// shows it (ShownLightSet), for lighting per pixel. The game keeps its lights
// fixed in the world: a model's light matrix is the lights turned into its
// model space. A model carried between updates turns, so its lights are turned
// back by as much as the model has turned since the update drew it.
#include <kf/renderer/backend.h>

#include <span>
#include <vector>

namespace kf {
// The part each light set's faces were drawn as (index n - 1 for light set n):
// a model in the world (ModelMove), or 0 when none is.
std::vector<u16> light_set_parts(std::size_t set_count, std::span<const DrawFace> faces,
                                 std::span<const PartMove> parts);

// Each light set seen through motions (FrameView::motions, as reprojected),
// parts from light_set_parts; no parts when the frame is not reprojected. A
// set of no part is shown as the game lit it.
std::vector<ShownLightSet> shown_light_sets(std::span<const LightSet> sets, std::span<const u16> parts,
                                            std::span<const Motion> motions);
} // namespace kf

#endif // KF_RENDERER_LIGHT_SETS_H
