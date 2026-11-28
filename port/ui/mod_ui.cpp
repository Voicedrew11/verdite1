// The mods' ui table, API 2.0 (mod_ui.h)
#include "mod_ui.h"

#include "internal.hpp"
#include "shell.h"

#include <kf/renderer/frontend.h>
#include <verdite/ui.h>
#include <verdite/ui_canvas.h>

#include <lauxlib.h>
#include <lua.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <vector>

namespace kf::ext {
namespace {

using verdite::ui::Canvas;
using verdite::ui::Colour;
using verdite::ui::Layer;
using verdite::ui::Point;

constexpr std::array<const char *, 2> layer_names{"foreground", "background"};
constexpr std::array<Layer, 2> layers{Layer::Front, Layer::Back};
constexpr lua_Integer max_polygon_points = 64;
constexpr std::string_view pages_prefix = "mods/";

struct DrawFrame {
    bool open = false;
    std::size_t layer = 0;       // into layers
    std::array<int, 2> clips{};  // pushed on each layer
};
DrawFrame frame;
float wheel_steps = 0;

// A page a mod loaded, by its path on the Screen ("mods/<id>/<handle>").
struct ModPage {
    std::string mod;
    std::string handle;     // the path the mod gave, as it names the page
    bool wanted = false;    // the mod showed it
};
std::map<std::string, ModPage, std::less<>> pages;
std::set<std::string, std::less<>> mounted;   // mods whose folder the Screen reads

// ---- arguments ----

float check_float(lua_State *L, int index) {
    const lua_Number value = luaL_checknumber(L, index);
    if (!std::isfinite(value))
        luaL_error(L, "argument %d is not a finite number", index);
    return static_cast<float>(value);
}

float optional_float(lua_State *L, int index, float otherwise) {
    return lua_isnoneornil(L, index) ? otherwise : check_float(L, index);
}

Point check_point(lua_State *L, int index) {
    return {check_float(L, index), check_float(L, index + 1)};
}

// r, g, b, a from first on, each 0..1 and 1 when absent.
Colour check_colour(lua_State *L, int first) {
    const auto channel = [&](int i) { return optional_float(L, first + i, 1.0f); };
    return {channel(0), channel(1), channel(2), channel(3)};
}

verdite::ui::Fill optional_fill(lua_State *L, int index) {
    return lua_isnoneornil(L, index) || lua_toboolean(L, index) ? verdite::ui::Fill::Solid
                                                                : verdite::ui::Fill::Outline;
}

float optional_thickness(lua_State *L, int index) {
    const float thickness = optional_float(L, index, 1.0f);
    if (thickness <= 0.0f || thickness > 1000.0f)
        luaL_error(L, "thickness %f is not above 0 and at most 1000", static_cast<double>(thickness));
    return thickness;
}

void require_frame(lua_State *L) {
    if (!frame.open)
        luaL_error(L, "ui.draw_* and the calls with them only work inside on_draw");
}

// The canvas the frame draws on; null without the interface (headless), where drawing does nothing.
Canvas *canvas() {
    verdite::ui::Screen *screen = interface_screen();
    return screen ? &screen->canvas(layers[frame.layer]) : nullptr;
}

// The mouse belongs to the menus while one is open.
bool mouse_for_mods() { return !overlay_captures_input(); }

float pixel_density() {
    const float density = window() ? SDL_GetWindowPixelDensity(window()) : 0.0f;
    return density > 0 ? density : 1.0f;
}

// ---- drawing ----

int ui_draw_text(lua_State *L) {
    require_frame(L);
    const Point at = check_point(L, 1);
    const char *text = luaL_checkstring(L, 3);
    const Colour colour = check_colour(L, 4);
    // A pixel size of its own keeps HUD text apart from the interface scale.
    const float size = optional_float(L, 8, hud_font_size);
    if (Canvas *to = canvas())
        to->text(at, text, colour, size, verdite::ui::Shadow::Drop);
    return 0;
}

int ui_draw_rect(lua_State *L) {
    require_frame(L);
    const Point at = check_point(L, 1);
    const verdite::ui::Area area{at.x, at.y, check_float(L, 3), check_float(L, 4)};
    const Colour colour = check_colour(L, 5);
    const verdite::ui::Fill fill = optional_fill(L, 9);
    if (Canvas *to = canvas())
        to->rect(area, colour, fill);
    return 0;
}

int ui_draw_line(lua_State *L) {
    require_frame(L);
    const Point from = check_point(L, 1);
    const Point to_point = check_point(L, 3);
    const Colour colour = check_colour(L, 5);
    const float thickness = optional_thickness(L, 9);
    if (Canvas *to = canvas())
        to->line(from, to_point, colour, thickness);
    return 0;
}

int ui_draw_circle(lua_State *L) {
    require_frame(L);
    const Point centre = check_point(L, 1);
    const float radius = check_float(L, 3);
    const Colour colour = check_colour(L, 4);
    const verdite::ui::Fill fill = optional_fill(L, 8);
    const float thickness = optional_thickness(L, 9);
    if (Canvas *to = canvas())
        to->circle(centre, radius, colour, fill, thickness);
    return 0;
}

std::vector<Point> check_points(lua_State *L, int index) {
    luaL_checktype(L, index, LUA_TTABLE);
    const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(L, index));
    if (count % 2 != 0 || count < 6 || count > max_polygon_points * 2)
        luaL_error(L, "points must be 3 to %I x, y pairs", max_polygon_points);
    std::vector<Point> points;
    for (lua_Integer i = 1; i <= count; i += 2) {
        lua_rawgeti(L, index, i);
        lua_rawgeti(L, index, i + 1);
        points.push_back({check_float(L, -2), check_float(L, -1)});
        lua_pop(L, 2);
    }
    return points;
}

int ui_draw_polygon(lua_State *L) {
    require_frame(L);
    const std::vector<Point> points = check_points(L, 1);
    const Colour colour = check_colour(L, 2);
    const verdite::ui::Fill fill = optional_fill(L, 6);
    const float thickness = optional_thickness(L, 7);
    if (Canvas *to = canvas())
        to->polygon(points, colour, fill, thickness);
    return 0;
}

int ui_push_clip(lua_State *L) {
    require_frame(L);
    const Point at = check_point(L, 1);
    const verdite::ui::Area area{at.x, at.y, check_float(L, 3), check_float(L, 4)};
    if (Canvas *to = canvas())
        to->push_clip(area);
    ++frame.clips[frame.layer];
    return 0;
}

int ui_pop_clip(lua_State *L) {
    require_frame(L);
    int &clips = frame.clips[frame.layer];
    if (clips == 0)
        luaL_error(L, "pop_clip without a push_clip on the %s layer", layer_names[frame.layer]);
    if (Canvas *to = canvas())
        to->pop_clip();
    --clips;
    return 0;
}

int ui_layer(lua_State *L) {
    require_frame(L);
    const char *name = luaL_checkstring(L, 1);
    for (std::size_t i = 0; i < layer_names.size(); ++i)
        if (std::strcmp(name, layer_names[i]) == 0) {
            frame.layer = i;
            return 0;
        }
    return luaL_error(L, "there is no layer '%s' (\"foreground\" or \"background\")", name);
}

int ui_text_size(lua_State *L) {
    require_frame(L);
    const char *text = luaL_checkstring(L, 1);
    const float size = optional_float(L, 2, hud_font_size);
    const Canvas *from = canvas();
    const Point extent = from ? from->text_size(text, size) : Point{};
    lua_pushnumber(L, extent.x);
    lua_pushnumber(L, extent.y);
    return 2;
}

int ui_mouse(lua_State *L) {
    require_frame(L);
    float x = -1e6f, y = -1e6f;   // far off screen: no mouse
    SDL_GetMouseState(&x, &y);
    lua_pushnumber(L, x * pixel_density());
    lua_pushnumber(L, y * pixel_density());
    return 2;
}

int ui_mouse_down(lua_State *L) {
    require_frame(L);
    const lua_Integer button = luaL_checkinteger(L, 1);
    if (button < 1 || button > 3)
        luaL_error(L, "mouse button %I is not 1 (left), 2 (right) or 3 (middle)", button);
    // Lua's 2 is the right button, SDL's 3.
    constexpr std::array<int, 3> sdl_buttons{SDL_BUTTON_LEFT, SDL_BUTTON_RIGHT, SDL_BUTTON_MIDDLE};
    const SDL_MouseButtonFlags held = SDL_GetMouseState(nullptr, nullptr);
    lua_pushboolean(L, mouse_for_mods() && (held & SDL_BUTTON_MASK(sdl_buttons[button - 1])) != 0);
    return 1;
}

int ui_mouse_wheel(lua_State *L) {
    require_frame(L);
    lua_pushnumber(L, mouse_for_mods() ? wheel_steps : 0.0f);
    return 1;
}

// The PS1's 320x240 picture on the window (the original area sits centred in a widescreen picture).
int ui_game_to_screen(lua_State *L) {
    const ViewRect view = original_view_px();
    lua_pushnumber(L, view.x + luaL_checknumber(L, 1) * view.w / 320.0);
    lua_pushnumber(L, view.y + luaL_checknumber(L, 2) * view.h / 240.0);
    return 2;
}

// A world point through the camera the frame shows, in window pixels, or nil.
int ui_world_to_screen(lua_State *L) {
    require_frame(L);
    const std::array<double, 3> world{luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3)};
    for (const double value : world)
        if (!std::isfinite(value))
            luaL_error(L, "world coordinates must be finite numbers");
    const auto point = kf::renderer_world_to_picture(world);
    if (!point) {
        lua_pushnil(L);
        lua_pushnil(L);
        return 2;
    }
    const ViewRect view = original_view_px();
    lua_pushnumber(L, view.x + (*point)[0] * view.w / 320.0);
    lua_pushnumber(L, view.y + (*point)[1] * view.h / 240.0);
    return 2;
}

