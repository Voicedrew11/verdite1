// Moving about a menu (verdite/menu_model.h)
#include <verdite/menu_model.h>

#include <algorithm>
#include <charconv>
#include <optional>
#include <utility>

namespace verdite {

bool MenuRows::choice_enabled(int, int, int) const {
    return true;
}

bool MenuModel::usable(int row) const {
    if (row < 0 || row >= rows_.row_count(tab_)) {
        return false;
    }
    MenuRowShape shape = rows_.shape(tab_, row);
    return shape.enabled && shape.kind != MenuRowKind::Heading && shape.kind != MenuRowKind::Status;
}

// The first usable row after (or before) row, going that way; -1 when none.
int MenuModel::usable_from(int row, int direction) const {
    for (int at = row + direction; at >= 0 && at < rows_.row_count(tab_); at += direction) {
        if (usable(at)) {
            return at;
        }
    }
    return -1;
}

void MenuModel::settle_list() {
    if (!list_open_) {
        return;
    }
    MenuRowShape shape = row_ >= 0 ? rows_.shape(tab_, row_) : MenuRowShape{};
    if (row_ < 0 || shape.kind != MenuRowKind::Choice || shape.choice_count <= 0) {
        list_open_ = false;
    } else if (list_choice_ >= shape.choice_count) {
        list_choice_ = shape.choice_count - 1;
    }
}

void MenuModel::settle() {
    if (tab_ >= rows_.tab_count()) {
        tab_ = 0;
    }
    if (!usable(row_)) {
        // the row went (hidden or disabled): the next one down, else the one before
        int count = rows_.row_count(tab_);
        int next = usable_from(row_, 1);
        row_ = next >= 0 ? next : usable_from(std::min(row_, count), -1);
    }
    if (row_ >= 0) {
        MenuRowShape shape = rows_.shape(tab_, row_);
        if (shape.kind == MenuRowKind::Binding && slot_ >= shape.choice_count) {
            slot_ = std::max(shape.choice_count - 1, 0);
        }
    }
    settle_list();
}

void MenuModel::rows_changed() {
    settle();
}

void MenuModel::open_at(int tab) {
    tab_ = tab >= 0 && tab < rows_.tab_count() ? tab : 0;
    list_open_ = false;
    row_ = usable_from(-1, 1);
}

// (the last cue made wins: Right then Clear on a slot is the clear's)
void MenuModel::cue(MenuCue cue) {
    if (cue != MenuCue::None) {
        cue_ = cue;
    }
}

MenuCue MenuModel::take_cue() {
    MenuCue taken = cue_;
    cue_ = MenuCue::None;
    return taken;
}

bool MenuModel::step_row(int delta) {
    if (row_ < 0) {
        return false;
    }
    MenuRowShape shape = rows_.shape(tab_, row_);
    switch (shape.kind) {
    case MenuRowKind::Choice: {
        int from = shape.current >= 0 ? shape.current : (delta > 0 ? -1 : shape.choice_count);
        int next = from + delta;
        while (next >= 0 && next < shape.choice_count && !rows_.choice_enabled(tab_, row_, next)) {
            next += delta;   // a dimmed choice is passed over
        }
        if (next < 0 || next >= shape.choice_count) {
            return false;
        }
        rows_.choose(tab_, row_, next);
        return true;
    }
    case MenuRowKind::Toggle: {
        int wanted = delta > 0 ? 1 : 0;
        if (shape.current == wanted) {
            return false;
        }
        rows_.choose(tab_, row_, wanted);
        return true;
    }
    case MenuRowKind::Number:
        rows_.step(tab_, row_, delta);
        return true;
    case MenuRowKind::Binding: {
        int next = std::clamp(slot_ + delta, 0, std::max(shape.choice_count - 1, 0));
        bool moved = next != slot_;
        slot_ = next;
        return moved;
    }
    case MenuRowKind::Action:
    case MenuRowKind::Heading:
    case MenuRowKind::Status:
        break;
    }
    return false;
}

bool MenuModel::confirm_row() {
    if (row_ < 0) {
        return false;
    }
    MenuRowShape shape = rows_.shape(tab_, row_);
    switch (shape.kind) {
    case MenuRowKind::Choice:
        if (shape.choice_count <= 0) {
            return false;
        }
        list_open_ = true;
        list_choice_ = shape.current >= 0 ? shape.current : 0;
        return true;
    case MenuRowKind::Toggle:
        rows_.choose(tab_, row_, shape.current != 0 ? 0 : 1);
        return true;
    case MenuRowKind::Action:
        rows_.activate(tab_, row_);
        return true;
    case MenuRowKind::Binding:
        rows_.listen(tab_, row_, slot_);
        return true;
    case MenuRowKind::Number:
    case MenuRowKind::Heading:
    case MenuRowKind::Status:
        break;
    }
    return false;
}

bool MenuModel::clear_row() {
    if (row_ < 0 || rows_.shape(tab_, row_).kind != MenuRowKind::Binding) {
        return false;
    }
    rows_.clear(tab_, row_, slot_);
    return true;
}

MenuResult MenuModel::press_in_list(MenuInput input) {
    int count = rows_.shape(tab_, row_).choice_count;
    int was = list_choice_;
    switch (input) {
    case MenuInput::Up:
        list_choice_ = std::max(list_choice_ - 1, 0);
        cue(list_choice_ != was ? MenuCue::Move : MenuCue::None);
        break;
    case MenuInput::Down:
        list_choice_ = std::min(list_choice_ + 1, count - 1);
        cue(list_choice_ != was ? MenuCue::Move : MenuCue::None);
        break;
    case MenuInput::Confirm:
        if (!rows_.choice_enabled(tab_, row_, list_choice_)) {
            cue(MenuCue::Refused);   // a dimmed choice: the list stays open
            break;
        }
        list_open_ = false;
        rows_.choose(tab_, row_, list_choice_);
        cue(MenuCue::Confirm);
        break;
    case MenuInput::Back:
        list_open_ = false;
        cue(MenuCue::Back);
        break;
    case MenuInput::NextTab:
    case MenuInput::PreviousTab:
        list_open_ = false;
        return press(input);
    case MenuInput::Left:
    case MenuInput::Right:
    case MenuInput::Clear:
        break;
    }
    settle();
    return MenuResult::Stay;
}

MenuResult MenuModel::press(MenuInput input) {
    settle();
    if (list_open_) {
        return press_in_list(input);
    }
    switch (input) {
    case MenuInput::Up:
    case MenuInput::Down: {
        int next = row_ < 0 ? usable_from(-1, 1)
                            : usable_from(row_, input == MenuInput::Up ? -1 : 1);
        cue(next >= 0 && next != row_ ? MenuCue::Move : MenuCue::None);
        row_ = next >= 0 ? next : row_;
        break;
    }
    case MenuInput::Left:
    case MenuInput::Right:
        cue(step_row(input == MenuInput::Left ? -1 : 1) ? MenuCue::Move : MenuCue::None);
        break;
    case MenuInput::Confirm:
        cue(confirm_row() ? MenuCue::Confirm : MenuCue::Refused);
        break;
    case MenuInput::Clear:
        cue(clear_row() ? MenuCue::Back : MenuCue::None);
        break;
    case MenuInput::Back:
        cue(MenuCue::Back);
        return MenuResult::Close;
    case MenuInput::NextTab:
    case MenuInput::PreviousTab: {
        int count = rows_.tab_count();
        if (count > 0) {
            open_at((tab_ + (input == MenuInput::NextTab ? 1 : count - 1)) % count);
        }
        cue(count > 1 ? MenuCue::Move : MenuCue::None);
        break;
    }
    }
    settle();
    return MenuResult::Stay;
}

void MenuModel::point_at_row(int row) {
    settle();
    if (!list_open_ && usable(row) && row != row_) {
        row_ = row;
        cue(MenuCue::Move);
    }
}

void MenuModel::press_row(int row) {
    settle();
    if (list_open_) {
        list_open_ = false;   // a press outside the list closes it
        cue(MenuCue::Back);
        return;
    }
    if (usable(row)) {
        row_ = row;
        cue(confirm_row() ? MenuCue::Confirm : MenuCue::Refused);
    } else if (row >= 0 && row < rows_.row_count(tab_) && !rows_.shape(tab_, row).enabled) {
        cue(MenuCue::Refused);
    }
    settle();
}

void MenuModel::press_step(int row, int delta) {
    settle();
    list_open_ = false;
    if (usable(row)) {
        row_ = row;
        cue(step_row(delta) ? MenuCue::Move : MenuCue::None);
    }
    settle();
}

void MenuModel::press_choice(int choice) {
    settle();
    if (list_open_ && choice >= 0 && choice < rows_.shape(tab_, row_).choice_count) {
        if (!rows_.choice_enabled(tab_, row_, choice)) {
            cue(MenuCue::Refused);
            return;
        }
        list_open_ = false;
        rows_.choose(tab_, row_, choice);
        cue(MenuCue::Confirm);
    }
    settle();
}

void MenuModel::press_slot(int row, int slot) {
    settle();
    list_open_ = false;
    if (!usable(row)) {
        return;
    }
    MenuRowShape shape = rows_.shape(tab_, row);
    if (shape.kind != MenuRowKind::Binding || slot < 0 || slot >= shape.choice_count) {
        return;
    }
    row_ = row;
    slot_ = slot;
    rows_.listen(tab_, row_, slot_);
    cue(MenuCue::Confirm);
    settle();
}

void MenuModel::press_tab(int tab) {
    if (tab >= 0 && tab < rows_.tab_count() && tab != tab_) {
        open_at(tab);
        cue(MenuCue::Move);
    }
}

namespace {

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

} // namespace

bool menu_press(MenuModel &model, std::string_view action, std::string_view value) {
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

bool menu_hover(MenuModel &model, std::string_view action, std::string_view value) {
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
