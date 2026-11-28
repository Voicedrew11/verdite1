// The first-run setup in RmlUi (setup_screen.h)
#include "setup_screen.h"

#include "game_window_art.h"
#include "menu_input.h"
#include "shell.h"

#include <verdite/menu_model.h>
#include <verdite/option_menu.h>
#include <verdite/rml_text.h>
#include <verdite/ui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace kf::ext {
namespace {
constexpr const char *document_path = "setup.rml";
constexpr const char *keep_label = "Keep as is";
constexpr const char *keep_help = "Set by hand. Choose one of these to replace it, or keep it as it is.";
constexpr const char *gpu_cost_note = "Higher GPU cost.";
constexpr const char *tier_label = "Set everything to";
constexpr double continuous_steps = 20;   // a Number with step 0 moves a twentieth a press

using verdite::MenuRowKind;
using verdite::MenuRowShape;
using verdite::SetupStep;

// What a row of the list is.
enum class LineKind : std::uint8_t {
    Heading,   // a summary section's title
    Summary,   // an answer on the summary
    Preset,    // a welcome card
    Tier,      // "Set everything to"
    Group,     // a page's group
    Option,    // a page's option
    Way,       // Back, Next, Finish, Skip
};

struct Line {
    LineKind kind;
    std::string label;
    std::string value;       // Summary: its answer; Way: its Setup::command line
    std::size_t index = 0;   // Preset: into presets; Group, Option: into items
};

verdite::ui::Document *document() {
    verdite::ui::Screen *screen = interface_screen();
    return screen ? screen->load(document_path) : nullptr;
}

std::string text_rml(const std::string &text) {
    std::string out;
    std::size_t at = 0;
    while (at <= text.size()) {   // a blank line between paragraphs
        const std::size_t end = text.find("\n\n", at);
        const std::string paragraph = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (!paragraph.empty())
            out += "<p>" + verdite::rml_escaped(paragraph) + "</p>";
        if (end == std::string::npos)
            break;
        at = end + 2;
    }
    return out;
}
} // namespace

struct SetupMenuScreen::State final : verdite::MenuRows {
    std::optional<verdite::SetupPageView> view;
    std::vector<Line> lines;
    verdite::Setup *setup = nullptr;
    verdite::MenuModel model{*this};
    MenuInputs inputs;
    bool new_page = false;   // the next frame starts at the page's first row
    int shown_row = -1;

    // ---- MenuRows: the list is one tab ----
    int tab_count() const override { return view ? 1 : 0; }
    int row_count(int) const override { return static_cast<int>(lines.size()); }
    MenuRowShape shape(int, int row) const override;
    void choose(int, int row, int choice) override;
    void step(int, int row, int delta) override;
    void activate(int, int row) override;
    void listen(int, int, int) override {}
    void clear(int, int, int) override {}

    const Line *line_at(int row) const {
        return row >= 0 && static_cast<std::size_t>(row) < lines.size() ? &lines[static_cast<std::size_t>(row)]
                                                                         : nullptr;
    }
    const verdite::SetupGroupView &group(const Line &line) const {
        return std::get<verdite::SetupGroupView>(view->items[line.index]);
    }
    const verdite::SetupOptionView &option(const Line &line) const {
        return std::get<verdite::SetupOptionView>(view->items[line.index]);
    }

    void build_lines();
    void input(verdite::MenuInput input);
    void go_on();   // Next, or Finish on the summary
    void go_back();

    std::string heading_rml() const;
    std::string steps_rml() const;
    std::string rows_rml() const;
    std::string row_rml(int index, const Line &line) const;
    std::string value_rml(int index, const Line &line, const MenuRowShape &shape) const;
    std::string list_rml(const Line &line) const;
    std::vector<std::string> answers(const Line &line) const;
    std::string help_rml() const;
    void write(verdite::ui::Document &page);
};

// ---- the rows ----

