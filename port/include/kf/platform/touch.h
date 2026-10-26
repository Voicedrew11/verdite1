#ifndef KF_PLATFORM_TOUCH_H
#define KF_PLATFORM_TOUCH_H
// KF1's on-screen controls on a touch screen, through verdite/touch.h. Which
// controls take a finger follows the game's input context:
//
//   gameplay            a move stick, a look area and buttons, drawn
//   menus               a finger points at the rows (menu_pointer.h); on a
//                       screen with no rows a tap is Confirm
//   opening, scripted   a tap anywhere is Confirm
//
// Only gameplay's controls are drawn, and only while the player is on the
// touch screen: a gamepad, a keyboard or a mouse in use fades them out and a
// touch brings them back. The overlay (F1) takes every touch while it is open.
#include <kf/lib/types.h>
#include <kf/platform/analog.h>

#include <span>

struct SDL_Window;
union SDL_Event;

namespace kf {

// What covers the game: the overlay takes every finger; the full map leaves
// only its own button, to close it.
enum class TouchCover : u8 { None, FullMap, Overlay };

// Port commands with no game button, for the overlay to carry out.
enum class TouchCommand : u8 { None, Settings, FullMap };

// The host's side. Every event: fingers, and the other devices that hide the controls.
void host_touch_event(SDL_Window *window, const SDL_Event &event);
// Once a poll, after the events: the controls for this context, and the look
// dragged since the last poll, in degrees (right and down positive).
struct TouchLook { double yaw_degrees, pitch_degrees; };
TouchLook host_touch_poll(SDL_Window *window, TouchCover cover);

// The game's actions (controls.h bits) held on the screen. take adds the
// presses lifted since the last take, so a quick tap reaches one pad read.
struct TouchActions { u32 held, pressed; };
TouchActions host_touch_take_actions();
u32 host_touch_held_actions();
// The move stick, -1..1, for the analog path beside the gamepad's left stick.
RawSticks host_touch_sticks();
TouchCommand host_touch_take_command();

// For drawing, in window pixels. Empty while nothing is to be drawn.
enum class TouchShape : u8 { Stick, Button, Gear };
struct TouchShown {
    TouchShape shape;
    const char *label;
    float x_px, y_px, radius_px;   // a stick: its base, where it rests or was centred
    bool held;
    float knob_x_px, knob_y_px;    // a stick's thumb
};
std::span<const TouchShown> host_touch_shown();
// How visible they are, 0..1: they fade rather than vanish.
float host_touch_opacity();

} // namespace kf

#endif // KF_PLATFORM_TOUCH_H
