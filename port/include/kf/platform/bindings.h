#ifndef KF_PLATFORM_BINDINGS_H
#define KF_PLATFORM_BINDINGS_H

#include <kf/platform/controls.h>
#include <kf/platform/input.h>   // PadLayout

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace kf {
// The player's controls per action, in two groups: the keyboard with the mouse, and the pad. The
// player's changes are kept apart from the defaults, so an action never changed follows the
// defaults (the pad's face buttons follow PadLayout) and one changed keeps its list. A control
// does one action per group; a changed list takes it from an unchanged action's defaults. A few
// controls are fixed and never named here as bindable: Escape and Android's Back (pause_or_back),
// the left stick's directions; F1 and F11 belong to the overlay.
enum class BindingGroup : u8 { keys, pad };
inline constexpr std::size_t action_count = static_cast<std::size_t>(Action::count);
inline constexpr std::size_t controls_per_action = 4;
// A pad axis direction as a control (100 + axis * 2, +1 when positive) and when it counts as held.
inline constexpr u32 pad_axis_code_first = 100;
inline constexpr int pad_axis_deadzone = 16000;
constexpr u32 pad_axis_negative(u32 sdl_axis) {
    return pad_axis_code_first + sdl_axis * 2;
}
constexpr u32 pad_axis_positive(u32 sdl_axis) {
    return pad_axis_negative(sdl_axis) + 1;
}

// Up to controls_per_action controls, in the order the player gave them.
struct ActionControls {
    std::array<Control, controls_per_action> list{};
    std::size_t count = 0;
    std::span<const Control> controls() const { return {list.data(), count}; }
    // Whether the used part holds this control.
    bool has(Control control) const;
    // Equal when the used parts are, in order; what lies past count does not matter.
    friend bool operator==(const ActionControls &, const ActionControls &);
};
using GroupBindings = std::array<ActionControls, action_count>;
struct Bindings {
    GroupBindings keys, pad;
    friend bool operator==(const Bindings &, const Bindings &) = default;
};
// What settings.json holds: an action's own list, or nothing for its default.
using GroupChanges = std::array<std::optional<ActionControls>, action_count>;
struct BindingChanges {
    GroupChanges keys, pad;
    friend bool operator==(const BindingChanges &, const BindingChanges &) = default;
};

// The group a control belongs to; the mouse goes with the keyboard.
BindingGroup binding_group(Control control);
// Each action's default controls in the group; only the pad's face buttons depend on the layout.
GroupBindings default_bindings(BindingGroup group, PadLayout layout);
// The controls no player change touches: Escape, Android's Back, the left stick's four directions.
std::span<const InputBinding> fixed_bindings();
// The controls in force: changed lists first (a control an earlier one has is left out), then
// each unchanged action's defaults less what the changed lists have.
GroupBindings bindings_resolve(const GroupBindings &defaults, const GroupChanges &changes);
// Both groups' controls in force for these changes over the layout's defaults.
Bindings bindings_resolve(const BindingChanges &changes, PadLayout layout);
// Gives action exactly controls, taking each from any other changed list; returned tidy.
GroupChanges bindings_set(const GroupBindings &defaults, GroupChanges changes, Action action,
                          const ActionControls &controls);
// Changes as bindings_set leaves them: a control in two lists stays in the first; a list equal
// to its default is no change, unless that default follows PadLayout (a face button), so the
// list still stands after the layout is switched. bindings_resolve gives the same before and
// after.
GroupChanges bindings_tidy(const GroupBindings &defaults, GroupChanges changes);
// A way of getting around the menus that has no usable control in the group: keys confirm
// (confirm or interact, a key that does not type on the title's pages), keys menu (inventory or
// back, a key: Back opens the menu in play); pad confirm (confirm or interact), back (back,
// inventory, pause_or_back) and menu (inventory or back). Escape always backs out.
enum class BindingWay : u8 { confirm, back, menu };
std::optional<BindingWay> bindings_missing_way(const GroupBindings &bindings, BindingGroup group);
// The keycode the game binds for a physical key: a letter by its US place, any other key as the
// layout makes it (layout_key: SDL_GetKeyFromScancode with no modifiers).
u32 bound_key(u32 scancode, u32 layout_key);

// binding_names.cpp: names as settings.json and the agent write them, one word each.
// The control's canonical name; empty for none (an unnamed mouse or pad control). A key whose
// name would read back as another key (Return2 is "Return" to SDL) is written "Keycode<n>".
std::string control_name(Control control);
// Any named control of the group, ignoring ASCII case and spaces; nothing unless the name, written
// again, gives this one back.
std::optional<Control> control_from_name(BindingGroup group, std::string_view name);
// Whether the player may bind it in the group: named, in the group, not fixed, not F1 or F11.
bool control_bindable(BindingGroup group, Control control);
// The names joined by commas ("Space,MouseLeft"), or "none".
std::string controls_text(std::span<const Control> controls);
// A list as controls_text writes it, any repeat counted once; nothing for an empty text, an empty
// piece, a name not bindable in the group, more than controls_per_action controls or 256 bytes.
std::optional<ActionControls> controls_from_text(BindingGroup group, std::string_view text);
// The control as the player sees it; a letter by the current keymap, so it needs SDL's keyboard.
std::string control_label(Control control);
} // namespace kf

#endif // KF_PLATFORM_BINDINGS_H
