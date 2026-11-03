#ifndef VERDITE_SETUP_H
#define VERDITE_SETUP_H
// The first-run setup: a welcome with presets, a few pages of choices, and a
// summary of what will change. Game-agnostic and drawn by no one here: the
// game describes the pages (SetupSpec) over its option table, a platform
// implements SetupScreen to draw them its own way (Dear ImGui on a desktop, a
// native panel on a phone), and the platform's controls come back as calls on
// Setup.
//
//   welcome    a card per preset; choosing one fills every page from it
//   pages      in the spec's order; each holds groups (one choice per tier)
//              and plain options, both from the option table
//   summary    every answer the pages now hold, page by page
//
// Nothing is applied until Finish, which sets each staged value through the
// option table, so a preset only writes ordinary settings: every one of them
// can be changed afterwards in the settings, as if the player had set it by
// hand. Finish and Skip mark the spec's version as seen; leaving any other
// way (the window closed) marks nothing, so setup is asked again.
//
// Rows the platform lacks a feature for are left out, as in the option table's
// views, and so are the values a preset or a choice would write to them.
#include "verdite/options.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace verdite {

// One option and the value to give it.
struct SetupValue {
    std::string option;   // an option table id ("display.aspect")
    OptionValue value;
};

// A level of the game's look and feel that runs through every group: the
// game's "original", "balanced", "modern".
struct SetupTier {
    std::string id;
    std::string label;
    std::string help;
};

// One answer to a group, named by what it does ("Steady, sharp pixels"), not
// by its tier. A choice may serve several tiers, or none (offered only by hand).
struct SetupChoice {
    std::string label;
    std::string help;
    std::vector<std::string> tiers;   // tier ids that pick this choice
    std::vector<SetupValue> values;
    bool higher_gpu_cost = false;     // shown beside it
};

// Several options set together, as one row with a choice per tier.
struct SetupGroup {
    std::string id;
    std::string label;
    std::string help;
    std::vector<SetupChoice> choices;
};

// How a Choice row is drawn. A Slider's positions are the row's choices in order;
// a Number row is a slider whatever this says.
enum class SetupOptionLook : std::uint8_t { Stepper, Slider };

// One option of the table, shown as the settings show it (a display's
// resolution, the enemy turn speed, the language). The table's own state reads
// the settings as they are, not as staged, so in the setup a row is always
// enabled, and a row that depends on another choice of the setup says so here:
// shown only while that value is staged (the fixed enemy turn speed, while its
// mode is Fixed).
struct SetupOptionItem {
    std::string option;
    std::optional<SetupValue> shown_while;
    SetupOptionLook look = SetupOptionLook::Stepper;
};

// A row of a page.
using SetupItem = std::variant<SetupGroup, SetupOptionItem>;

// Whether a page offers "Set everything to" one tier above its groups.
enum class SetupPageTiers : std::uint8_t { Hidden, Offered };

// A page with nothing this platform can show is skipped, whatever is staged.
struct SetupPage {
    std::string id;
    std::string title;
    std::string text;   // a paragraph under the title; may be empty
    std::vector<SetupItem> items;
    SetupPageTiers tiers = SetupPageTiers::Hidden;
};

// A welcome card. A preset with a tier puts every group at that tier's choice;
// one without (Custom) starts every page from the settings as they are. Its
// values come after the tier's, for what no tier knows: the display suggested
// for this monitor, which the game works out when it builds the spec.
struct SetupPreset {
    std::string id;
    std::string label;
    std::string help;
    std::string tier;   // empty: Custom
    std::vector<SetupValue> values;
    std::string summary;   // the display it picks, one dim line on its card; may be empty
};

// The words on the welcome. The game picks them when it builds the spec, so a
// first run and a run from the settings can differ.
struct SetupWelcome {
    std::string title;
    std::string text;
    std::string prompt;   // over the cards
    std::string skip_label;
};

struct SetupSpec {
    int version = 1;   // raise it to ask everyone again
    SetupWelcome welcome;
    std::vector<SetupTier> tiers;
    std::vector<SetupPreset> presets;
    std::vector<SetupPage> pages;
};

// Whether the setup is to be shown, given the version the player last finished
// or skipped (0: never).
bool setup_due(const SetupSpec &spec, int seen_version);

// --- What a platform draws ---------------------------------------------------

enum class SetupStep : std::uint8_t { Welcome, Page, Summary };

struct SetupGroupView {
    const SetupGroup *group;              // borrowed from the spec
    std::optional<std::size_t> chosen;    // none: "Keep as is"
    bool keep_offered = false;            // the settings match no choice, so "Keep as is" is an answer
};

struct SetupOptionView {
    OptionRowView row;   // its staged value
    SetupOptionLook look = SetupOptionLook::Stepper;
};

using SetupItemView = std::variant<SetupGroupView, SetupOptionView>;

// One answer on the summary.
struct SetupSummaryLine {
    std::string_view label;
    std::string value;   // a choice's label, "Keep as is", or as the settings show an option
};

