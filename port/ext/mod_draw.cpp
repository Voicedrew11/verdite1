#include "mod_draw.h"

#include "internal.hpp"
#include <kf/renderer/frontend.h>
#include <imgui.h>
#include <lauxlib.h>
#include <lua.h>

#include <array>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace kf::ext {
namespace {

enum class Layer : unsigned char { Foreground, Background };
constexpr std::array<const char *, 2> layer_names{"foreground", "background"};
constexpr lua_Integer max_polygon_points = 64;

struct DrawFrame {
    bool open = false;
    Layer layer = Layer::Foreground;
    std::array<int, 2> clips{};   // pushed on each layer's list
    std::vector<UiScope> scopes;  // begun and not yet ended, outermost first
};
DrawFrame frame;

struct ScopeNames {
    const char *begin;
    const char *end;
};
constexpr std::array<ScopeNames, 4> scope_names{{
    {"begin_window", "end_window"},
    {"begin_scroll", "end_scroll"},
    {"begin_tab_bar", "end_tab_bar"},
    {"begin_tab", "end_tab"},
}};

void end_imgui(UiScope scope) {
    switch (scope) {
    case UiScope::Window:
        ImGui::End();
        return;
    case UiScope::Scroll:
        ImGui::EndChild();
        return;
    case UiScope::TabBar:
        ImGui::EndTabBar();
        return;
    case UiScope::Tab:
        ImGui::EndTabItem();
        return;
    }
}

ImDrawList *list_of(Layer layer) {
    return layer == Layer::Background ? ImGui::GetBackgroundDrawList() : ImGui::GetForegroundDrawList();
}

ImDrawList *current_list() { return list_of(frame.layer); }

float check_float(lua_State *L, int index) {
    const lua_Number value = luaL_checknumber(L, index);
    if (!std::isfinite(value))
        luaL_error(L, "argument %d is not a finite number", index);
    return static_cast<float>(value);
}

float optional_float(lua_State *L, int index, float otherwise) {
    return lua_isnoneornil(L, index) ? otherwise : check_float(L, index);
}

ImVec2 check_point(lua_State *L, int index) {
    return {check_float(L, index), check_float(L, index + 1)};
}

// r, g, b, a from first on, each 0..1 and 1 when absent; all absent is the fallback.
ImU32 check_color(lua_State *L, int first, ImU32 fallback) {
    if (lua_isnoneornil(L, first))
        return fallback;
    const auto channel = [&](int i) { return optional_float(L, first + i, 1.0f); };
    return ImGui::GetColorU32(ImVec4(channel(0), channel(1), channel(2), channel(3)));
}

bool optional_filled(lua_State *L, int index) {
    return lua_isnoneornil(L, index) || lua_toboolean(L, index);
}

float optional_thickness(lua_State *L, int index) {
    const float thickness = optional_float(L, index, 1.0f);
    if (thickness <= 0.0f || thickness > 1000.0f)
        luaL_error(L, "thickness %f is not above 0 and at most 1000", static_cast<double>(thickness));
    return thickness;
}

// The mouse belongs to the overlay while it is open.
bool mouse_for_mods() { return !overlay_captures_input(); }

int ui_draw_text(lua_State *L) {
    ui_require_frame(L);
    const ImVec2 at = check_point(L, 1);
    const char *text = luaL_checkstring(L, 3);
    const ImU32 color = check_color(L, 4, IM_COL32_WHITE);
    // An explicit pixel size keeps HUD text independent of the interface scale.
    const float size = optional_float(L, 8, hud_font_size);
    ImFont *font = ImGui::GetFont();
    ImDrawList *list = current_list();
    list->AddText(font, size, ImVec2(at.x + 1, at.y + 1), IM_COL32(0, 0, 0, 200), text);
    list->AddText(font, size, at, color, text);
    return 0;
}

int ui_draw_rect(lua_State *L) {
    ui_require_frame(L);
    const ImVec2 at = check_point(L, 1);
    const ImVec2 to(at.x + check_float(L, 3), at.y + check_float(L, 4));
    const ImU32 color = check_color(L, 5, IM_COL32_WHITE);
    if (optional_filled(L, 9))
        current_list()->AddRectFilled(at, to, color);
    else
        current_list()->AddRect(at, to, color);
    return 0;
}

int ui_draw_line(lua_State *L) {
    ui_require_frame(L);
    const ImVec2 from = check_point(L, 1), to = check_point(L, 3);
    current_list()->AddLine(from, to, check_color(L, 5, IM_COL32_WHITE), optional_thickness(L, 9));
    return 0;
}

int ui_draw_circle(lua_State *L) {
    ui_require_frame(L);
    const ImVec2 centre = check_point(L, 1);
    const float radius = check_float(L, 3);
    const ImU32 color = check_color(L, 4, IM_COL32_WHITE);
    if (optional_filled(L, 8))
        current_list()->AddCircleFilled(centre, radius, color);
    else
        current_list()->AddCircle(centre, radius, color, 0, optional_thickness(L, 9));
    return 0;
}

// {x1, y1, x2, y2, ...}: 3 to 64 points.
std::vector<ImVec2> check_points(lua_State *L, int index) {
    luaL_checktype(L, index, LUA_TTABLE);
    const lua_Integer count = luaL_len(L, index);
    if (count % 2 != 0 || count < 6 || count > 2 * max_polygon_points)
        luaL_error(L, "a polygon is 3 to %I points as x, y numbers (got %I numbers)", max_polygon_points, count);
    std::vector<ImVec2> points;
    points.reserve(static_cast<std::size_t>(count / 2));
    for (lua_Integer i = 1; i <= count; i += 2) {
        lua_rawgeti(L, index, i);
        lua_rawgeti(L, index, i + 1);
        points.push_back(check_point(L, -2));
        lua_pop(L, 2);
    }
    return points;
}

int ui_draw_polygon(lua_State *L) {
    ui_require_frame(L);
    const std::vector<ImVec2> points = check_points(L, 1);
    const ImU32 color = check_color(L, 2, IM_COL32_WHITE);
    const int count = static_cast<int>(points.size());
    if (optional_filled(L, 6))
        current_list()->AddConvexPolyFilled(points.data(), count, color);
    else
        current_list()->AddPolyline(points.data(), count, color, ImDrawFlags_Closed, optional_thickness(L, 7));
    return 0;
}

int ui_push_clip(lua_State *L) {
    ui_require_frame(L);
    const ImVec2 at = check_point(L, 1);
    current_list()->PushClipRect(at, ImVec2(at.x + check_float(L, 3), at.y + check_float(L, 4)), true);
    ++frame.clips[static_cast<std::size_t>(frame.layer)];
    return 0;
}

int ui_pop_clip(lua_State *L) {
    ui_require_frame(L);
    int &clips = frame.clips[static_cast<std::size_t>(frame.layer)];
    if (clips == 0)
        luaL_error(L, "pop_clip without a push_clip on the %s layer", layer_names[static_cast<std::size_t>(frame.layer)]);
    current_list()->PopClipRect();
    --clips;
    return 0;
}

int ui_layer(lua_State *L) {
    ui_require_frame(L);
    const char *name = luaL_checkstring(L, 1);
    for (std::size_t i = 0; i < layer_names.size(); ++i)
        if (std::strcmp(name, layer_names[i]) == 0) {
            frame.layer = static_cast<Layer>(i);
            return 0;
        }
    return luaL_error(L, "there is no layer '%s' (\"foreground\" or \"background\")", name);
}

int ui_text_size(lua_State *L) {
    ui_require_frame(L);
    const char *text = luaL_checkstring(L, 1);
    const float size = optional_float(L, 2, hud_font_size);
    const ImVec2 extent = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    lua_pushnumber(L, extent.x);
    lua_pushnumber(L, extent.y);
    return 2;
}

int ui_mouse(lua_State *L) {
    ui_require_frame(L);
    const ImVec2 at = ImGui::GetIO().MousePos;
    lua_pushnumber(L, at.x);
    lua_pushnumber(L, at.y);
    return 2;
}

int ui_mouse_down(lua_State *L) {
    ui_require_frame(L);
    const lua_Integer button = luaL_checkinteger(L, 1);
    if (button < 1 || button > 3)
        luaL_error(L, "mouse button %I is not 1 (left), 2 (right) or 3 (middle)", button);
    lua_pushboolean(L, mouse_for_mods() && ImGui::IsMouseDown(static_cast<ImGuiMouseButton>(button - 1)));
    return 1;
}

int ui_mouse_wheel(lua_State *L) {
    ui_require_frame(L);
    lua_pushnumber(L, mouse_for_mods() ? ImGui::GetIO().MouseWheel : 0.0f);
    return 1;
}

// Maps the PS1's 320x240 picture coordinates onto the window (the original area
// sits centred in a widescreen picture).
int ui_game_to_screen(lua_State *L) {
    const auto view = original_view();
    lua_pushnumber(L, view.x + luaL_checknumber(L, 1) * view.w / 320.0);
    lua_pushnumber(L, view.y + luaL_checknumber(L, 2) * view.h / 240.0);
    return 2;
}

// A world point through the camera the frame shows, in window points, or nil.
int ui_world_to_screen(lua_State *L) {
    ui_require_frame(L);
    const std::array<double, 3> world{luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3)};
    for (const double value : world) {
        if (!std::isfinite(value))
            luaL_error(L, "world coordinates must be finite numbers");
    }
    const auto point = kf::renderer_world_to_picture(world);
    if (!point) {
        lua_pushnil(L);
        lua_pushnil(L);
        return 2;
    }
    const auto view = original_view();
    lua_pushnumber(L, view.x + (*point)[0] * view.w / 320.0);
    lua_pushnumber(L, view.y + (*point)[1] * view.h / 240.0);
    return 2;
}

} // namespace

