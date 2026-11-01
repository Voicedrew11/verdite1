#pragma once
// The ui table's drawing (docs/MODDING.md): shapes and text over the game picture in window pixels,
// clipping, the layer they go on, and the mouse as the drawing sees it. mods.cpp binds these in its
// ui table and opens a frame around each mod's on_draw and on_settings.
#include <verdite/mods.h>

#include <vector>

namespace kf::ext {

// ui.draw_*, ui.push_clip, ui.pop_clip, ui.layer, ui.text_size, ui.mouse*, ui.game_to_screen.
std::vector<verdite::ModFunction> ui_draw_functions();

// One mod may draw until the frame closes. Each frame starts on the foreground layer; closing it
// pops any clip the mod left pushed.
void ui_frame_open();
void ui_frame_close();
// A Lua error unless a frame is open.
void ui_require_frame(lua_State *L);

} // namespace kf::ext
