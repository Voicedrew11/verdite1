// The Controls page's binding rows: their ids, labels, help and notes, and the edits the overlay,
// the agent and Android make to settings().bindings. The player's changes are kept over the
// defaults (kf/platform/bindings.h); every edit goes through bindings_set, so they stay tidy.
#include "internal.hpp"
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_timer.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace kf::ext {
namespace {
constexpr std::string_view row_prefix = "controls.bindings.";
constexpr std::array<std::string_view, 2> group_names{"keys", "pad"};
constexpr std::uint64_t note_ms = 4000;

struct ActionText {
    const char *label;
    const char *keys_help;
    const char *pad_help;
};
constexpr const char *stick_help = "The left stick also walks and turns, unless analog movement has it.";
constexpr auto action_texts = std::to_array<ActionText>({
    {"Walk forward", "", stick_help},
    {"Walk back", "", stick_help},
    {"Strafe left", "", ""},
    {"Strafe right", "", ""},
    {"Turn left", "", stick_help},
    {"Turn right", "", stick_help},
    {"Look up", "", ""},
    {"Look down", "", ""},
    {"Attack", "", ""},
    {"Magic / item", "", ""},
    {"Examine", "Talks, opens, picks up; also confirms in menus.", "Talks, opens, picks up; also confirms in menus."},
    {"Menu", "Opens the menu; backs out inside menus.", "Opens the menu; backs out inside menus."},
    {"Pause", "", ""},
    {"Pause / back", "Escape always does this too.", ""},
    {"Confirm", "", ""},
    {"Back", "", ""},
    {"Run", "For mods that add running.", "For mods that add running."},
    {"Quick save", "", ""},
    {"Quick load", "Also on the title screen.", "Also on the title screen."},
    {"Compare language", "Held on a talk page, shows it in the other language.",
     "Held on a talk page, shows it in the other language."},
    {"Full map", "For mods that add a map.", "For mods that add a map."},
});
static_assert(action_texts.size() == action_count);

// The warning a row carries while its group has no usable control for a way around the menus.
struct WayWarning {
    BindingGroup group;
    BindingWay way;
    Action row;
    const char *text;
};
constexpr std::array way_warnings{
    WayWarning{BindingGroup::keys, BindingWay::confirm, Action::confirm,
               "Nothing on the keyboard confirms on the title's name and room-code pages: give Confirm or "
               "Examine a key that is not a letter, digit or Backspace."},
    WayWarning{BindingGroup::keys, BindingWay::menu, Action::inventory, "No key opens the menu."},
    WayWarning{BindingGroup::pad, BindingWay::confirm, Action::confirm, "No pad button confirms."},
    WayWarning{BindingGroup::pad, BindingWay::back, Action::back, "No pad button backs out."},
    WayWarning{BindingGroup::pad, BindingWay::menu, Action::inventory, "No pad button opens the menu."},
};

constexpr const char *full_note = "Four is the most: remove one first.";
constexpr const char *alt_note = " Alt+Enter also presses it.";

// The last edit's note: one row's, until until_ms.
struct RowNote {
    std::string row_id;
    std::string text;
    std::uint64_t until_ms = 0;
};
RowNote row_note;

std::size_t index_of(Action action) { return static_cast<std::size_t>(action); }

GroupBindings defaults_now(BindingGroup group) {
    return default_bindings(group, pad_layout_of(settings().pad_layout));
}

GroupChanges &changes_of(BindingGroup group) {
    BindingChanges &changes = settings().bindings;
    return group == BindingGroup::keys ? changes.keys : changes.pad;
}

void store_and_apply() {
    apply_control_settings();
    settings_mark_dirty();
}

void set_controls(BindingRow row, const ActionControls &controls) {
    GroupChanges &changes = changes_of(row.group);
    changes = bindings_set(defaults_now(row.group), changes, row.action, controls);
    store_and_apply();
}

std::optional<Action> owner_of(const GroupBindings &bindings, Control control) {
    for (std::size_t i = 0; i < bindings.size(); ++i)
        if (bindings[i].has(control))
            return static_cast<Action>(i);
    return std::nullopt;
}

// Whether the action keeps a fixed control in the group (Escape, a left stick direction), which
// its row shows, so taking its last bindable control does not leave it with none.
bool has_fixed(BindingGroup group, Action action) {
    return std::ranges::any_of(fixed_bindings(), [&](const InputBinding &binding) {
        return binding.action == action && binding_group(binding.control) == group;
    });
}

bool is_alt(Control control) {
    return control.device == InputDevice::keyboard && (control.code == SDLK_LALT || control.code == SDLK_RALT);
}
} // namespace

std::optional<BindingRow> binding_row(std::string_view id) {
    if (!id.starts_with(row_prefix))
        return std::nullopt;
    id.remove_prefix(row_prefix.size());
    const std::size_t dot = id.find('.');
    if (dot == std::string_view::npos)
        return std::nullopt;
    const auto group = std::ranges::find(group_names, id.substr(0, dot));
    if (group == group_names.end())
        return std::nullopt;
    const std::optional<Action> action = action_from_name(id.substr(dot + 1));
    if (!action)
        return std::nullopt;
    return BindingRow{static_cast<BindingGroup>(group - group_names.begin()), *action};
}

std::string binding_row_id(BindingRow row) {
    return std::string(row_prefix) + std::string(group_names[static_cast<std::size_t>(row.group)]) + "." +
           std::string(action_name(row.action));
}

const char *action_label(Action action) {
    return index_of(action) < action_texts.size() ? action_texts[index_of(action)].label : "";
}

const char *action_help(BindingRow row) {
    if (index_of(row.action) >= action_texts.size())
        return "";
    const ActionText &text = action_texts[index_of(row.action)];
    return row.group == BindingGroup::keys ? text.keys_help : text.pad_help;
}

GroupBindings bindings_in_play(BindingGroup group) {
    return bindings_resolve(defaults_now(group), changes_of(group));
}

void binding_add(BindingRow row, Control control) {
    if (!control_bindable(row.group, control))
        return;
    const GroupBindings before = bindings_in_play(row.group);
    const ActionControls &mine = before[index_of(row.action)];
    const std::string label = control_label(control);
    if (mine.has(control)) {
        binding_note(row, label + " is already here.");
        return;
    }
    if (mine.count >= controls_per_action) {
        binding_note(row, full_note);
        return;
    }
    const std::optional<Action> owner = owner_of(before, control);
    ActionControls next = mine;
    next.list[next.count++] = control;
    set_controls(row, next);
    std::string text = label + " added";
    if (owner) {
        text += std::string("; taken from ") + action_label(*owner);
        if (bindings_in_play(row.group)[index_of(*owner)].count == 0 && !has_fixed(row.group, *owner))
            text += ", which has none now";
    }
    text += ".";
    if (is_alt(control))
        text += alt_note;
    binding_note(row, std::move(text));
}

void binding_remove(BindingRow row, Control control) {
    const ActionControls mine = bindings_in_play(row.group)[index_of(row.action)];
    if (!mine.has(control))
        return;
    ActionControls next;
    for (const Control each : mine.controls())
        if (each != control)
            next.list[next.count++] = each;
    set_controls(row, next);
    binding_note(row, control_label(control) + " removed.");
}

void bindings_reset(BindingGroup group) {
    changes_of(group) = GroupChanges{};
    store_and_apply();
}

bool bindings_changed(BindingGroup group) {
    return std::ranges::any_of(changes_of(group), [](const auto &change) { return change.has_value(); });
}

void binding_note(BindingRow row, std::string text) {
    row_note = {binding_row_id(row), std::move(text), SDL_GetTicks() + note_ms};
}

std::string binding_row_note(BindingRow row) {
    if (SDL_GetTicks() < row_note.until_ms && row_note.row_id == binding_row_id(row))
        return row_note.text;
    const std::optional<BindingWay> way = bindings_missing_way(bindings_in_play(row.group), row.group);
    if (!way)
        return {};
    for (const WayWarning &warning : way_warnings)
        if (warning.group == row.group && warning.way == *way && warning.row == row.action)
            return warning.text;
    return {};
}

std::optional<std::string> binding_text_change(std::string_view id, std::string_view text) {
    const std::optional<BindingRow> row = binding_row(id);
    if (!row)
        return std::nullopt;
    const std::optional<ActionControls> controls = controls_from_text(row->group, text);
    if (!controls)
        return std::string("not a value it takes");
    set_controls(*row, *controls);
    return std::string();
}
} // namespace kf::ext