void ui_frame_open() {
    frame = DrawFrame{};
    frame.open = true;
}

void ui_scope_begin(UiScope scope) {
    frame.scopes.push_back(scope);
}

void ui_scope_end(lua_State *L, UiScope scope) {
    if (frame.scopes.empty() || frame.scopes.back() != scope) {
        const ScopeNames &names = scope_names[static_cast<std::size_t>(scope)];
        luaL_error(L, "ui.%s has no open ui.%s to end", names.end, names.begin);
    }
    frame.scopes.pop_back();
    end_imgui(scope);
}

void ui_frame_close() {
    // A scope the mod left open is ended here, innermost first, so ImGui is whole for the next
    // frame.
    while (!frame.scopes.empty()) {
        const UiScope scope = frame.scopes.back();
        std::printf("[mods] ui.%s was not called; ending it\n",
                    scope_names[static_cast<std::size_t>(scope)].end);
        frame.scopes.pop_back();
        end_imgui(scope);
    }
    for (std::size_t i = 0; i < frame.clips.size(); ++i)
        for (; frame.clips[i] > 0; --frame.clips[i])
            list_of(static_cast<Layer>(i))->PopClipRect();
    frame.open = false;
}

void ui_require_frame(lua_State *L) {
    if (!frame.open)
        luaL_error(L, "ui.* functions only work inside on_draw or on_settings");
}

