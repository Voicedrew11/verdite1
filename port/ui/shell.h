#ifndef KF_UI_SHELL_H
#define KF_UI_SHELL_H
// KF1's interface on libverdite's RmlUi layer (verdite/ui.h): the one Screen,
// made after the window, fed the window's input, and drawn into a frame for
// each present. Its documents are KF1's (port/ui/res/) and its fonts
// libverdite's (libverdite/res/fonts/), carried in the binary. The F1 menu,
// the setup, the notices and the HUD draw through it. Game thread.
#include <SDL3/SDL.h>

namespace verdite::ui {
class Screen;
struct Frame;
} // namespace verdite::ui

namespace kf::ext {
// After the window exists; false, logged, if RmlUi or a font does not start
// (the game runs on without the interface).
bool interface_start(SDL_Window *window);
void interface_stop();
// The Screen, or null before the start, headless, or after a failed one.
verdite::ui::Screen *interface_screen();
// The pointer, a finger or typing for the interface: true when it took the
// event (over a document, or into a text field) and the game must not see it.
bool interface_event(const SDL_Event &event);
// The interface laid out at the window's pixel size and recorded; null when
// there is no Screen.
const verdite::ui::Frame *interface_frame(int width_px, int height_px);
} // namespace kf::ext

#endif // KF_UI_SHELL_H