void SetupMenuScreen::State::build_lines() {
    lines.clear();
    if (!view)
        return;
    switch (view->step) {
    case SetupStep::Welcome:
        for (std::size_t i = 0; i < view->presets.size(); ++i)
            lines.push_back({LineKind::Preset, view->presets[i]->label, {}, i});
        lines.push_back({LineKind::Way, view->welcome->skip_label, "skip"});
        return;
    case SetupStep::Page:
        if (!view->tiers.empty())
            lines.push_back({LineKind::Tier, tier_label});
        for (std::size_t i = 0; i < view->items.size(); ++i) {
            if (const auto *group = std::get_if<verdite::SetupGroupView>(&view->items[i]))
                lines.push_back({LineKind::Group, group->group->label, {}, i});
            else
                lines.push_back(
                    {LineKind::Option, std::get<verdite::SetupOptionView>(view->items[i]).row.row->label, {}, i});
        }
        lines.push_back({LineKind::Way, "Next", "next"});
        lines.push_back({LineKind::Way, "Back", "back"});
        return;
    case SetupStep::Summary:
        for (const verdite::SetupSummarySection &section : view->summary) {
            lines.push_back({LineKind::Heading, std::string(section.title)});
            for (const verdite::SetupSummaryLine &each : section.lines)
                lines.push_back({LineKind::Summary, std::string(each.label), each.value});
        }
        lines.push_back({LineKind::Way, "Finish", "finish"});
        lines.push_back({LineKind::Way, "Back", "back"});
        return;
    }
}

MenuRowShape SetupMenuScreen::State::shape(int, int row) const {
    const Line *line = line_at(row);
    if (!line)
        return {};
    switch (line->kind) {
    case LineKind::Heading: return {MenuRowKind::Heading};
    case LineKind::Summary: return {MenuRowKind::Status};
    case LineKind::Preset:
    case LineKind::Way: return {MenuRowKind::Action};
    case LineKind::Tier: {
        const int current = view->page_tier ? static_cast<int>(*view->page_tier) : -1;
        return {MenuRowKind::Choice, true, static_cast<int>(view->tiers.size()), current};
    }
    case LineKind::Group: {
        const verdite::SetupGroupView &each = group(*line);
        const int offset = each.keep_offered ? 1 : 0;
        int current = -1;
        if (each.chosen)
            current = static_cast<int>(*each.chosen) + offset;
        else if (each.keep_offered)
            current = 0;
        return {MenuRowKind::Choice, true, static_cast<int>(each.group->choices.size()) + offset, current};
    }
    case LineKind::Option: {
        const verdite::OptionRowView &each = option(*line).row;
        switch (each.row->kind) {
        case verdite::OptionKind::Toggle:
            return {MenuRowKind::Toggle, true, 2, std::get<bool>(each.value) ? 1 : 0};
        case verdite::OptionKind::Choice:
            return {MenuRowKind::Choice, true, static_cast<int>(each.row->choices.size()),
                    verdite::option_choice_index(*each.row, each.value)};
        case verdite::OptionKind::Number: return {MenuRowKind::Number};
        case verdite::OptionKind::Action:
        case verdite::OptionKind::Status: return {MenuRowKind::Status};
        }
        return {MenuRowKind::Status};
    }
    }
    return {};
}

// Each control below makes Setup show its new state, which rebuilds the lines.
void SetupMenuScreen::State::choose(int, int row, int choice) {
    const Line *line = line_at(row);
    if (!line || !setup || choice < 0)
        return;
    const auto at = static_cast<std::size_t>(choice);
    if (line->kind == LineKind::Tier && at < view->tiers.size()) {
        setup->choose_page_tier(view->tiers[at]->id);
    } else if (line->kind == LineKind::Group) {
        const verdite::SetupGroupView &each = group(*line);
        const std::string id = each.group->id;
        if (each.keep_offered && at == 0)
            setup->keep(id);
        else
            setup->choose(id, at - (each.keep_offered ? 1 : 0));
    } else if (line->kind == LineKind::Option) {
        const verdite::OptionRow &each = *option(*line).row.row;
        if (each.kind == verdite::OptionKind::Toggle)
            setup->set_option(each.id, choice == 1);
        else if (each.kind == verdite::OptionKind::Choice && at < each.choices.size())
            setup->set_option(each.id, each.choices[at].value);
    }
}

void SetupMenuScreen::State::step(int, int row, int delta) {
    const Line *line = line_at(row);
    if (!line || !setup || line->kind != LineKind::Option)
        return;
    const verdite::OptionRowView &each = option(*line).row;
    const verdite::OptionRow &spec = *each.row;
    const double by = spec.step > 0 ? spec.step : (spec.max - spec.min) / continuous_steps;
    const double value = std::clamp(std::get<double>(each.value) + by * delta, spec.min, spec.max);
    setup->set_option(spec.id, value);
}