bool ui_frame_is_open() { return frame.open; }

std::vector<verdite::ModFunction> ui_draw_functions() {
    using verdite::ModFunction;
    return {
        ModFunction{"draw_text",
                    "fun(x: number, y: number, text: string, r?: number, g?: number, b?: number, a?: number, "
                    "size?: number)",
                    "Window pixels, with a shadow; size in pixels (18).", ui_draw_text},
        {"draw_rect",
         "fun(x: number, y: number, w: number, h: number, r?: number, g?: number, b?: number, a?: number, "
         "filled?: boolean)",
         "", ui_draw_rect},
        {"draw_line",
         "fun(x1: number, y1: number, x2: number, y2: number, r?: number, g?: number, b?: number, a?: number, "
         "thickness?: number)",
         "Thickness in pixels (1).", ui_draw_line},
        {"draw_circle",
         "fun(x: number, y: number, radius: number, r?: number, g?: number, b?: number, a?: number, "
         "filled?: boolean, thickness?: number)",
         "Filled unless filled is false; thickness for an outline (1).", ui_draw_circle},
        {"draw_polygon",
         "fun(points: number[], r?: number, g?: number, b?: number, a?: number, filled?: boolean, "
         "thickness?: number)",
         "points is {x1, y1, x2, y2, ...}, 3 to 64 points; a filled polygon must be convex.", ui_draw_polygon},
        {"push_clip", "fun(x: number, y: number, w: number, h: number)",
         "Clips the layer's drawing to a rectangle until pop_clip; on_draw's end pops any left.", ui_push_clip},
        {"pop_clip", "fun()", "", ui_pop_clip},
        {"layer", "fun(name: \"foreground\"|\"background\")",
         "Where the draw_* calls after it go: over everything (the default), or under the windows.", ui_layer},
        {"text_size", "fun(text: string, size?: number): number, number", "Width and height in pixels.",
         ui_text_size},
        {"mouse", "fun(): number, number", "The cursor in window pixels; far off screen when there is no mouse.", ui_mouse},
        {"mouse_down", "fun(button: integer): boolean",
         "1 left, 2 right, 3 middle; false while the overlay is open.", ui_mouse_down},
        {"mouse_wheel", "fun(): number", "The wheel's steps this frame, up positive; 0 while the overlay is open.",
         ui_mouse_wheel},
        {"game_to_screen", "fun(x: number, y: number): number, number",
         "The PS1's 320x240 coordinates to window pixels.", ui_game_to_screen},
        {"world_to_screen", "fun(x: number, y: number, z: number): number?, number?",
         "A world point in window pixels, through the camera as the frame shows it; nil, nil behind the "
         "camera or in a menu.", ui_world_to_screen},
    };
}

} // namespace kf::ext
