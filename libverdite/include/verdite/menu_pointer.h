#ifndef VERDITE_MENU_POINTER_H
#define VERDITE_MENU_POINTER_H
// A pointer on a game's own pad menus: the mouse, or a finger on a touch
// screen. Game-agnostic: the game hands in the rows each menu frame draws and
// the pointer's events, both in its own screen pixels, and reads back the row
// the pointer took and the presses to put into its pad read. The game's own
// cursor is the indicator; this draws nothing and knows no windowing library.
//
// A frame's rows are grouped by widget in drawing order, and the widget drawn
// last is the one taking input (a yes/no prompt over the list it asks about).
//
//   moving onto a row (mouse)        the cursor goes there
//   pressing a row                   the cursor goes there; the row shows held
//   releasing on the row pressed     Confirm, once the cursor is drawn on it
//   a click or tap clear of all rows Back (near a row, by margin_px: nothing)
//   back() (the right button)        Back
//   scroll()                         Up or Down, a step at a time
//
// A resting pointer never takes the cursor back from the keyboard or pad: only
// a move or a press does. Each press reaches the game once and never on two
// reads in a row, so a menu that detects edges sees each one.
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace verdite {

struct MenuBox {
    float x = 0, y = 0, width = 0, height = 0;   // game pixels
};

// A row of the frame on screen.
struct MenuRow {
    std::uint32_t widget;
    int group;      // which widget of the frame, in drawing order
    std::int32_t row;
    MenuBox box;
    bool cursor;    // drawn as the cursor's
    bool input;     // of the widget drawn last
};

// What the game reads as a pad press.
enum class MenuPress : std::uint8_t { None, Confirm, Back, Up, Down };

class MenuPointer {
public:
    // Drawing a frame. widget is the game's (a window, a list, a prompt), rows
    // are numbered as its cursor counts, and cursor_row is the one drawn as
    // the cursor's. Returns whether the pointer holds this row, to draw it held.
    void begin_frame();
    void begin_widget(std::uint32_t widget, std::int32_t cursor_row);
    bool add_row(std::int32_t row, MenuBox box);
    // The frame drawn since begin_frame is on screen: the pointer is tested
    // against it until the next one. A frame with no rows (play) ends any press.
    void frame_shown();

    // The pointer, in game pixels. Touch calls moved only while a finger is down.
    void moved(float x, float y);
    void pressed(float x, float y);
    void released(float x, float y);
    void back();
    void scroll(int steps);   // positive is down the list
    // No pointer any more: it left the window, the mouse was captured, focus went.
    void gone();

    // The row of the shown frame's input widget the pointer took since the last
    // call, if that widget is this one: the game moves its cursor there.
    std::optional<std::int32_t> take_row(std::uint32_t widget);
    // The press for this pad read, at most one.
    MenuPress take_press();
    // The frame on screen, for a tool that drives the menus.
    std::span<const MenuRow> shown_rows() const { return shown_; }

    static constexpr float margin_px = 8;

private:
    using Row = MenuRow;
    struct Held {
        std::uint32_t widget;
        std::int32_t row;
    };
    const Row *input_row_at(float x, float y) const;
    bool near_a_row(float x, float y) const;
    bool pointer_on(MenuBox box) const;

    std::vector<Row> building_, shown_;
    std::uint32_t building_widget_ = 0;
    std::int32_t building_cursor_ = 0;
    int building_group_ = 0;
    float x_ = 0, y_ = 0;
    bool present_ = false;          // a position is known
    bool row_wanted_ = false;       // moved or pressed since take_row
    std::optional<Held> held_;      // pressed on a row, not yet released
    bool pressed_clear_ = false;    // pressed clear of every row
    std::optional<Held> confirm_;   // released on its row, waiting for the cursor
    int confirm_frames_ = 0;        // frames shown while waiting
    bool back_ = false;
    int steps_ = 0;
    bool pressed_last_read_ = false;
};

} // namespace verdite

#endif // VERDITE_MENU_POINTER_H
