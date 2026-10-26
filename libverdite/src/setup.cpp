#include <verdite/setup.h>

#include <verdite/agent.h>

#include <algorithm>
#include <array>
#include <string>
#include <utility>

namespace verdite {
namespace {

bool platform_has(PlatformFeatures has, PlatformFeatures needs) { return (has & needs) == needs; }

bool serves(const SetupChoice &choice, std::string_view tier) {
    return std::ranges::find(choice.tiers, tier) != choice.tiers.end();
}

std::optional<std::size_t> choice_for(const SetupGroup &group, std::string_view tier) {
    for (std::size_t i = 0; i < group.choices.size(); ++i)
        if (serves(group.choices[i], tier))
            return i;
    return std::nullopt;
}

// Splits "word rest" at the first space.
std::pair<std::string_view, std::string_view> first_word(std::string_view line) {
    const std::size_t space = line.find(' ');
    if (space == std::string_view::npos)
        return {line, {}};
    return {line.substr(0, space), line.substr(space + 1)};
}

std::optional<std::size_t> parse_index(std::string_view text) {
    if (text.empty() || text.size() > 6)
        return std::nullopt;
    std::size_t index = 0;
    for (const char c : text) {
        if (c < '0' || c > '9')
            return std::nullopt;
        index = index * 10 + static_cast<std::size_t>(c - '0');
    }
    return index;
}

constexpr std::array<const char *, 3> step_names = {"welcome", "page", "summary"};

std::string bool_json(bool on) { return on ? "true" : "false"; }

std::string index_json(std::optional<std::size_t> index) { return index ? std::to_string(*index) : "null"; }

std::string group_json(const SetupGroupView &view) {
    const SetupGroup &group = *view.group;
    std::string out = "{\"type\":\"group\",\"id\":" + json_quote(group.id) + ",\"label\":" + json_quote(group.label) +
                      ",\"help\":" + json_quote(group.help) + ",\"chosen\":" + index_json(view.chosen) + ",\"choices\":[";
    for (std::size_t i = 0; i < group.choices.size(); ++i) {
        const SetupChoice &choice = group.choices[i];
        out += std::string(i ? "," : "") + "{\"label\":" + json_quote(choice.label) + ",\"help\":" +
               json_quote(choice.help) + ",\"gpuCost\":" + bool_json(choice.higher_gpu_cost) + "}";
    }
    return out + "]}";
}

std::string item_json(const SetupItemView &item) {
    if (const auto *group = std::get_if<SetupGroupView>(&item))
        return group_json(*group);
    return "{\"type\":\"option\",\"row\":" + option_row_json(std::get<OptionRowView>(item)) + "}";
}

template <typename T, typename Write>
std::string array_json(const std::vector<T> &items, Write write) {
    std::string out = "[";
    for (std::size_t i = 0; i < items.size(); ++i)
        out += std::string(i ? "," : "") + write(items[i]);
    return out + "]";
}

} // namespace

bool setup_due(const SetupSpec &spec, int seen_version) { return seen_version < spec.version; }

std::string setup_json(const SetupPageView &view) {
    const SetupPage *page = view.spec_page;
    std::string out = "{\"step\":\"" + std::string(step_names[static_cast<std::size_t>(view.step)]) +
                      "\",\"page\":" + std::to_string(view.page) + ",\"pageCount\":" + std::to_string(view.page_count) +
                      ",\"id\":" + json_quote(page ? page->id : "") + ",\"title\":" + json_quote(page ? page->title : "") +
                      ",\"text\":" + json_quote(page ? page->text : "");
    out += ",\"presets\":" + array_json(view.presets, [](const SetupPreset *preset) {
               return "{\"id\":" + json_quote(preset->id) + ",\"label\":" + json_quote(preset->label) +
                      ",\"help\":" + json_quote(preset->help) + "}";
           });
    out += ",\"items\":" + array_json(view.items, item_json);
    out += ",\"tiers\":" + array_json(view.tiers, [](const SetupTier *tier) {
               return "{\"id\":" + json_quote(tier->id) + ",\"label\":" + json_quote(tier->label) +
                      ",\"help\":" + json_quote(tier->help) + "}";
           });
    out += ",\"pageTier\":" + index_json(view.page_tier);
    out += ",\"changes\":" + array_json(view.changes, [](const SetupChange &change) {
               return "{\"label\":" + json_quote(change.option_label) + ",\"from\":" + json_quote(change.from) +
                      ",\"to\":" + json_quote(change.to) + "}";
           });
    return out + "}";
}

Setup::Setup(const SetupSpec &spec, OptionTable &options, PlatformFeatures has,
             std::function<void(int version)> mark_seen)
    : spec_(spec), options_(options), has_(has), mark_seen_(std::move(mark_seen)) {}

void Setup::start(SetupScreen &screen) {
    screen_ = &screen;
    step_ = SetupStep::Welcome;
    page_ = 0;
    staged_.clear();
    show();
}

bool Setup::choose_preset(std::string_view preset) {
    if (!running() || step_ != SetupStep::Welcome)
        return false;
    const auto found = std::ranges::find(spec_.presets, preset, &SetupPreset::id);
    if (found == spec_.presets.end())
        return false;
    staged_.clear();
    if (!found->tier.empty())
        for (const SetupPage &page : spec_.pages)
            for (const SetupItem &item : page.items)
                if (const auto *group = std::get_if<SetupGroup>(&item))
                    if (const auto choice = choice_for(*group, found->tier))
                        stage_choice(group->choices[*choice]);
    for (const SetupValue &value : found->values)
        stage(value);
    step_ = shown_pages().empty() ? SetupStep::Summary : SetupStep::Page;
    page_ = 0;
    show();
    return true;
}

bool Setup::choose(std::string_view group, std::size_t choice) {
    if (!running() || step_ != SetupStep::Page)
        return false;
    const SetupGroup *found = group_on_page(group);
    if (!found || choice >= found->choices.size())
        return false;
    stage_choice(found->choices[choice]);
    show();
    return true;
}

bool Setup::choose_page_tier(std::string_view tier) {
    if (!running() || step_ != SetupStep::Page)
        return false;
    const SetupPage &page = *shown_pages()[page_];
    if (page.tiers != SetupPageTiers::Offered || std::ranges::find(spec_.tiers, tier, &SetupTier::id) == spec_.tiers.end())
        return false;
    for (const SetupItem &item : page.items) {
        const auto *group = std::get_if<SetupGroup>(&item);
        if (!group || !item_shown(item))
            continue;
        if (const auto choice = choice_for(*group, tier))
            stage_choice(group->choices[*choice]);
    }
    show();
    return true;
}

bool Setup::set_option(std::string_view option, const OptionValue &value) {
    if (!running() || step_ != SetupStep::Page)
        return false;
    const SetupPage &page = *shown_pages()[page_];
    const bool on_page = std::ranges::any_of(page.items, [&](const SetupItem &item) {
        const auto *plain = std::get_if<SetupOptionItem>(&item);
        return plain && plain->option == option && item_shown(item);
    });
    if (!on_page)
        return false;
    const auto checked = options_.check(option, value);
    if (!checked)
        return false;
    stage({std::string(option), *checked});
    show();
    return true;
}

bool Setup::next() {
    if (!running() || step_ != SetupStep::Page)
        return false;
    if (++page_ >= shown_pages().size())
        step_ = SetupStep::Summary;
    show();
    return true;
}

bool Setup::back() {
    if (!running() || step_ == SetupStep::Welcome)
        return false;
    const std::size_t pages = shown_pages().size();
    if (step_ == SetupStep::Summary && pages > 0) {
        step_ = SetupStep::Page;
        page_ = pages - 1;
    } else if (step_ == SetupStep::Summary || page_ == 0) {
        step_ = SetupStep::Welcome;
        page_ = 0;
    } else {
        --page_;
    }
    show();
    return true;
}

bool Setup::finish() {
    if (!running() || step_ != SetupStep::Summary)
        return false;
    // A row may be enabled only by another staged value (a slider under its
    // switch), so the values left out are tried again while any pass sets one.
    std::vector<SetupValue> left = staged_;
    for (bool progress = true; progress && !left.empty();) {
        const std::size_t before = left.size();
        std::erase_if(left, [&](const SetupValue &value) { return options_.set(value.option, value.value); });
        progress = left.size() < before;
    }
    mark_seen_(spec_.version);
    close();
    return true;
}

bool Setup::skip() {
    if (!running())
        return false;
    mark_seen_(spec_.version);
    close();
    return true;
}

bool Setup::command(std::string_view line) {
    const auto [verb, rest] = first_word(line);
    if (verb == "next" && rest.empty())
        return next();
    if (verb == "back" && rest.empty())
        return back();
    if (verb == "finish" && rest.empty())
        return finish();
    if (verb == "skip" && rest.empty())
        return skip();
    if (verb == "preset")
        return choose_preset(rest);
    if (verb == "tier")
        return choose_page_tier(rest);
    if (verb == "choose") {
        const auto [group, index] = first_word(rest);
        const auto choice = parse_index(index);
        return choice && choose(group, *choice);
    }
    if (verb == "option") {
        const auto [id, text] = first_word(rest);
        const OptionRow *row = options_.find(id);
        if (!row)
            return false;
        const auto value = option_value_from_text(*row, text);
        return value && set_option(id, *value);
    }
    return false;
}

SetupPageView Setup::view() const {
    SetupPageView view;
    view.step = step_;
    const std::vector<const SetupPage *> pages = shown_pages();
    view.page_count = pages.size();
    view.page = step_ == SetupStep::Summary ? pages.size() : page_;
    if (step_ == SetupStep::Welcome) {
        for (const SetupPreset &preset : spec_.presets)
            view.presets.push_back(&preset);
        return view;
    }
    if (step_ == SetupStep::Summary) {
        view.changes = changes();
        return view;
    }
    const SetupPage &page = *pages[page_];
    view.spec_page = &page;
    bool any_group = false;
    for (const SetupItem &item : page.items) {
        if (!item_shown(item))
            continue;
        if (const auto *group = std::get_if<SetupGroup>(&item)) {
            view.items.emplace_back(SetupGroupView{group, chosen(*group)});
            any_group = true;
            continue;
        }
        const std::string &option = std::get<SetupOptionItem>(item).option;
        view.items.emplace_back(OptionRowView{options_.find(option), value_now(option), OptionState{}});
    }
    if (page.tiers != SetupPageTiers::Offered || !any_group)
        return view;
    for (const SetupTier &tier : spec_.tiers)
        view.tiers.push_back(&tier);
    for (std::size_t t = 0; t < spec_.tiers.size() && !view.page_tier; ++t) {
        const bool all_at_tier = std::ranges::all_of(view.items, [&](const SetupItemView &item) {
            const auto *group = std::get_if<SetupGroupView>(&item);
            return !group || (group->chosen && serves(group->group->choices[*group->chosen], spec_.tiers[t].id));
        });
        if (all_at_tier)
            view.page_tier = t;
    }
    return view;
}

bool Setup::usable(std::string_view option) const {
    const OptionRow *row = options_.find(option);
    return row && row->set && platform_has(has_, row->needs);
}

OptionValue Setup::value_now(std::string_view option) const {
    const auto staged = std::ranges::find(staged_, option, &SetupValue::option);
    if (staged != staged_.end())
        return staged->value;
    const OptionRow *row = options_.find(option);
    return row && row->get ? row->get() : OptionValue(std::string());
}

void Setup::stage(const SetupValue &value) {
    if (!usable(value.option))
        return;
    const auto checked = options_.check(value.option, value.value);
    if (!checked)
        return;
    const auto staged = std::ranges::find(staged_, value.option, &SetupValue::option);
    if (staged != staged_.end())
        staged->value = *checked;
    else
        staged_.push_back({value.option, *checked});
}

void Setup::stage_choice(const SetupChoice &choice) {
    for (const SetupValue &value : choice.values)
        stage(value);
}

bool Setup::item_available(const SetupItem &item) const {
    if (const auto *group = std::get_if<SetupGroup>(&item))
        return std::ranges::any_of(group->choices, [&](const SetupChoice &choice) {
            return std::ranges::any_of(choice.values, [&](const SetupValue &value) { return usable(value.option); });
        });
    return usable(std::get<SetupOptionItem>(item).option);
}

bool Setup::item_shown(const SetupItem &item) const {
    if (!item_available(item))
        return false;
    const auto *plain = std::get_if<SetupOptionItem>(&item);
    if (!plain || !plain->shown_while)
        return true;
    const auto wanted = options_.check(plain->shown_while->option, plain->shown_while->value);
    return wanted && value_now(plain->shown_while->option) == *wanted;
}

std::vector<const SetupPage *> Setup::shown_pages() const {
    std::vector<const SetupPage *> pages;
    for (const SetupPage &page : spec_.pages)
        if (std::ranges::any_of(page.items, [&](const SetupItem &item) { return item_available(item); }))
            pages.push_back(&page);
    return pages;
}

const SetupGroup *Setup::group_on_page(std::string_view group) const {
    for (const SetupItem &item : shown_pages()[page_]->items) {
        const auto *found = std::get_if<SetupGroup>(&item);
        if (found && found->id == group && item_shown(item))
            return found;
    }
    return nullptr;
}

// The first choice whose every value this platform can set is the value now.
std::optional<std::size_t> Setup::chosen(const SetupGroup &group) const {
    for (std::size_t i = 0; i < group.choices.size(); ++i) {
        bool any = false, all = true;
        for (const SetupValue &value : group.choices[i].values) {
            if (!usable(value.option))
                continue;
            any = true;
            const auto checked = options_.check(value.option, value.value);
            all = all && checked && value_now(value.option) == *checked;
        }
        if (any && all)
            return i;
    }
    return std::nullopt;
}

std::vector<SetupChange> Setup::changes() const {
    std::vector<SetupChange> out;
    for (const SetupValue &value : staged_) {
        const OptionRow *row = options_.find(value.option);
        if (!row || !row->get)
            continue;
        const OptionValue now = row->get();
        if (now == value.value)
            continue;
        out.push_back({row->label, option_value_label(*row, now), option_value_label(*row, value.value)});
    }
    return out;
}

void Setup::show() const {
    if (screen_)
        screen_->show(view());
}

void Setup::close() {
    SetupScreen *screen = std::exchange(screen_, nullptr);
    screen->close();
}

} // namespace verdite
