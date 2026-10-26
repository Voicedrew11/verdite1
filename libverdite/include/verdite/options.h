#ifndef VERDITE_OPTIONS_H
#define VERDITE_OPTIONS_H
// The game's settings as data: pages of sections of options, each with its
// label, help, kind, range or choices, and how to read and change it. A
// frontend (an overlay, a native settings screen) asks for the view its
// platform can use and draws it as it likes; it never names an option. A new
// setting is a new row, and every frontend shows it.
//
// Game-agnostic: the game builds the table and binds each row to its own
// settings through get and set; set also applies the change. Rows say what a
// platform must have for them to make sense (a window to go fullscreen, a mouse
// for menus by pointer); a platform without it never sees them.
//
// An id is stable and dotted, by convention the option's settings.json path
// ("display.frame_rate"), so the agent channel and tests can name options too.
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace verdite {

using OptionValue = std::variant<bool, double, std::string>;

enum class OptionKind : std::uint8_t {
    Toggle,   // bool
    Choice,   // one of its choices' values
    Number,   // double in [min, max], snapped to step
    Action,   // a button: run, no value
    Status,   // read-only text from get (a std::string): a readout, a task's progress
};

// What a platform has. A row lists what it needs; it is left out of a view
// whose platform lacks any of them.
enum class PlatformFeature : std::uint32_t {
    Window = 1u << 0,       // resizable window: fullscreen, VSync choice
    Keyboard = 1u << 1,
    Mouse = 1u << 2,
    Gamepad = 1u << 3,      // may be plugged in: a phone with a Bluetooth pad has it
    Touch = 1u << 4,
    FileDialog = 1u << 5,   // the user can pick a file
    Discord = 1u << 6,
};
using PlatformFeatures = std::uint32_t;   // PlatformFeature bits ORed together

struct OptionChoice {
    OptionValue value;
    std::string label;
};

// Whether a row shows now and can be changed, from the game's other settings.
struct OptionState {
    bool shown = true;
    bool enabled = true;
    std::string note;   // why it is disabled, or a hint under it ("Switches when you return to the game")
};

struct OptionRow {
    std::string id;
    OptionKind kind = OptionKind::Toggle;
    std::string label;
    std::string help;                     // a sentence or two; a tooltip on desktop, a summary line on a phone
    PlatformFeatures needs = 0;
    std::vector<OptionChoice> choices;    // Choice
    double min = 0, max = 1, step = 0;    // Number; step 0 is continuous
    std::string format;                   // Number: printf for one double ("%.0f%%", "%.2fx")
    bool settle = false;                  // Number: set when the edit ends, not while dragging
    std::function<OptionValue()> get;     // every kind but Action
    std::function<void(const OptionValue &)> set;   // Toggle, Choice, Number: stores and applies
    std::function<void()> run;            // Action
    std::function<OptionState()> state;   // empty: always shown and enabled
};

struct OptionSection {
    std::string title;   // empty: no heading
    std::vector<OptionRow> rows;
};

struct OptionPage {
    std::string title;
    std::vector<OptionSection> sections;
};

// A row as a frontend draws it: its description, its value and state now.
struct OptionRowView {
    const OptionRow *row;   // borrowed from the table
    OptionValue value;      // empty string for Action
    OptionState state;
};
struct OptionSectionView {
    std::string_view title;
    std::vector<OptionRowView> rows;
};
struct OptionPageView {
    std::string_view title;
    std::vector<OptionSectionView> sections;
};

class OptionTable {
public:
    explicit OptionTable(std::vector<OptionPage> pages);

    // The pages as a platform with these features shows them, now: rows it
    // lacks a feature for and rows not shown are left out, then empty sections
    // and pages. Valid until the table is replaced.
    std::vector<OptionPageView> view(PlatformFeatures has) const;

    // Changes an option, as the frontend's control did. A Number is clamped and
    // snapped, a Choice must be one of its values. False, with nothing changed,
    // for an unknown id, a wrong kind or type, a disabled row, or an Action.
    bool set(std::string_view id, const OptionValue &value);
    // The value set would store (clamped, snapped), without storing it and
    // whether or not the row is enabled now. Nothing for an unknown id or a
    // value the row cannot take.
    std::optional<OptionValue> check(std::string_view id, const OptionValue &value) const;
    // Presses an Action. False for an unknown id, another kind or a disabled row.
    bool run(std::string_view id);

    const OptionRow *find(std::string_view id) const;

private:
    std::vector<OptionPage> pages_;
};

// A view as JSON, for a frontend in another language (Android's Java) and the
// agent channel: {"pages":[{"title","sections":[{"title","rows":[{"id","kind",
// "label","help","value","enabled","note"}]}]}]}, where kind is "toggle",
// "choice", "number", "action" or "status", a choice row adds "choices"
// ([{"value","label"}]) and a number row "min", "max", "step", "format" and "settle".
std::string options_json(std::span<const OptionPageView> pages);
// One row of it.
std::string option_row_json(const OptionRowView &row);

// A value as the settings show it: "On"/"Off", a choice's label, a number in
// the row's format.
std::string option_value_label(const OptionRow &row, const OptionValue &value);

// A row's value from typed text: "true"/"false" (or "on"/"off") for a toggle, a
// number, or a choice's value as options_json writes it ("60", "true", "16:9").
// Nothing when the text is none of these; set still checks the range.
std::optional<OptionValue> option_value_from_text(const OptionRow &row, std::string_view text);

} // namespace verdite

#endif // VERDITE_OPTIONS_H