// ---- a mod's pages ----

// A path inside the mod's folder, as the mod names it: relative, '/' between parts, no "..", an .rml.
bool page_handle_fits(std::string_view handle) {
    if (handle.empty() || handle.size() > 200 || handle.front() == '/' || !handle.ends_with(".rml"))
        return false;
    if (handle.find('\\') != std::string_view::npos || handle.find(':') != std::string_view::npos)
        return false;
    std::size_t at = 0;
    while (at <= handle.size()) {
        const std::size_t end = std::min(handle.find('/', at), handle.size());
        const std::string_view part = handle.substr(at, end - at);
        if (part.empty() || part == "." || part == "..")
            return false;
        at = end + 1;
    }
    return true;
}

std::string screen_path(std::string_view mod, std::string_view handle) {
    return std::string(pages_prefix) + std::string(mod) + "/" + std::string(handle);
}

// The calling mod's page by its handle; a Lua error for one it did not load.
std::string check_page(lua_State *L, int index) {
    const char *handle = luaL_checkstring(L, index);
    std::string path = screen_path(verdite::mod_id(L), handle);
    if (!pages.contains(path))
        luaL_error(L, "'%s' is not a page this mod loaded with ui.load", handle);
    return path;
}

// The page's Document, or null headless or when it no longer loads.
verdite::ui::Document *document(const std::string &path) {
    verdite::ui::Screen *screen = interface_screen();
    return screen ? screen->load(path) : nullptr;
}

