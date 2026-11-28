// The option table as a menu (verdite/option_menu.h)
#include <verdite/option_menu.h>

#include <verdite/rml_text.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <optional>
#include <utility>

namespace verdite {

namespace {

constexpr double continuous_steps = 20;   // a Number with step 0 moves a twentieth a press

std::optional<int> parse_int(std::string_view text) {
    int value = 0;
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

// "N:M" as two numbers.
std::optional<std::pair<int, int>> parse_pair(std::string_view text) {
    size_t colon = text.find(':');
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    std::optional<int> first = parse_int(text.substr(0, colon));
    std::optional<int> second = parse_int(text.substr(colon + 1));
    if (!first || !second) {
        return std::nullopt;
    }
    return std::pair(*first, *second);
}

const char *kind_class(MenuRowKind kind) {
    switch (kind) {
    case MenuRowKind::Choice: return "choice";
    case MenuRowKind::Toggle: return "toggle";
    case MenuRowKind::Number: return "number";
    case MenuRowKind::Action: return "action";
    case MenuRowKind::Heading: return "heading";
    case MenuRowKind::Status: return "status";
    case MenuRowKind::Binding: return "binding";
    }
    return "status";
}

std::string status_text(const OptionValue &value) {
    if (const std::string *text = std::get_if<std::string>(&value)) {
        return *text;
    }
    return {};
}

} // namespace

int option_choice_index(const OptionRow &row, const OptionValue &value) {
    for (size_t i = 0; i < row.choices.size(); i++) {
        if (row.choices[i].value == value) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

double option_number_share(const OptionRow &row, const OptionValue &value) {
    const double *number = std::get_if<double>(&value);
    if (number == nullptr || row.max <= row.min) {
        return 0;
    }
    return std::clamp((*number - row.min) / (row.max - row.min), 0.0, 1.0);
}

OptionMenu::OptionMenu(OptionTable &table, PlatformFeatures has, BindingSlots *bindings)
    : table_(table), has_(has), bindings_(bindings) {
    refresh();
}

void OptionMenu::refresh() {
    pages_ = table_.view(has_);
    lines_.clear();
    lines_.reserve(pages_.size());
    for (const OptionPageView &page : pages_) {
        std::vector<Line> &lines = lines_.emplace_back();
        for (const OptionSectionView &section : page.sections) {
            if (!section.title.empty()) {
                lines.push_back({std::string(section.title), nullptr});
            }
            for (const OptionRowView &row : section.rows) {
                lines.push_back({{}, &row});
            }
        }
    }
}

const OptionRowView *OptionMenu::row_at(int tab, int row) const {
    if (tab < 0 || tab >= tab_count() || row < 0 || row >= row_count(tab)) {
        return nullptr;
    }
    return lines_[static_cast<size_t>(tab)][static_cast<size_t>(row)].row;
}

int OptionMenu::tab_count() const {
    return static_cast<int>(lines_.size());
}

int OptionMenu::row_count(int tab) const {
    if (tab < 0 || tab >= tab_count()) {
        return 0;
    }
    return static_cast<int>(lines_[static_cast<size_t>(tab)].size());
}

MenuRowShape OptionMenu::shape(int tab, int row) const {
    const OptionRowView *view = row_at(tab, row);
    if (view == nullptr) {
        return {MenuRowKind::Heading, true, 0, -1};
    }
    const OptionRow &option = *view->row;
    bool enabled = view->state.enabled;
    switch (option.kind) {
    case OptionKind::Toggle: {
        const bool *on = std::get_if<bool>(&view->value);
        return {MenuRowKind::Toggle, enabled, 2, on != nullptr && *on ? 1 : 0};
    }
    case OptionKind::Choice:
        return {MenuRowKind::Choice, enabled, static_cast<int>(option.choices.size()),
                option_choice_index(option, view->value)};
    case OptionKind::Number:
        return {MenuRowKind::Number, enabled, 0, -1};
    case OptionKind::Action:
        return {MenuRowKind::Action, enabled, 0, -1};
    case OptionKind::Status: {
        int slots = bindings_ != nullptr ? bindings_->slot_count(option.id) : 0;
        if (slots > 0) {
            return {MenuRowKind::Binding, enabled, slots, -1};
        }
        return {MenuRowKind::Status, enabled, 0, -1};
    }
    }
    return {MenuRowKind::Status, enabled, 0, -1};
}

void OptionMenu::choose(int tab, int row, int choice) {
    const OptionRowView *view = row_at(tab, row);
    if (view == nullptr) {
        return;
    }
    const OptionRow &option = *view->row;
    if (option.kind == OptionKind::Toggle) {
        table_.set(option.id, OptionValue(choice != 0));
    } else if (option.kind == OptionKind::Choice && choice >= 0 &&
               choice < static_cast<int>(option.choices.size())) {
        table_.set(option.id, option.choices[static_cast<size_t>(choice)].value);
    }
    refresh();
}

void OptionMenu::step(int tab, int row, int delta) {
    const OptionRowView *view = row_at(tab, row);
    if (view == nullptr || view->row->kind != OptionKind::Number) {
        return;
    }
    const OptionRow &option = *view->row;
    const double *now = std::get_if<double>(&view->value);
    if (now == nullptr) {
        return;
    }
    double step = option.step > 0 ? option.step : (option.max - option.min) / continuous_steps;
    table_.set(option.id, OptionValue(*now + step * delta));
    refresh();
}

void OptionMenu::activate(int tab, int row) {
    if (const OptionRowView *view = row_at(tab, row)) {
        std::string id = view->row->id;   // (the views go on refresh)
        table_.run(id);
        refresh();
    }
}

void OptionMenu::listen(int tab, int row, int slot) {
    const OptionRowView *view = row_at(tab, row);
    if (view != nullptr && bindings_ != nullptr) {
        bindings_->listen(view->row->id, slot);
    }
}

void OptionMenu::clear(int tab, int row, int slot) {
    const OptionRowView *view = row_at(tab, row);
    if (view != nullptr && bindings_ != nullptr) {
        std::string id = view->row->id;
        bindings_->clear(id, slot);
        refresh();
    }
}

std::string OptionMenu::tabs_rml(const MenuModel &model) const {
    std::string out;
    for (size_t i = 0; i < pages_.size(); i++) {
        bool chosen = static_cast<int>(i) == model.tab();
        out += "<tab data-action=\"tab\" data-value=\"" + std::to_string(i) + "\"";
        out += chosen ? " class=\"chosen\">" : ">";
        out += rml_escaped(pages_[i].title) + "</tab>";
    }
    return out;
}

std::string OptionMenu::slots_rml(const MenuModel &model, int index,
                                  const OptionRowView &row) const {
    std::string out;
    int count = bindings_->slot_count(row.row->id);
    bool focused = model.row() == index;
    for (int slot = 0; slot < count; slot++) {
        std::string classes;
        if (focused && model.slot() == slot) {
            classes += "chosen";
        }
        if (bindings_->listening(row.row->id, slot)) {
            classes += classes.empty() ? "listening" : " listening";
        }
        out += "<slot data-action=\"slot\" data-value=\"" + std::to_string(index) + ":" +
               std::to_string(slot) + "\"";
        out += classes.empty() ? ">" : " class=\"" + classes + "\">";
        out += bindings_->slot_rml(row.row->id, slot) + "</slot>";
    }
    return out;
}

std::string OptionMenu::value_rml(const MenuModel &model, int index,
                                  const OptionRowView &row) const {
    const OptionRow &option = *row.row;
    std::string out;
    bool focused = model.row() == index;
    std::string here = std::to_string(index);
    if (option.kind == OptionKind::Number && focused) {
        out += "<arrow data-action=\"step\" data-value=\"" + here + ":-1\">&lt;</arrow>";
    }
    if (option.kind == OptionKind::Status) {
        out += "<value>" + rml_escaped(status_text(row.value)) + "</value>";
    } else if (option.kind != OptionKind::Action) {
        out += "<value>" + rml_escaped(option_value_label(option, row.value)) + "</value>";
    }
    if (option.kind == OptionKind::Number) {
        char width[32];
        std::snprintf(width, sizeof width, "%.1f", option_number_share(option, row.value) * 100.0);
        out += std::string("<bar><fill style=\"width: ") + width + "%\"/></bar>";
        if (focused) {
            out += "<arrow data-action=\"step\" data-value=\"" + here + ":1\">&gt;</arrow>";
        }
    }
    return out;
}

std::string OptionMenu::list_rml(const MenuModel &model, const OptionRowView &row) const {
    std::string out = "<list>";
    int current = option_choice_index(*row.row, row.value);
    for (size_t i = 0; i < row.row->choices.size(); i++) {
        int choice = static_cast<int>(i);
        std::string classes;
        if (choice == model.list_choice()) {
            classes += "highlighted";
        }
        if (choice == current) {
            classes += classes.empty() ? "chosen" : " chosen";
        }
        out += "<choice data-action=\"choice\" data-value=\"" + std::to_string(i) + "\"";
        out += classes.empty() ? ">" : " class=\"" + classes + "\">";
        out += rml_escaped(row.row->choices[i].label) + "</choice>";
    }
    return out + "</list>";
}

std::string OptionMenu::row_rml(const MenuModel &model, int index, const Line &line) const {
    if (line.row == nullptr) {
        return "<row class=\"heading\">" + rml_escaped(line.heading) + "</row>";
    }
    const OptionRowView &row = *line.row;
    MenuRowShape shape = this->shape(model.tab(), index);
    bool focused = model.row() == index;
    bool open = focused && model.list_open();
    std::string classes = kind_class(shape.kind);
    if (focused) {
        classes += " focused";
    }
    if (!row.state.enabled) {
        classes += " disabled";
    }
    if (open) {
        classes += " open";
    }
    if (shape.kind == MenuRowKind::Toggle && shape.current == 1) {
        classes += " on";
    }
    std::string here = std::to_string(index);
    std::string out = "<row id=\"row-" + here + "\" data-action=\"row\" data-hover=\"row\" "
                      "data-value=\"" + here + "\" class=\"" + classes + "\">";
    out += "<label>" + rml_escaped(row.row->label) + "</label>";
    out += shape.kind == MenuRowKind::Binding ? slots_rml(model, index, row)
                                              : value_rml(model, index, row);
    if (!row.state.note.empty()) {
        out += "<note>" + rml_escaped(row.state.note) + "</note>";
    }
    out += "</row>";
    if (open) {
        out += list_rml(model, row);
    }
    return out;
}

std::string OptionMenu::rows_rml(const MenuModel &model) const {
    int tab = model.tab();
    if (tab < 0 || tab >= tab_count()) {
        return {};
    }
    std::string out;
    const std::vector<Line> &lines = lines_[static_cast<size_t>(tab)];
    for (size_t i = 0; i < lines.size(); i++) {
        out += row_rml(model, static_cast<int>(i), lines[i]);
    }
    return out;
}

std::string OptionMenu::help_rml(const MenuModel &model) const {
    const OptionRowView *row = row_at(model.tab(), model.row());
    if (row == nullptr) {
        return {};
    }
    std::string out = "<h2>" + rml_escaped(row->row->label) + "</h2>";
    if (!row->row->help.empty()) {
        out += "<p>" + rml_escaped(row->row->help) + "</p>";
    }
    if (!row->state.note.empty()) {
        out += "<p class=\"note\">" + rml_escaped(row->state.note) + "</p>";
    }
    return out;
}

std::string OptionMenu::focused_id(const MenuModel &model) const {
    const OptionRowView *row = row_at(model.tab(), model.row());
    return row != nullptr ? row->row->id : std::string();
}

int OptionMenu::tab_of(std::string_view title) const {
    for (size_t i = 0; i < pages_.size(); i++) {
        if (pages_[i].title == title) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool OptionMenu::press(MenuModel &model, std::string_view action, std::string_view value) {
    if (action == "tab" || action == "row" || action == "choice") {
        std::optional<int> at = parse_int(value);
        if (!at) {
            return false;
        }
        if (action == "tab") {
            model.press_tab(*at);
        } else if (action == "row") {
            model.press_row(*at);
        } else {
            model.press_choice(*at);
        }
        return true;
    }
    if (action == "step" || action == "slot") {
        std::optional<std::pair<int, int>> at = parse_pair(value);
        if (!at) {
            return false;
        }
        if (action == "step") {
            model.press_step(at->first, at->second);
        } else {
            model.press_slot(at->first, at->second);
        }
        return true;
    }
    return false;
}

bool OptionMenu::hover(MenuModel &model, std::string_view action, std::string_view value) {
    if (action != "row") {
        return false;
    }
    std::optional<int> at = parse_int(value);
    if (!at) {
        return false;
    }
    model.point_at_row(*at);
    return true;
}

} // namespace verdite
