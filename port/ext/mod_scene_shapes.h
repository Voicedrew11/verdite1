#pragma once
// A mod's line or marker as the quad the game draws (kf.scene, docs/MODDING.md): its corners relative
// to the view position, in the game's 16-bit units, clipped first so that nothing wraps. A line is
// cut to the reach and at the near plane; a marker is dropped past either. Corners never reach the
// game at or behind the near plane. No Lua and no game state: the view is passed in.
#include <kf/lib/geometry_types.h>

#include <array>
#include <optional>
#include <variant>

namespace kf::ext {

using WorldPoint = std::array<s32, 3>;   // world units
// A corner relative to the view position: the game's SVECTOR components.
using ViewOffset = std::array<s16, 3>;

struct SceneLine {
    WorldPoint from{}, to{};
    double width = 1;    // world units, across the line
};
struct SceneMarker {
    WorldPoint centre{};
    double size = 1;     // world units, the square's side
};
using SceneShape = std::variant<SceneLine, SceneMarker>;

// The camera: its position, and its rotation as the game's view matrix (camera space = rotation x
// (world - position), 4096 a unit).
struct SceneView {
    VECTOR position;
    MATRIX rotation;
};

// The quad in strip order: corners 0 and 1 at one end of a line (or the marker's bottom row), 2
// and 3 at the other (top row), so (0, 1, 2) and (1, 3, 2) are its two triangles.
struct SceneQuad {
    std::array<ViewOffset, 4> corners{};
    ViewOffset centre{};   // for depth sorting
};

// The quad the shape is drawn as, or none when nothing of it lies in front of the near plane within
// the reach.
std::optional<SceneQuad> scene_quad(const SceneView &view, const SceneShape &shape);

} // namespace kf::ext
