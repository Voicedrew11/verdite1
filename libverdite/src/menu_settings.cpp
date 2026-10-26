#include <verdite/menu_settings.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <ranges>
#include <utility>

namespace verdite {
namespace {

bool platform_has(PlatformFeatures has, PlatformFeatures needs) { return (has & needs) == needs; }

bool usable(const OptionRow &row) {
    if (!row.get || !row.set)
        return false;
    const OptionState state = row.state ? row.state() : OptionState{};
    return state.shown && state.enabled;
}

// Doubles that went through a format and back, or a snap, count as the same stop.
bool same_value(const OptionValue &a, const OptionValue &b) {
    if (!std::holds_alternative<double>(a) || !std::holds_alternative<double>(b))
        return a == b;
    const double x = std::get<double>(a);
    const double y = std::get<double>(b);
    return std::abs(x - y) <= 1e-9 * std::max({1.0, std::abs(x), std::abs(y)});
}

std::vector<OptionValue> stops_of(const OptionRow &row, const std::vector<OptionValue> &given) {
    if (!given.empty())
        return given;
    std::vector<OptionValue> stops;
    switch (row.kind) {
    case OptionKind::Toggle:
        return {false, true};
    case OptionKind::Choice:
        for (const OptionChoice &choice : row.choices)
            stops.push_back(choice.value);
        return stops;
    case OptionKind::Number:
        if (row.step <= 0)
            return stops;
        for (long long i = 0; row.min + static_cast<double>(i) * row.step <= row.max + row.step * 1e-6; ++i)
            stops.emplace_back(std::min(row.min + static_cast<double>(i) * row.step, row.max));
        return stops;
    case OptionKind::Action:
    case OptionKind::Status:
        return stops;
    }
    return stops;
}

// The stop one along from value, round the ends. A value between stops (a
// Number set elsewhere) goes to the nearer stop on that side.
OptionValue next_stop(const std::vector<OptionValue> &stops, const OptionValue &value, bool forward) {
    const auto at = std::ranges::find_if(stops, [&](const OptionValue &stop) { return same_value(stop, value); });
    if (at != stops.end()) {
        const auto index = static_cast<std::size_t>(at - stops.begin());
        return stops[forward ? (index + 1) % stops.size() : (index + stops.size() - 1) % stops.size()];
    }
    const double *number = std::get_if<double>(&value);
    if (!number)
        return forward ? stops.front() : stops.back();
    if (forward) {
        for (const OptionValue &stop : stops)
            if (const double *at_stop = std::get_if<double>(&stop); at_stop && *at_stop > *number)
                return stop;
        return stops.front();
    }
    for (const OptionValue &stop : std::ranges::reverse_view(stops))
        if (const double *at_stop = std::get_if<double>(&stop); at_stop && *at_stop < *number)
            return stop;
    return stops.back();
}

std::size_t wrapped(std::size_t index, int delta, std::size_t count) {
    const auto signed_count = static_cast<long long>(count);
    long long moved = (static_cast<long long>(index) + delta) % signed_count;
    if (moved < 0)
        moved += signed_count;
    return static_cast<std::size_t>(moved);
}

} // namespace

MenuSettings::MenuSettings(OptionTable &table, std::vector<MenuSettingsPage> pages, PlatformFeatures has)
    : table_(&table) {
    for (MenuSettingsPage &page : pages) {
        std::erase_if(page.rows, [&](MenuSettingsRow &row) {
            const OptionRow *option = table.find(row.option);
            if (!option || !platform_has(has, option->needs))
                return true;
            row.stops = stops_of(*option, row.stops);
            return row.stops.empty();
        });
        if (!page.rows.empty())
            pages_.push_back(std::move(page));
    }
}

void MenuSettings::open() {
    row_ = 0;
    opened_with_.clear();
}

void MenuSettings::move_row(int delta) {
    if (!pages_.empty())
        row_ = wrapped(row_, delta, pages_[page_].rows.size());
}

void MenuSettings::move_page(int delta) {
    if (pages_.empty())
        return;
    page_ = wrapped(page_, delta, pages_.size());
    row_ = 0;
}

void MenuSettings::select_row(std::size_t row) {
    if (!pages_.empty() && row < pages_[page_].rows.size())
        row_ = row;
}

bool MenuSettings::step(int delta) {
    if (pages_.empty() || delta == 0)
        return false;
    const MenuSettingsRow &row = pages_[page_].rows[row_];
    const OptionRow *option = table_->find(row.option);
    if (!option || !usable(*option) || row.stops.size() < 2)
        return false;
    const OptionValue before = option->get();
    OptionValue value = before;
    for (int i = 0; i < std::abs(delta); ++i)
        value = next_stop(row.stops, value, delta > 0);
    if (!table_->set(row.option, value))
        return false;
    const bool seen = std::ranges::any_of(opened_with_, [&](const auto &kept) { return kept.first == row.option; });
    if (!seen)
        opened_with_.emplace_back(row.option, before);
    return true;
}

bool MenuSettings::changed() const {
    return std::ranges::any_of(opened_with_, [&](const auto &kept) {
        const OptionRow *option = table_->find(kept.first);
        return option && !same_value(option->get(), kept.second);
    });
}

void MenuSettings::keep() { opened_with_.clear(); }

void MenuSettings::put_back() {
    // Newest first: an option that enabled or disabled another is restored
    // before the one it governs.
    for (const auto &[option, value] : std::ranges::reverse_view(opened_with_))
        table_->set(option, value);
    opened_with_.clear();
}

MenuSettingsView MenuSettings::view() const {
    MenuSettingsView view;
    if (pages_.empty())
        return view;
    const MenuSettingsPage &page = pages_[page_];
    view.page = page.id;
    view.page_index = page_;
    view.page_count = pages_.size();
    for (std::size_t i = 0; i < page.rows.size(); ++i) {
        const OptionRow *option = table_->find(page.rows[i].option);
        MenuSettingsRowView row;
        row.option = page.rows[i].option;
        row.value = option && option->get ? option->get() : OptionValue(std::string());
        row.enabled = option && usable(*option);
        row.selected = i == row_;
        view.rows.push_back(std::move(row));
    }
    return view;
}

} // namespace verdite