void SetupMenuScreen::State::activate(int, int row) {
    const Line *line = line_at(row);
    if (!line || !setup)
        return;
    if (line->kind == LineKind::Preset)
        setup->choose_preset(view->presets[line->index]->id);
    else if (line->kind == LineKind::Way)
        setup->command(line->value);
}

void SetupMenuScreen::State::go_on() {
    if (!setup || !view)
        return;
    if (view->step == SetupStep::Page)
        setup->next();
    else if (view->step == SetupStep::Summary)
        setup->finish();
}

// Never off the welcome, where only its Skip leaves.
void SetupMenuScreen::State::go_back() {
    if (setup && view && view->step != SetupStep::Welcome)
        setup->back();
}

void SetupMenuScreen::State::input(verdite::MenuInput input) {
    using verdite::MenuInput;
    if (input == MenuInput::NextTab) {
        go_on();
        return;
    }
    if (input == MenuInput::PreviousTab) {
        go_back();
        return;
    }
    if (model.press(input) == verdite::MenuResult::Close)
        go_back();
    static_cast<void>(model.take_cue());   // (KF1 has no menu sounds of its own to play)
}

// ---- the document ----

std::string SetupMenuScreen::State::heading_rml() const {
    switch (view->step) {
    case SetupStep::Welcome:
        return "<h1>" + verdite::rml_escaped(view->welcome->title) + "</h1>" + text_rml(view->welcome->text) +
               "<p class=\"prompt\">" + verdite::rml_escaped(view->welcome->prompt) + "</p>";
    case SetupStep::Page: {
        // A one-paragraph intro is secondary, so dim; a page explaining at length is meant to be read.
        const std::string &text = view->spec_page->text;
        const bool long_text = text.find("\n\n") != std::string::npos;
        return "<h1>" + verdite::rml_escaped(view->spec_page->title) + "</h1>" + "<div class=\"" +
               (long_text ? "read" : "dim") + "\">" + text_rml(text) + "</div>";
    }
    case SetupStep::Summary:
        return "<h1>Ready</h1><p class=\"dim\">These choices will be applied:</p>";
    }
    return {};
}

std::string SetupMenuScreen::State::steps_rml() const {
    if (view->step == SetupStep::Welcome)
        return "<p>Welcome</p>";
    return "<p>Step " + std::to_string(view->page + 1) + " of " + std::to_string(view->page_count + 1) + "</p>";
}

// A Choice-shaped line's answers, in the list's order.
std::vector<std::string> SetupMenuScreen::State::answers(const Line &line) const {
    std::vector<std::string> out;
    if (line.kind == LineKind::Tier) {
        for (const verdite::SetupTier *tier : view->tiers)
            out.push_back(tier->label);
    } else if (line.kind == LineKind::Group) {
        const verdite::SetupGroupView &each = group(line);
        if (each.keep_offered)
            out.emplace_back(keep_label);
        for (const verdite::SetupChoice &choice : each.group->choices)
            out.push_back(choice.label);
    } else if (line.kind == LineKind::Option) {
        for (const verdite::OptionChoice &choice : option(line).row.row->choices)
            out.push_back(choice.label);
    }
    return out;
}

std::string SetupMenuScreen::State::value_rml(int index, const Line &line, const MenuRowShape &shape) const {
    const std::string here = std::to_string(index);
    const bool focused = model.row() == index;
    const std::string left = "<arrow data-action=\"step\" data-value=\"" + here + ":-1\">&lt;</arrow>";
    const std::string right = "<arrow data-action=\"step\" data-value=\"" + here + ":1\">&gt;</arrow>";
    if (line.kind == LineKind::Summary)
        return "<value>" + verdite::rml_escaped(line.value) + "</value>";
    if (line.kind == LineKind::Option && shape.kind != MenuRowKind::Choice) {
        const verdite::OptionRowView &each = option(line).row;
        std::string out = focused && shape.kind == MenuRowKind::Number ? left : "";
        out += "<value>" + verdite::rml_escaped(verdite::option_value_label(*each.row, each.value)) + "</value>";
        if (shape.kind == MenuRowKind::Number) {
            char width[32];
            std::snprintf(width, sizeof width, "%.1f", verdite::option_number_share(*each.row, each.value) * 100.0);
            out += std::string("<bar><fill style=\"width: ") + width + "%\"/></bar>";
            out += focused ? right : "";
        }
        return out;
    }
    if (shape.kind != MenuRowKind::Choice)
        return {};
    const std::vector<std::string> names = answers(line);
    const std::string shown = shape.current >= 0 && static_cast<std::size_t>(shape.current) < names.size()
                                  ? names[static_cast<std::size_t>(shape.current)]
                                  : std::string("Custom");
    std::string out = focused ? left : "";
    out += "<value>" + verdite::rml_escaped(shown) + "</value>";
    const bool slider = line.kind == LineKind::Option && option(line).look == verdite::SetupOptionLook::Slider;
    if (slider && names.size() > 1 && shape.current >= 0) {
        char width[32];
        std::snprintf(width, sizeof width, "%.1f",
                      100.0 * shape.current / static_cast<double>(names.size() - 1));
        out += std::string("<bar><fill style=\"width: ") + width + "%\"/></bar>";
    }
    return out + (focused ? right : "");
}

