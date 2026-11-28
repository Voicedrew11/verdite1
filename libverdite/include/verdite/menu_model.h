#ifndef VERDITE_MENU_MODEL_H
#define VERDITE_MENU_MODEL_H
// Moving about a menu of tabs of rows by pad, keys and pointer: which tab and
// row have the focus, a row's open list of choices, and what each input does
// to them. Draws nothing and knows no setting: the rows are asked through
// MenuRows (verdite/option_menu.h over the option table; tests' stand-ins).
// From holyland's player menu (port/ui/menu_model.hpp), unchanged in kind.
//
//   Up / Down       the usable row before / after (not past the ends); in an
//                   open list, its choice
//   Left / Right    a choice: the enabled one before / after; a toggle: off /
//                   on; a number: a step; a binding: the slot before / after
//   Confirm         a choice: opens its list; a toggle: turns it over; an
//                   action: does it; in a list: chooses; a binding: listens for
//                   a new control in its chosen slot
//   Clear           a binding: empties its chosen slot
//   Back            closes the open list, else asks to close the menu
//   Next / Previous tab   the tab after / before (round), on its first usable row
// The pointer: over a row gives it the focus, a press on it is Confirm, on a
// row's arrow Left or Right, on a list's choice chooses it, on a tab shows it,
// on a binding's slot listens there.
//
// Each input leaves a cue, the sound a game's menu makes for it (take_cue):
// Move for the focus, a highlight, a tab or a value that changed; Confirm for a
// list opened or chosen from, a toggle turned, an action done, a slot listened
// for; Back for a list closed, the menu asked to close or a slot emptied;
// Refused for Confirm on a row with nothing to do or a press on a dimmed row;
// None when nothing changed (Up at the top).

#include <string_view>

namespace verdite {

enum class MenuRowKind : unsigned char {
    Choice,    // one of a list of choices
    Toggle,    // on or off
    Number,    // a value in steps
    Action,    // a button
    Heading,   // a section's title: never has the focus
    Status,    // read-only text: never has the focus
    Binding,   // a command's controls, in slots: one chosen at a time
};

// A row as the model sees it, asked again before each move (a change can show,
// hide or disable rows).
struct MenuRowShape {
    MenuRowKind kind = MenuRowKind::Heading;
    bool enabled = true;    // false: dimmed, never has the focus
    int choice_count = 0;   // Choice: how many; Binding: its slots
    int current = -1;       // Choice: the chosen one (-1: none); Toggle: 0 or 1
};

class MenuRows {
public:
    virtual ~MenuRows() = default;
    virtual int tab_count() const = 0;
    virtual int row_count(int tab) const = 0;
    virtual MenuRowShape shape(int tab, int row) const = 0;
    // A Choice's choice, or a Toggle's 0 / 1.
    virtual void choose(int tab, int row, int choice) = 0;
    // A Number: delta steps up (or down).
    virtual void step(int tab, int row, int delta) = 0;
    virtual void activate(int tab, int row) = 0;
    // A Binding: the next control pressed goes into the slot.
    virtual void listen(int tab, int row, int slot) = 0;
    virtual void clear(int tab, int row, int slot) = 0;
    // Whether a Choice's choice can be chosen (false: dimmed, skipped by Left /
    // Right, refused in its list).
    virtual bool choice_enabled(int tab, int row, int choice) const;
};

enum class MenuInput : unsigned char {
    Up, Down, Left, Right, Confirm, Back, NextTab, PreviousTab, Clear,
};

enum class MenuResult : unsigned char { Stay, Close };

enum class MenuCue : unsigned char { None, Move, Confirm, Back, Refused };

class MenuModel {
public:
    explicit MenuModel(MenuRows &rows) : rows_(rows) {}

    // Opens on that tab, on its first usable row, with no list open.
    void open_at(int tab);
    MenuResult press(MenuInput input);
    // The rows changed without an input: the focus kept to a usable row.
    void rows_changed();

    void point_at_row(int row);
    void press_row(int row);
    void press_step(int row, int delta);
    void press_choice(int choice);
    void press_tab(int tab);
    void press_slot(int row, int slot);

    [[nodiscard]] int tab() const { return tab_; }
    // The row with the focus, or -1 (the tab has no usable row).
    [[nodiscard]] int row() const { return row_; }
    [[nodiscard]] bool list_open() const { return list_open_; }
    [[nodiscard]] int list_choice() const { return list_choice_; }
    // A Binding row's chosen slot, kept from row to row as a column.
    [[nodiscard]] int slot() const { return slot_; }
    // The last cue made since the last take, then None.
    [[nodiscard]] MenuCue take_cue();

private:
    [[nodiscard]] bool usable(int row) const;
    [[nodiscard]] int usable_from(int row, int direction) const;
    void settle();
    void settle_list();
    MenuResult press_in_list(MenuInput input);
    // Each true when it changed something.
    bool step_row(int delta);
    bool confirm_row();
    bool clear_row();
    void cue(MenuCue cue);

    MenuRows &rows_;
    int tab_ = 0;
    int row_ = -1;
    bool list_open_ = false;
    int list_choice_ = 0;
    int slot_ = 0;
    MenuCue cue_ = MenuCue::None;
};

// A press or hover on a menu's RML, by its data-action and data-value
// (verdite/option_menu.h's convention: "tab" N, "row" N, "choice" N, "step"
// N:delta, "slot" N:slot); false for any other name or a value that does not
// read.
bool menu_press(MenuModel &model, std::string_view action, std::string_view value);
bool menu_hover(MenuModel &model, std::string_view action, std::string_view value);

} // namespace verdite

#endif // VERDITE_MENU_MODEL_H
