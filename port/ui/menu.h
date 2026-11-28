#ifndef KF_UI_MENU_H
#define KF_UI_MENU_H
// The F1 menu (port/ui/res/menu.rml): the option table's pages as tabs, moved
// about by keys, a pad, the pointer or a finger (verdite/menu_model.h), with
// the Controls page's bindings through the capture (binding_capture.cpp).
// Game thread.
#include <SDL3/SDL.h>

#include <string_view>

namespace verdite::ui {
struct Action;
}

namespace kf::ext {
[[nodiscard]] bool menu_open();
void menu_set_open(bool open);
// Opens on the page of that title (KF_TEST_SETTINGS), or its first.
void menu_open_page(std::string_view title);
// Keys and pad buttons while the menu is open: true when it took them.
bool menu_event(const SDL_Event &event);
// A press or hover on the menu's document.
void menu_action(const verdite::ui::Action &action);
// Each frame before the interface is drawn: the rows read again, held keys
// repeated, the document's parts written.
void menu_frame();
} // namespace kf::ext

#endif // KF_UI_MENU_H