std::string SetupMenuScreen::State::list_rml(const Line &line) const {
    const std::vector<std::string> names = answers(line);
    const int current = shape(0, model.row()).current;
    std::string out = "<list>";
    for (std::size_t i = 0; i < names.size(); ++i) {
        const int choice = static_cast<int>(i);
        std::string classes = choice == model.list_choice() ? "highlighted" : "";
        if (choice == current)
            classes += classes.empty() ? "chosen" : " chosen";
        out += "<choice data-action=\"choice\" data-value=\"" + std::to_string(i) + "\"";
        out += classes.empty() ? ">" : " class=\"" + classes + "\">";
        out += verdite::rml_escaped(names[i]) + "</choice>";
    }
    return out + "</list>";
}

std::string SetupMenuScreen::State::row_rml(int index, const Line &line) const {
    if (line.kind == LineKind::Heading)
        return "<row class=\"heading\">" + verdite::rml_escaped(line.label) + "</row>";
    const MenuRowShape row_shape = shape(0, index);
    const bool focused = model.row() == index;
    const bool open = focused && model.list_open();
    std::string classes = line.kind == LineKind::Preset ? "card"
                          : line.kind == LineKind::Way  ? "way"
                          : line.kind == LineKind::Summary ? "status"
                                                           : "choice";
    if (focused)
        classes += " focused";
    if (open)
        classes += " open";
    const std::string here = std::to_string(index);
    std::string out = "<row id=\"row-" + here + "\" data-action=\"row\" data-hover=\"row\" data-value=\"" + here +
                      "\" class=\"" + classes + "\">";
    out += "<label>" + verdite::rml_escaped(line.label) + "</label>";
    if (line.kind == LineKind::Preset) {
        const verdite::SetupPreset &preset = *view->presets[line.index];
        out += "<note>" + verdite::rml_escaped(preset.help) + "</note>";
        if (!preset.summary.empty())
            out += "<note class=\"dim\">" + verdite::rml_escaped(preset.summary) + "</note>";
    } else {
        out += value_rml(index, line, row_shape);
    }
    if (line.kind == LineKind::Group) {
        const verdite::SetupGroupView &each = group(line);
        if (each.chosen && each.group->choices[*each.chosen].higher_gpu_cost)
            out += "<note>" + std::string(gpu_cost_note) + "</note>";
    }
    out += "</row>";
    return open ? out + list_rml(line) : out;
}

std::string SetupMenuScreen::State::rows_rml() const {
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i)
        out += row_rml(static_cast<int>(i), lines[i]);
    return out;
}

