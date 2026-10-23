#ifndef KF_RENDERER_VERTEX_CARRY_H
#define KF_RENDERER_VERTEX_CARRY_H
// Where each moving part of a game update was in the update before, taken from
// the game as it draws, so smoothing can show the part in between:
//   - a model drawn in the world: its model-to-world transform at both updates
//     (ModelMove) and each posed vertex's way back in model space, so an
//     animation, a scale and a turn blend as the game's own data does;
//   - camera-fixed and 2D points (the held weapon, the item display, the HUD
//     bars): each point's way back on screen;
//   - the compass needle: a dial that turns with the shown view (DialTurn).
// A part is matched to the one the same thing drew in the update before: the
// same tag, the same nth drawing under it, the same identity and vertex count.
// A part with no match, or that stepped farther or turned more than one update
// can (a warp, a new occupant of the slot), carries nothing.
#include <kf/lib/geometry_types.h>
#include <kf/renderer/frontend.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace kf {
// Who drew a part. tag: the game's number for the thing (render_entity_tag; 0:
// nothing that moves, never carried); identity: what occupies it (a model or
// type), so a slot taken over by something else starts afresh.
struct PartOwner {
    u16 tag;
    u32 identity;
};

class VertexCarry {
public:
    // A game update starts; the last one seen from a camera is what parts are
    // carried from.
    void begin_update();
    // The camera the update is drawn from. Until it is given (a 2D update),
    // nothing is carried.
    void seen_from(const CameraPose &camera);

    // A model drawn in the world. model: its model-to-camera transform, as the
    // game projects it; vertices: its posed vertices. back stays valid until
    // the next call.
    CarriedModel carry_model(PartOwner owner, const MATRIX &model, std::span<const SVECTOR> vertices);
    // Camera-fixed or 2D points, in screen pixels. Returns each point's way back
    // (empty: not carried), valid until the next call.
    std::span<const std::array<float, 2>> carry_screen(PartOwner owner, std::span<const std::array<float, 2>> points);
    // The compass needle, drawn about this screen point. Returns its part, 0 in a
    // 2D update.
    u16 add_dial(float pivot_x, float pivot_y);

    // This update's parts: GpuVertex::part n is parts()[n - 1]. Empty in a 2D update.
    std::span<const PartMove> parts() const;

private:
    struct Drawing {
        u32 identity = 0;
        std::array<double, 12> to_world{};      // models: model-to-world, row-major 3 x 4
        std::vector<SVECTOR> vertices;          // models
        std::vector<std::array<float, 2>> points;   // screen parts
    };
    struct Update {
        std::optional<CameraPose> camera;
        std::unordered_map<u32, Drawing> drawings;   // by tag << 16 | nth drawing under it
        std::unordered_map<u16, u16> drawn;          // drawings so far, by tag
        std::vector<PartMove> parts;
    };
    // The key of owner's next drawing in the current update.
    u32 next_key(u16 tag);
    const Drawing *drawn_before(u32 key, u32 identity) const;

    Update current_, previous_;
    std::vector<std::array<float, 3>> model_back_;
    std::vector<std::array<float, 2>> screen_back_;
};
} // namespace kf

#endif // KF_RENDERER_VERTEX_CARRY_H
