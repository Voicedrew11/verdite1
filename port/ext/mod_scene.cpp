#include "mod_scene.h"

#include "mod_draw.h"
#include "mod_scene_shapes.h"

#include <kf/platform/mod_scene.h>
#include <kf/platform/prelude.h>
#include <lauxlib.h>
#include <lua.h>

#include <cmath>
#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <variant>

namespace kf::ext {
namespace {
constexpr std::size_t shape_capacity = 4096;   // per mod, per update
constexpr double width_max = 1024;             // world units
constexpr double size_max = 4096;

struct StoredShape {
    SceneShape shape;
    CVECTOR colour{};
    BlendMode blend = BlendMode::opaque;
};

// Each mod's shapes, by id, in the order they were drawn. Only the main thread touches them.
std::map<std::string, std::vector<StoredShape>, std::less<>> shapes_of;

struct Tint {
    u8 r = 255, g = 255, b = 255;
    double alpha = 1;
};

s32 check_coordinate(lua_State *L, int index) {
    const lua_Integer value = luaL_checkinteger(L, index);
    if (!std::in_range<s32>(value))
        luaL_error(L, "coordinate %I is not a 32-bit integer", value);
    return static_cast<s32>(value);
}

double optional_size(lua_State *L, int index, double fallback, double largest, const char *name) {
    if (lua_isnoneornil(L, index))
        return fallback;
    const auto value = static_cast<double>(luaL_checknumber(L, index));
    const bool in_range = value > 0 && value <= largest;   // NaN is out of range too
    if (!in_range)
        luaL_error(L, "%s %f is not above 0 and at most %f", name, value, largest);
    return value;
}

// r, g, b, a from first on, each 0..1; a is 1 and the colour white when absent.
Tint check_tint(lua_State *L, int first) {
    const auto channel = [L, first](int offset, double fallback) {
        if (lua_isnoneornil(L, first + offset))
            return fallback;
        const auto value = static_cast<double>(luaL_checknumber(L, first + offset));
        const bool in_range = value >= 0 && value <= 1;   // NaN is out of range too
        if (!in_range)
            luaL_error(L, "colour %f is not 0 to 1", value);
        return value;
    };
    Tint tint;
    tint.r = static_cast<u8>(std::lround(channel(0, 1) * 255));
    tint.g = static_cast<u8>(std::lround(channel(1, 1) * 255));
    tint.b = static_cast<u8>(std::lround(channel(2, 1) * 255));
    tint.alpha = channel(3, 1);
    return tint;
}

// The PS1's blends are not alpha: a translucent shape is added with its colour scaled by alpha.
StoredShape stored(const SceneShape &shape, const Tint &tint) {
    StoredShape out{shape, {tint.r, tint.g, tint.b, 0}, BlendMode::opaque};
    if (tint.alpha < 1) {
        out.blend = BlendMode::add;
        out.colour.r = static_cast<u8>(std::lround(tint.r * tint.alpha));
        out.colour.g = static_cast<u8>(std::lround(tint.g * tint.alpha));
        out.colour.b = static_cast<u8>(std::lround(tint.b * tint.alpha));
    }
    return out;
}

void refuse_in_ui_frame(lua_State *L) {
    if (ui_frame_is_open())
        luaL_error(L, "kf.scene works in on_update, not on_draw or on_settings");
}

void add_shape(lua_State *L, StoredShape shape) {
    const std::string_view id = verdite::mod_id(L);
    auto found = shapes_of.find(id);
    if (found == shapes_of.end())
        found = shapes_of.emplace(std::string(id), std::vector<StoredShape>{}).first;
    if (found->second.size() >= shape_capacity)
        luaL_error(L, "kf.scene: more than %d shapes in one update", static_cast<int>(shape_capacity));
    found->second.push_back(shape);
}

// kf.scene.line(x1, y1, z1, x2, y2, z2 [, r, g, b, a [, width]])
int scene_line(lua_State *L) {
    refuse_in_ui_frame(L);
    SceneLine line;
    line.from = {check_coordinate(L, 1), check_coordinate(L, 2), check_coordinate(L, 3)};
    line.to = {check_coordinate(L, 4), check_coordinate(L, 5), check_coordinate(L, 6)};
    line.width = optional_size(L, 11, 1, width_max, "width");
    add_shape(L, stored(line, check_tint(L, 7)));
    return 0;
}

// kf.scene.marker(x, y, z [, size [, r, g, b, a]])
int scene_marker(lua_State *L) {
    refuse_in_ui_frame(L);
    SceneMarker marker;
    marker.centre = {check_coordinate(L, 1), check_coordinate(L, 2), check_coordinate(L, 3)};
    marker.size = optional_size(L, 4, 16, size_max, "size");
    add_shape(L, stored(marker, check_tint(L, 5)));
    return 0;
}

ModSceneFace face_of(const SceneQuad &quad, const StoredShape &shape) {
    const auto svector = [](const ViewOffset &offset) { return SVECTOR{offset[0], offset[1], offset[2]}; };
    ModSceneFace face;
    for (std::size_t i = 0; i < face.corners.size(); ++i)
        face.corners[i] = svector(quad.corners[i]);
    face.centre = svector(quad.centre);
    face.colour = shape.colour;
    face.blend = shape.blend;
    return face;
}
} // namespace

std::vector<verdite::ModFunction> mod_scene_functions() {
    using verdite::ModFunction;
    return {
        ModFunction{"line",
                    "fun(x1: integer, y1: integer, z1: integer, x2: integer, y2: integer, z2: integer, r?: number, "
                    "g?: number, b?: number, a?: number, width?: number)",
                    "A line in the world, from on_update; it shows in the next update's picture. Colours 0-1, "
                    "width in world units (above 0, at most 1024, default 1). Walls hide it.",
                    scene_line},
        {"marker",
         "fun(x: integer, y: integer, z: integer, size?: number, r?: number, g?: number, b?: number, a?: number)",
         "A square facing the camera, from on_update, size in world units (above 0, at most 4096, default 16). Colours 0-1. Walls hide it.",
         scene_marker},
    };
}

void mod_scene_clear_mod(std::string_view id) {
    if (const auto found = shapes_of.find(id); found != shapes_of.end())
        shapes_of.erase(found);
}

void mod_scene_clear_all() { shapes_of.clear(); }

} // namespace kf::ext

namespace kf {

std::vector<ModSceneFace> mod_scene_faces(const VECTOR &view_position, const MATRIX &view_matrix) {
    const ext::SceneView view{view_position, view_matrix};
    std::vector<ModSceneFace> faces;
    for (const auto &[id, shapes] : ext::shapes_of) {
        for (const ext::StoredShape &shape : shapes) {
            if (const auto quad = ext::scene_quad(view, shape.shape))
                faces.push_back(ext::face_of(*quad, shape));
        }
    }
    return faces;
}

} // namespace kf
