#include <verdite/options.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace verdite {
namespace {

OptionState state_of(const OptionRow &row) { return row.state ? row.state() : OptionState{}; }

bool platform_has(PlatformFeatures has, PlatformFeatures needs) { return (has & needs) == needs; }

OptionValue value_of(const OptionRow &row) {
    if (row.kind == OptionKind::Action || !row.get)
        return std::string();
    return row.get();
}

// The value a row would store for what a control gave, or nothing if it cannot take it.
std::optional<OptionValue> accepted(const OptionRow &row, const OptionValue &value) {
    switch (row.kind) {
    case OptionKind::Toggle:
        if (!std::holds_alternative<bool>(value))
            return std::nullopt;
        return value;
    case OptionKind::Choice: {
        const bool known = std::ranges::any_of(row.choices, [&](const OptionChoice &choice) { return choice.value == value; });
        if (!known)
            return std::nullopt;
        return value;
    }
    case OptionKind::Number: {
        const double *number = std::get_if<double>(&value);
        if (!number || !std::isfinite(*number))
            return std::nullopt;
        double snapped = row.step > 0 ? std::round(*number / row.step) * row.step : *number;
        return std::clamp(snapped, row.min, row.max);
    }
    case OptionKind::Action:
    case OptionKind::Status:
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace

OptionTable::OptionTable(std::vector<OptionPage> pages) : pages_(std::move(pages)) {}

std::vector<OptionPageView> OptionTable::view(PlatformFeatures has) const {
    std::vector<OptionPageView> pages;
    for (const OptionPage &page : pages_) {
        OptionPageView page_view{page.title, {}};
        for (const OptionSection &section : page.sections) {
            OptionSectionView section_view{section.title, {}};
            for (const OptionRow &row : section.rows) {
                if (!platform_has(has, row.needs))
                    continue;
                OptionState state = state_of(row);
                if (!state.shown)
                    continue;
                section_view.rows.push_back({&row, value_of(row), std::move(state)});
            }
            if (!section_view.rows.empty())
                page_view.sections.push_back(std::move(section_view));
        }
        if (!page_view.sections.empty())
            pages.push_back(std::move(page_view));
    }
    return pages;
}

bool OptionTable::set(std::string_view id, const OptionValue &value) {
    const OptionRow *row = find(id);
    if (!row || !row->set || !state_of(*row).enabled)
        return false;
    const auto stored = accepted(*row, value);
    if (!stored)
        return false;
    row->set(*stored);
    return true;
}

bool OptionTable::run(std::string_view id) {
    const OptionRow *row = find(id);
    if (!row || row->kind != OptionKind::Action || !row->run || !state_of(*row).enabled)
        return false;
    row->run();
    return true;
}

const OptionRow *OptionTable::find(std::string_view id) const {
    for (const OptionPage &page : pages_)
        for (const OptionSection &section : page.sections)
            for (const OptionRow &row : section.rows)
                if (row.id == id)
                    return &row;
    return nullptr;
}

} // namespace verdite