std::string SetupMenuScreen::State::help_rml() const {
    const Line *line = line_at(model.row());
    if (!line)
        return {};
    const auto block = [](const std::string &title, const std::string &text) {
        std::string out = "<h2>" + verdite::rml_escaped(title) + "</h2>";
        return text.empty() ? out : out + "<p>" + verdite::rml_escaped(text) + "</p>";
    };
    switch (line->kind) {
    case LineKind::Preset: {
        const verdite::SetupPreset &preset = *view->presets[line->index];
        return block(preset.label, preset.help);
    }
    case LineKind::Tier: {
        std::string out = "<h2>" + std::string(tier_label) + "</h2>";
        for (const verdite::SetupTier *tier : view->tiers)
            out += "<p><b>" + verdite::rml_escaped(tier->label) + "</b>: " + verdite::rml_escaped(tier->help) + "</p>";
        return out;
    }
    case LineKind::Group: {
        const verdite::SetupGroupView &each = group(*line);
        std::string out = block(each.group->label, each.group->help);
        if (each.chosen) {
            const verdite::SetupChoice &choice = each.group->choices[*each.chosen];
            out += "<p class=\"note\">" + verdite::rml_escaped(choice.help) + "</p>";
            if (choice.higher_gpu_cost)
                out += "<p class=\"note\">" + std::string(gpu_cost_note) + "</p>";
        } else if (each.keep_offered) {
            out += "<p class=\"note\">" + std::string(keep_help) + "</p>";
        }
        return out;
    }
    case LineKind::Option: {
        const verdite::OptionRowView &each = option(*line).row;
        std::string out = block(each.row->label, each.row->help);
        if (!each.state.note.empty())
            out += "<p class=\"note\">" + verdite::rml_escaped(each.state.note) + "</p>";
        return out;
    }
    case LineKind::Heading:
    case LineKind::Summary:
    case LineKind::Way: return {};
    }
    return {};
}

void SetupMenuScreen::State::write(verdite::ui::Document &page) {
    page.set_body_classes(game_window_art().available() ? "setup game-art" : "setup");
    page.set_rml("steps", steps_rml());
    page.set_rml("heading", heading_rml());
    page.set_rml("rows", rows_rml());
    page.set_rml("help", help_rml());
    page.set_rml("prompts", "<prompt><key>&#x2191;&#x2193;</key> choose</prompt>"
                            "<prompt><key>&#x2190;&#x2192;</key> change</prompt>"
                            "<prompt><key>Enter</key> select</prompt>"
                            "<prompt><key>PgDn</key> next</prompt><prompt><key>Esc</key> back</prompt>");
}

// ---- SetupMenuScreen ----

SetupMenuScreen::SetupMenuScreen() : state_(std::make_unique<State>()) {}
SetupMenuScreen::~SetupMenuScreen() = default;

void SetupMenuScreen::show(const verdite::SetupPageView &view) {
    State &s = *state_;
    const bool same_page = s.view && s.view->step == view.step && s.view->page == view.page;
    s.view = view;
    s.build_lines();
    if (!same_page)
        s.new_page = true;
    if (verdite::ui::Document *page = document())
        page->show();
}

void SetupMenuScreen::close() {
    state_->view.reset();
    state_->lines.clear();
    state_->inputs.reset();
    if (verdite::ui::Document *page = document())
        page->hide();
}

void SetupMenuScreen::attach(verdite::Setup *setup) {
    state_->setup = setup;
}

bool SetupMenuScreen::event(const SDL_Event &event) {
    State &s = *state_;
    if (!s.view)
        return false;
    const bool pad = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP ||
                     event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION;
    if (!pad && event.type != SDL_EVENT_KEY_DOWN && event.type != SDL_EVENT_KEY_UP)
        return false;
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F1)
        return false;   // (the F1 menu opens over it)
    if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && event.gbutton.button == SDL_GAMEPAD_BUTTON_START) {
        s.go_on();
        return true;
    }
    if (const std::optional<verdite::MenuInput> input = s.inputs.take(event))
        s.input(*input);
    return true;
}

void SetupMenuScreen::action(const verdite::ui::Action &action) {
    State &s = *state_;
    if (!s.view || action.document != document_path)
        return;
    if (action.kind == verdite::ui::ActionKind::Hover)
        verdite::menu_hover(s.model, action.name, action.value);
    else if (action.kind == verdite::ui::ActionKind::Press)
        verdite::menu_press(s.model, action.name, action.value);
    static_cast<void>(s.model.take_cue());
}

void SetupMenuScreen::frame() {
    State &s = *state_;
    if (!s.view)
        return;
    if (const std::optional<verdite::MenuInput> input = s.inputs.repeat())
        s.input(*input);
    if (!s.view)
        return;   // (finished by that input)
    if (s.new_page) {
        s.new_page = false;
        s.model.open_at(0);
        s.shown_row = -1;
    }
    s.model.rows_changed();
    verdite::ui::Document *page = document();
    if (!page)
        return;
    s.write(*page);
    if (s.model.row() != s.shown_row) {
        s.shown_row = s.model.row();
        page->scroll_into_view("row-" + std::to_string(s.shown_row));
    }
}

} // namespace kf::ext