int ui_load(lua_State *L) {
    const char *handle = luaL_checkstring(L, 1);
    if (!page_handle_fits(handle))
        return luaL_error(L, "'%s' is not an .rml path inside the mod's folder", handle);
    const std::string mod(verdite::mod_id(L));
    const std::string path = screen_path(mod, handle);
    verdite::ui::Screen *screen = interface_screen();
    if (screen && !mounted.contains(mod)) {
        screen->add_folder(std::string(pages_prefix) + mod, mods_dir() / mod);
        mounted.insert(mod);
    }
    if (screen && !screen->load(path)) {
        lua_pushnil(L);
        return 1;
    }
    pages.try_emplace(path, ModPage{mod, handle});
    lua_pushstring(L, handle);
    return 1;
}

int ui_show(lua_State *L) {
    const std::string path = check_page(L, 1);
    pages.find(path)->second.wanted = true;
    if (verdite::ui::Document *page = document(path))
        page->show();
    return 0;
}

int ui_hide(lua_State *L) {
    const std::string path = check_page(L, 1);
    pages.find(path)->second.wanted = false;
    if (verdite::ui::Document *page = document(path))
        page->hide();
    return 0;
}

int ui_shown(lua_State *L) {
    lua_pushboolean(L, pages.find(check_page(L, 1))->second.wanted);
    return 1;
}

