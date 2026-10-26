#include <verdite/options.h>

#include <verdite/agent.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
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

constexpr std::array<const char *, 5> kind_names = {"toggle", "choice", "number", "action", "status"};

std::string number_text(double number) {
    char text[32];
    std::snprintf(text, sizeof text, "%.10g", number);
    return text;
}

// A value as JSON, and as typed text: a string is quoted only in JSON.
std::string value_json(const OptionValue &value) {
    if (const bool *on = std::get_if<bool>(&value))
        return *on ? "true" : "false";
    if (const double *number = std::get_if<double>(&value))
        return number_text(*number);
    return json_quote(std::get<std::string>(value));
}

std::string value_text(const OptionValue &value) {
    if (const std::string *text = std::get_if<std::string>(&value))
        return *text;
    return value_json(value);
}

} // namespace

std::string option_row_json(const OptionRowView &view) {
    std::string out;
    const OptionRow &row = *view.row;
    out += "{\"id\":" + json_quote(row.id) + ",\"kind\":\"" + kind_names[static_cast<std::size_t>(row.kind)] +
           "\",\"label\":" + json_quote(row.label) + ",\"help\":" + json_quote(row.help) +
           ",\"value\":" + value_json(view.value) + ",\"enabled\":" + (view.state.enabled ? "true" : "false") +
           ",\"note\":" + json_quote(view.state.note);
    if (row.kind == OptionKind::Choice) {
        out += ",\"choices\":[";
        for (std::size_t i = 0; i < row.choices.size(); ++i)
            out += std::string(i ? "," : "") + "{\"value\":" + value_json(row.choices[i].value) +
                   ",\"label\":" + json_quote(row.choices[i].label) + "}";
        out += "]";
    }
    if (row.kind == OptionKind::Number)
        out += ",\"min\":" + number_text(row.min) + ",\"max\":" + number_text(row.max) +
               ",\"step\":" + number_text(row.step) + ",\"format\":" + json_quote(row.format) +
               ",\"settle\":" + (row.settle ? "true" : "false");
    return out + "}";
}

std::string option_value_label(const OptionRow &row, const OptionValue &value) {
    if (const bool *on = std::get_if<bool>(&value); on && row.kind == OptionKind::Toggle)
        return *on ? "On" : "Off";
    for (const OptionChoice &choice : row.choices)
        if (choice.value == value)
            return choice.label;
    if (const double *number = std::get_if<double>(&value)) {
        if (row.format.empty())
            return number_text(*number);
        char text[64];
        std::snprintf(text, sizeof text, row.format.c_str(), *number);
        return text;
    }
    return value_text(value);
}

namespace {

// strtod, as Android's libc++ has no from_chars for doubles; the C locale's '.' is the point.
std::optional<double> parse_number(std::string_view text) {
    const std::string copy(text);
    char *end = nullptr;
    const double number = std::strtod(copy.c_str(), &end);
    if (copy.empty() || end != copy.c_str() + copy.size())
        return std::nullopt;
    return number;
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

std::optional<OptionValue> OptionTable::check(std::string_view id, const OptionValue &value) const {
    const OptionRow *row = find(id);
    if (!row || !row->set)
        return std::nullopt;
    return accepted(*row, value);
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

std::string options_json(std::span<const OptionPageView> pages) {
    std::string out = "{\"pages\":[";
    for (std::size_t p = 0; p < pages.size(); ++p) {
        out += std::string(p ? "," : "") + "{\"title\":" + json_quote(pages[p].title) + ",\"sections\":[";
        for (std::size_t s = 0; s < pages[p].sections.size(); ++s) {
            const OptionSectionView &section = pages[p].sections[s];
            out += std::string(s ? "," : "") + "{\"title\":" + json_quote(section.title) + ",\"rows\":[";
            for (std::size_t r = 0; r < section.rows.size(); ++r) {
                if (r)
                    out += ",";
                out += option_row_json(section.rows[r]);
            }
            out += "]}";
        }
        out += "]}";
    }
    return out + "]}";
}

std::optional<OptionValue> option_value_from_text(const OptionRow &row, std::string_view text) {
    switch (row.kind) {
    case OptionKind::Toggle:
        if (text == "true" || text == "on")
            return true;
        if (text == "false" || text == "off")
            return false;
        return std::nullopt;
    case OptionKind::Number:
        if (const auto number = parse_number(text))
            return *number;
        return std::nullopt;
    case OptionKind::Choice:
        for (const OptionChoice &choice : row.choices)
            if (value_text(choice.value) == text)
                return choice.value;
        return std::nullopt;
    case OptionKind::Action:
    case OptionKind::Status:
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace verdite