struct SetupSummarySection {
    std::string_view title;   // the page's
    std::vector<SetupSummaryLine> lines;
};

// Pointers and views borrow from the spec and the option table.
struct SetupPageView {
    SetupStep step = SetupStep::Welcome;
    std::size_t page = 0;         // Page: which of the shown pages; Summary: their count
    std::size_t page_count = 0;   // pages shown on this platform
    const SetupPage *spec_page = nullptr;       // Page: title, text, id
    const SetupWelcome *welcome = nullptr;      // Welcome
    std::vector<const SetupPreset *> presets;   // Welcome
    std::vector<SetupItemView> items;           // Page
    std::vector<const SetupTier *> tiers;       // Page offering "Set everything to"
    std::optional<std::size_t> page_tier;       // index into tiers: every group of the page is at it
    std::vector<SetupSummarySection> summary;   // Summary
};

// A view as JSON, for a platform drawing in another language (Android's Java)
// and the agent channel: {"step","page","pageCount","id","title","text",
// "welcome":{"title","text","prompt","skipLabel"} or null,
// "presets":[{"id","label","help","summary"}],"items":[...],"tiers":[{"id",
// "label","help"}],"pageTier","summary":[{"title","lines":[{"label","value"}]}]},
// where step is "welcome", "page" or "summary", pageTier an index or null, and
// an item is {"type":"group","id","label","help","chosen","keepOffered",
// "choices":[{"label","help","gpuCost"}]} or {"type":"option","look":"stepper"
// or "slider","row":<a row as options_json writes it>}.
std::string setup_json(const SetupPageView &view);

// A platform's setup screen. show is called with every new state, the first
// time to open it; close once the setup is over, either way. Neither may call
// back into Setup.
class SetupScreen {
public:
    virtual ~SetupScreen() = default;
    virtual void show(const SetupPageView &view) = 0;
    virtual void close() = 0;
};

// --- Running it --------------------------------------------------------------

class Setup {
public:
    // The spec and table are borrowed for the setup's life. mark_seen is called
    // with the spec's version on Finish or Skip, for the game to keep.
    Setup(const SetupSpec &spec, OptionTable &options, PlatformFeatures has,
          std::function<void(int version)> mark_seen);

    // Shows the welcome on screen, from the settings as they are. Calling it
    // again (setup run from the settings) starts over on that screen.
    void start(SetupScreen &screen);
    bool running() const { return screen_ != nullptr; }

    // The platform's controls. Each changes the state and shows it again; one
    // that does not fit the step (a preset on a page) or names nothing on it
    // does nothing and returns false.
    bool choose_preset(std::string_view preset);   // Welcome: fills the pages, goes to the first
    bool choose(std::string_view group, std::size_t choice);
    bool keep(std::string_view group);             // back to the settings' own values, when keep_offered
    bool choose_page_tier(std::string_view tier);  // every group of this page
    bool set_option(std::string_view option, const OptionValue &value);   // checked as the table checks it
    bool next();      // to the next page, or the summary
    bool back();      // the first page goes back to the welcome; the summary to the last page
    // Summary: sets every staged value, marks seen, closes. A value whose row
    // stays disabled after the others are set is left out.
    bool finish();
    bool skip();      // any step: sets nothing, marks seen, closes

    // A control as a line of text, for a platform across a queue and for the
    // agent channel: "preset <id>", "choose <group> <index>", "choose <group>
    // keep", "tier <id>",
    // "option <id> <value as option_value_from_text reads it>", "next",
    // "back", "finish", "skip". False for anything else or what does not fit.
    bool command(std::string_view line);

    // The state now, as show was last given it.
    SetupPageView view() const;

private:
    bool usable(std::string_view option) const;
    OptionValue value_now(std::string_view option) const;
    void stage(const SetupValue &value);
    void stage_choice(const SetupChoice &choice);
    bool item_available(const SetupItem &item) const;   // this platform can show it
    bool item_shown(const SetupItem &item) const;       // and shows it now
    std::vector<const SetupPage *> shown_pages() const;
    const SetupGroup *group_on_page(std::string_view group) const;
    template <typename Read>
    std::optional<std::size_t> chosen_by(const SetupGroup &group, Read read) const;
    std::optional<std::size_t> chosen(const SetupGroup &group) const;
    std::optional<std::size_t> chosen_in_settings(const SetupGroup &group) const;
    std::vector<SetupSummarySection> summary() const;
    void show() const;
    void close();

    const SetupSpec &spec_;
    OptionTable &options_;
    PlatformFeatures has_;
    std::function<void(int version)> mark_seen_;
    SetupScreen *screen_ = nullptr;   // borrowed while running
    SetupStep step_ = SetupStep::Welcome;
    std::size_t page_ = 0;            // into shown_pages()
    std::vector<SetupValue> staged_;  // in the order staged, applied in that order on Finish
};

} // namespace verdite

#endif // VERDITE_SETUP_H
