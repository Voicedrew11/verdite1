#ifndef KF_UI_MOD_UI_H
#define KF_UI_MOD_UI_H
// The mods' ui table, API 2.0 (docs/MODDING.md): shapes and text on the
// interface's canvases in window pixels, and a mod's own RmlUi pages from its
// folder, whose presses come back to that mod as on_ui_action. mods.cpp binds
// the table, opens a drawing frame around each mod's on_draw and raises the
// presses. Game thread.
#include <verdite/mods.h>

#include <optional>
#include <string>
#include <string_view>

namespace verdite::ui {
struct Action;
}

namespace kf::ext {

verdite::ModTable ui_table();
// on_ui_action's fields, for mods.cpp's event table.
verdite::ModEvent ui_action_event();

// One mod may draw until the frame closes. Each frame starts on the foreground
// layer; closing it pops any clip the mod left pushed.
void ui_frame_open();
void ui_frame_close();
[[nodiscard]] bool ui_frame_is_open();
// The mouse wheel's steps (up positive) since the last drawn frame, and that
// frame ending: every mod has drawn.
void ui_wheel(float steps);
void ui_frames_done();

// A press or an Enter on a mod's page: the mod and on_ui_action's values
// (document, name, value, kind). Nothing for any other document.
struct ModUiPress {
    std::string mod, document, name, value, kind;
};
[[nodiscard]] std::optional<ModUiPress> ui_mod_press(const verdite::ui::Action &action);
// A mod's Lua state went: its pages and its folder go with it.
void ui_mod_forget(std::string_view id);
// Each frame: mods' pages show only while mods run (none in coop).
void ui_mod_pages_shown(bool mods_running);

} // namespace kf::ext

#endif // KF_UI_MOD_UI_H
