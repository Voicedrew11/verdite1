#ifndef KF_RENDERER_VIEW_SMOOTHING_H
#define KF_RENDERER_VIEW_SMOOTHING_H
// Where each displayed frame shows the last game update from. The game updates
// 20 times a second; between updates the view blends the camera and moving
// entities from the previous update to the last one, while look input shows
// at once. The result is matrices a backend applies in its vertex shader.
#include <kf/renderer/backend.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace kf {
struct ShownView {
    std::vector<Motion> motions;   // [0]: faces of no entity; [n]: entity n of the update
    bool reproject = false;
    float projection_distance = 200;
    float center_x = 160, center_y = 120;
    std::array<float, 9> camera_to_world{1, 0, 0, 0, 1, 0, 0, 0, 1};
    std::array<float, 3> camera_world{};
};

class ViewSmoothing {
public:
    // A new update, received at at_ns. entity_tags: the game's tag of each
    // entity the update drew (FrameBatch::entity_tags). A 2D update (no camera),
    // a warp, a sharp turn or a long gap starts blending afresh.
    void take_update(const std::optional<CameraPose> &camera, SVECTOR look_applied,
                     std::span<const EntityPose> poses, std::span<const u16> entity_tags, std::uint64_t at_ns);
    // The last update exactly as the game drew it.
    ShownView view_as_drawn() const;
    // The last update seen at now_ns, turned further by pending_look (angle
    // units). As drawn when the update has no camera.
    ShownView view_at(std::uint64_t now_ns, SVECTOR pending_look) const;

private:
    struct EntityStep {
        bool moves = false;
        std::array<float, 3> origin{};    // this update's position, world units
        std::array<float, 3> back{};      // to the previous update's position
        float back_yaw = 0;               // radians, to the previous update's turn
        int yaw_sign = 0;
    };
    std::optional<CameraPose> previous_, current_;
    SVECTOR look_applied_{0, 0, 0};
    std::uint64_t current_ns_ = 0, interval_ns_ = 0;
    std::vector<EntityStep> steps_;              // per entity number, from 1
    std::vector<std::optional<EntityPose>> last_poses_;   // by tag
};

// The update's lights as this frame shows them: strength and flicker at now_ns
// applied, moved by camera_change into the shown camera's space, nearest first.
std::vector<RenderLight> shown_lights(std::span<const GameLight> lights, float strength,
                                      const Motion &camera_change, std::uint64_t now_ns);
} // namespace kf

#endif // KF_RENDERER_VIEW_SMOOTHING_H
