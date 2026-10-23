#ifndef KF_RENDERER_VIEW_SMOOTHING_H
#define KF_RENDERER_VIEW_SMOOTHING_H
// Where each displayed frame shows the last game update from. The game updates
// 20 times a second; between updates the view blends the camera and each part
// that moved (VertexCarry) from the previous update to the last one, while
// look input shows at once. The result is matrices a backend applies in its
// vertex shader.
#include <kf/renderer/backend.h>

#include "model_transform.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace kf {
struct ShownView {
    std::vector<Motion> motions;   // [0]: faces of no part; [n]: part n of the update
    float carry = 0;               // FrameView::carry
    bool reproject = false;
    float projection_distance = 200;
    float center_x = 160, center_y = 120;
    std::array<float, 9> camera_to_world{1, 0, 0, 0, 1, 0, 0, 0, 1};
    std::array<float, 3> camera_world{};
};

class ViewSmoothing {
public:
    // A new update, received at at_ns, and what moved in it (GameFrame::parts).
    // A 2D update (no camera), a warp, a sharp turn or a long gap starts
    // blending afresh.
    void take_update(const std::optional<CameraPose> &camera, SVECTOR look_applied, std::span<const PartMove> parts,
                     std::uint64_t at_ns);
    // The last update exactly as the game drew it.
    ShownView view_as_drawn() const;
    // The last update seen at now_ns, turned further by pending_look (angle
    // units). As drawn when the update has no camera.
    ShownView view_at(std::uint64_t now_ns, SVECTOR pending_look) const;

private:
    struct ModelStep {
        ModelTransform before, now;
        std::array<double, 12> drawn{};   // the update's own transform, shown once blending is done
    };
    using PartStep = std::variant<ModelStep, DialTurn>;
    std::optional<CameraPose> previous_, current_;
    SVECTOR look_applied_{0, 0, 0};
    std::uint64_t current_ns_ = 0, interval_ns_ = 0;
    bool blending_ = false;              // previous_ leads into current_
    std::vector<PartStep> parts_;
};

// The update's lights as this frame shows them: strength and flicker at now_ns
// applied, moved by camera_change into the shown camera's space, nearest first.
std::vector<RenderLight> shown_lights(std::span<const GameLight> lights, float strength,
                                      const Motion &camera_change, std::uint64_t now_ns);
} // namespace kf

#endif // KF_RENDERER_VIEW_SMOOTHING_H