// set_text, set_rml, set_classes and set_field: a page, an element id, a string.
template <void (verdite::ui::Document::*set)(std::string_view, std::string_view)>
int ui_set(lua_State *L) {
    const std::string path = check_page(L, 1);
    const char *id = luaL_checkstring(L, 2);
    std::size_t length = 0;
    const char *text = luaL_checklstring(L, 3, &length);
    verdite::mod_spend_instructions(L, static_cast<std::int64_t>(length));
    if (verdite::ui::Document *page = document(path))
        (page->*set)(id, std::string_view(text, length));
    return 0;
}

int ui_field(lua_State *L) {
    const std::string path = check_page(L, 1);
    const char *id = luaL_checkstring(L, 2);
    const verdite::ui::Document *page = document(path);
    const std::string text = page ? page->field(id) : std::string();
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

} // namespace

void ui_frame_open() {
    frame = DrawFrame{};
    frame.open = true;
}

void ui_frame_close() {
    for (std::size_t i = 0; i < frame.clips.size(); ++i) {
        frame.layer = i;
        Canvas *to = canvas();
        for (; frame.clips[i] > 0; --frame.clips[i])
            if (to)
                to->pop_clip();
    }
    frame.open = false;
}

bool ui_frame_is_open() {
    return frame.open;
}

void ui_wheel(float steps) {
    wheel_steps += steps;
}

void ui_frames_done() {
    wheel_steps = 0;
}

std::optional<ModUiPress> ui_mod_press(const verdite::ui::Action &action) {
    if (action.kind == verdite::ui::ActionKind::Hover)
        return std::nullopt;
    const auto found = pages.find(action.document);
    if (found == pages.end())
        return std::nullopt;
    const char *kind = action.kind == verdite::ui::ActionKind::Enter ? "enter" : "press";
    return ModUiPress{found->second.mod, found->second.handle, action.name, action.value, kind};
}

void ui_mod_forget(std::string_view id) {
    verdite::ui::Screen *screen = interface_screen();
    for (auto page = pages.begin(); page != pages.end();) {
        if (page->second.mod != id) {
            ++page;
            continue;
        }
        if (screen)
            screen->unload(page->first);
        page = pages.erase(page);
    }
    if (const auto found = mounted.find(id); found != mounted.end()) {
        if (screen)
            screen->remove_folder(std::string(pages_prefix) + std::string(id));
        mounted.erase(found);
    }
}

void ui_mod_pages_shown(bool mods_running) {
    for (const auto &[path, page] : pages) {
        verdite::ui::Document *shown = document(path);
        if (!shown)
            continue;
        if (page.wanted && mods_running && !shown->shown())
            shown->show();
        else if ((!page.wanted || !mods_running) && shown->shown())
            shown->hide();
    }
}

verdite::ModEvent ui_action_event() {
    return {.name = "on_ui_action",
            .doc = "A press on one of this mod's own pages (ui.load), or Enter in its text field; only the mod "
                   "whose page it is hears it.",
            .fields = {{.name = "document", .type = "string", .doc = "The page, as ui.load named it."},
                       {.name = "name", .type = "string",
                        .doc = "The element's data-action (a press) or data-enter (Enter in a field)."},
                       {.name = "value", .type = "string", .doc = "The element's data-value; \"\" without one."},
                       {.name = "kind", .type = "string", .doc = "\"press\" or \"enter\"."}}};
}

