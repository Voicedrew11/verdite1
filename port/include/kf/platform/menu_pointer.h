#ifndef KF_PLATFORM_MENU_POINTER_H
#define KF_PLATFORM_MENU_POINTER_H
// The mouse (later a finger) on the game's menus, through verdite/menu_pointer.h.
// The game's drawing names each selectable row as it draws it, and each menu
// loop asks for the row the pointer took beside its Up and Down; Confirm, Back
// and the wheel arrive through host_read_buttons, so every loop's own arms run
// (sounds, the confirm-on-Return quirks, screens with no cancel).
// Positions are the original 320 x 240 pixels; the widescreen columns are
// outside them and count as off the menu.
#include <kf/lib/types.h>

#include <optional>

struct SDL_Window;
union SDL_Event;
namespace verdite { class MenuPointer; }

namespace kf {
enum class MenuWidget : u8 { Window, List, Choice };

// While drawing: a widget, then its rows. Returns whether the pointer holds
// the row, for the drawing to show it pressed.
void host_menu_widget(MenuWidget widget, s32 cursor_row);
bool host_menu_row(s32 row, s32 x, s32 y, s32 width, s32 height);

// In a menu loop: the row the pointer took, once, if this widget is the one
// on screen taking input.
std::optional<s32> host_menu_pointed_row(MenuWidget widget);

// Settings: Controls > Mouse in menus (controls.menu_pointer).
inline bool menu_pointer_enabled = true;

// The host's side. Mouse events while a menu is up and the mouse is free; SDL's
// mouse events made from touches are left to the touch controls.
void host_menu_pointer_event(SDL_Window *window, const SDL_Event &event);
// Focus lost or no menu up any more: no pointer, and nothing it asked for is delivered.
void host_menu_pointer_gone();
void host_menu_frame_begin();
void host_menu_frame_shown();
// The press for this pad read as game buttons, or 0.
u32 host_menu_pointer_buttons();
// The pointer itself, for the agent channel's point and click (positions in game pixels).
verdite::MenuPointer &host_menu_pointer();
}

#endif // KF_PLATFORM_MENU_POINTER_H
