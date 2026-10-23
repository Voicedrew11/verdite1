#ifndef KF_PLATFORM_INPUT_H
#define KF_PLATFORM_INPUT_H

#include <kf/lib/types.h>

namespace kf {
enum class Button : u32 {
    Up = 1u << 12, Down = 1u << 14, Left = 1u << 15, Right = 1u << 13,
    Attack = 1u << 4, Back = 1u << 6, Magic = 1u << 7, Confirm = 1u << 5,
    Start = 1u << 8, Select = 1u << 11,
    StrafeRight = 1u << 3, StrafeLeft = 1u << 2,
    LookDown = 1u << 1, LookUp = 1u << 0
};
constexpr u32 button_mask(Button button) { return static_cast<u32>(button); }
constexpr u32 operator&(u32 buttons, Button button) { return buttons & button_mask(button); }
constexpr u32 operator|(Button a, Button b) { return button_mask(a) | button_mask(b); }

constexpr bool button_pressed(u32 current, u32 previous, Button button)
{
    return (current & button) != 0 && (previous & button) == 0;
}

enum class InputContext : u8 { Opening, Gameplay, Menu, Scripted };
struct LookDelta { s32 yaw, pitch; };

u32 host_read_buttons();
void host_wait_buttons_released(u32 mask = ~u32(0));
void host_wait_button_press();
InputContext host_set_input_context(InputContext context);
LookDelta host_take_look();
// Run is a port addition with no PlayStation button, so it stays out of the button mask.
bool host_run_held();
// Quick save/load presses made during gameplay since the last call.
enum class QuickRequest : u8 { None, Save, Load };
QuickRequest host_take_quick_request();
// Intro and title: true when F4 was pressed and a quick save exists. The request
// is kept, so the game loads it on its first update after the opening skips out.
bool host_opening_quick_load();

// Port options (settings.json). Run speed is a percentage of the walking speed.
inline int player_run_speed = 160;
// Run key mode: false holds to run, true toggles running on each press.
inline bool player_run_toggle = false;
// Camera pitch limit in 1/4096 turns; the original is 191 (about 17 degrees).
inline s32 player_pitch_limit = 191;
// Fixes for original-game bugs whose branches never ran (upstream sushi-shi #50, #51).
// Enemies that hit a ceiling mid-jump start falling again.
inline bool fix_actor_ceiling_fall = true;
// Sounds flagged for narrower stereo panning get their volume adjustment.
inline bool fix_spatial_panning = true;
// Classic mode (F1 > Settings): as originally shipped, so also no automap,
// equipment stats or quick save/load.
inline bool classic_mode = false;
// Interacting with a lock uses the matching key if the player carries it.
inline bool auto_use_keys = true;
}

#endif // KF_PLATFORM_INPUT_H
