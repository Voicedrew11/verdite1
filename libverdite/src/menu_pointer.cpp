#include <verdite/menu_pointer.h>

#include <algorithm>
#include <utility>

namespace verdite {
namespace {

// A released row waits this many shown frames for the game to draw its cursor
// there; past that the menu it was on has gone.
constexpr int confirm_wait_frames = 3;
// Wheel steps kept for later reads; a fast spin does not queue a page of them.
constexpr int most_steps = 4;

bool contains(MenuBox box, float x, float y) {
    return x >= box.x && x < box.x + box.width && y >= box.y && y < box.y + box.height;
}

MenuBox grown(MenuBox box, float by) {
    return {box.x - by, box.y - by, box.width + 2 * by, box.height + 2 * by};
}

} // namespace

void MenuPointer::begin_frame() {
    building_.clear();
    building_group_ = 0;
}

void MenuPointer::begin_widget(std::uint32_t widget, std::int32_t cursor_row) {
    building_widget_ = widget;
    building_cursor_ = cursor_row;
    ++building_group_;
}

bool MenuPointer::add_row(std::int32_t row, MenuBox box) {
    building_.push_back({building_widget_, building_group_, row, box, row == building_cursor_, false});
    return held_ && held_->widget == building_widget_ && held_->row == row && pointer_on(box);
}

void MenuPointer::frame_shown() {
    shown_ = std::move(building_);
    building_.clear();
    building_group_ = 0;
    if (shown_.empty()) {
        held_.reset();
        pressed_clear_ = false;
        confirm_.reset();
        steps_ = 0;
        return;
    }
    const int input_group = shown_.back().group;
    for (auto &row : shown_)
        row.input = row.group == input_group;
    if (confirm_ && ++confirm_frames_ > confirm_wait_frames)
        confirm_.reset();
}

void MenuPointer::moved(float x, float y) {
    x_ = x;
    y_ = y;
    present_ = true;
    row_wanted_ = true;
}

void MenuPointer::pressed(float x, float y) {
    moved(x, y);
    held_.reset();
    pressed_clear_ = false;
    if (const Row *row = input_row_at(x, y))
        held_ = Held{row->widget, row->row};
    else
        pressed_clear_ = !near_a_row(x, y);
}

void MenuPointer::released(float x, float y) {
    moved(x, y);
    if (held_) {
        const Row *row = input_row_at(x, y);
        if (row && row->widget == held_->widget && row->row == held_->row) {
            confirm_ = held_;
            confirm_frames_ = 0;
        }
    } else if (pressed_clear_ && !near_a_row(x, y)) {
        back_ = true;
    }
    held_.reset();
    pressed_clear_ = false;
}

void MenuPointer::back() { back_ = true; }

void MenuPointer::scroll(int steps) {
    if (shown_.empty())
        return;
    steps_ = std::clamp(steps_ + steps, -most_steps, most_steps);
}

void MenuPointer::gone() {
    present_ = false;
    row_wanted_ = false;
    held_.reset();
    pressed_clear_ = false;
}

std::optional<std::int32_t> MenuPointer::take_row(std::uint32_t widget) {
    if (!row_wanted_ || !present_)
        return std::nullopt;
    const auto input = std::find_if(shown_.begin(), shown_.end(), [](const Row &row) { return row.input; });
    // Another widget is on screen (a prompt this loop opened, drawn last): it
    // is left the move, for when this loop's own frame is shown.
    if (input == shown_.end() || input->widget != widget)
        return std::nullopt;
    row_wanted_ = false;
    const Row *row = input_row_at(x_, y_);
    if (!row || row->cursor)
        return std::nullopt;
    return row->row;
}

MenuPress MenuPointer::take_press() {
    if (pressed_last_read_) {
        pressed_last_read_ = false;
        return MenuPress::None;
    }
    MenuPress press = MenuPress::None;
    if (back_) {
        back_ = false;
        confirm_.reset();
        press = MenuPress::Back;
    } else if (confirm_) {
        const bool drawn = std::any_of(shown_.begin(), shown_.end(), [this](const Row &row) {
            return row.input && row.cursor && row.widget == confirm_->widget && row.row == confirm_->row;
        });
        if (drawn) {
            confirm_.reset();
            press = MenuPress::Confirm;
        }
    } else if (steps_ != 0) {
        press = steps_ > 0 ? MenuPress::Down : MenuPress::Up;
        steps_ += steps_ > 0 ? -1 : 1;
    }
    pressed_last_read_ = press != MenuPress::None;
    return press;
}

const MenuPointer::Row *MenuPointer::input_row_at(float x, float y) const {
    const auto row = std::find_if(shown_.begin(), shown_.end(),
                                  [x, y](const Row &each) { return each.input && contains(each.box, x, y); });
    return row == shown_.end() ? nullptr : &*row;
}

bool MenuPointer::near_a_row(float x, float y) const {
    return std::any_of(shown_.begin(), shown_.end(),
                       [x, y](const Row &row) { return contains(grown(row.box, margin_px), x, y); });
}

bool MenuPointer::pointer_on(MenuBox box) const { return present_ && contains(box, x_, y_); }

} // namespace verdite