verdite::ModTable ui_table() {
    using verdite::ModFunction;
    using verdite::ui::Document;
    return {
        .path = "ui",
        .doc = "Drawing over the game in on_draw, in window pixels, and the mod's own pages (RmlUi documents "
               "from its folder) at any time. Colours are 0-1.",
        .functions =
            {
                ModFunction{"draw_text",
                            "fun(x: number, y: number, text: string, r?: number, g?: number, b?: number, "
                            "a?: number, size?: number)",
                            "With a shadow; size in pixels (18).", ui_draw_text},
                {"draw_rect",
                 "fun(x: number, y: number, w: number, h: number, r?: number, g?: number, b?: number, a?: number, "
                 "filled?: boolean)",
                 "Filled unless filled is false (then a 1-pixel outline).", ui_draw_rect},
                {"draw_line",
                 "fun(x1: number, y1: number, x2: number, y2: number, r?: number, g?: number, b?: number, "
                 "a?: number, thickness?: number)",
                 "Thickness in pixels (1).", ui_draw_line},
                {"draw_circle",
                 "fun(x: number, y: number, radius: number, r?: number, g?: number, b?: number, a?: number, "
                 "filled?: boolean, thickness?: number)",
                 "Filled unless filled is false; thickness for an outline (1).", ui_draw_circle},
                {"draw_polygon",
                 "fun(points: number[], r?: number, g?: number, b?: number, a?: number, filled?: boolean, "
                 "thickness?: number)",
                 "points is {x1, y1, x2, y2, ...}, 3 to 64 points; a filled polygon must be convex.",
                 ui_draw_polygon},
                {"push_clip", "fun(x: number, y: number, w: number, h: number)",
                 "Clips the layer's drawing to a rectangle until pop_clip; on_draw's end pops any left.",
                 ui_push_clip},
                {"pop_clip", "fun()", "", ui_pop_clip},
                {"layer", "fun(name: \"foreground\"|\"background\")",
                 "Where the draw_* calls after it go: over everything (the default), or under the pages and "
                 "menus.",
                 ui_layer},
                {"text_size", "fun(text: string, size?: number): number, number", "Width and height in pixels.",
                 ui_text_size},
                {"mouse", "fun(): number, number", "The cursor in window pixels.", ui_mouse},
                {"mouse_down", "fun(button: integer): boolean",
                 "1 left, 2 right, 3 middle; false while a menu is open.", ui_mouse_down},
                {"mouse_wheel", "fun(): number",
                 "The wheel's steps since the last drawn frame, up positive; 0 while a menu is open.",
                 ui_mouse_wheel},
                {"game_to_screen", "fun(x: number, y: number): number, number",
                 "The PS1's 320x240 coordinates to window pixels.", ui_game_to_screen},
                {"world_to_screen", "fun(x: number, y: number, z: number): number?, number?",
                 "A world point in window pixels, through the camera as the frame shows it; nil, nil behind "
                 "the camera or in a menu.",
                 ui_world_to_screen},
                {"load", "fun(path: string): string?",
                 "A page (an .rml file) from the mod's folder, loaded hidden; its styles and images come from "
                 "the folder too. Returns the handle the calls below take (the path), or nil when it does not "
                 "load (the log says why). Its presses arrive as on_ui_action.",
                 ui_load},
                {"show", "fun(page: string)", "Shows the page; mods' pages hide while mods are off (coop).",
                 ui_show},
                {"hide", "fun(page: string)", "", ui_hide},
                {"shown", "fun(page: string): boolean", "Whether the mod has it shown.", ui_shown},
                {"set_text", "fun(page: string, id: string, text: string)",
                 "The element's content as plain text.", ui_set<&Document::set_text>},
                {"set_rml", "fun(page: string, id: string, rml: string)",
                 "The element's content as RML; nothing is rebuilt when it is the same as before.",
                 ui_set<&Document::set_rml>},
                {"set_classes", "fun(page: string, id: string, classes: string)",
                 "The element's class attribute, space-separated.", ui_set<&Document::set_classes>},
                {"field", "fun(page: string, id: string): string", "A text field's (<input>) text.", ui_field},
                {"set_field", "fun(page: string, id: string, text: string)", "", ui_set<&Document::set_field>},
            },
        .fields = {},
    };
}

} // namespace kf::ext
