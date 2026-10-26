#include <verdite/menu_pointer.h>

#include <doctest/doctest.h>

using namespace verdite;

namespace {

constexpr std::uint32_t window = 1;
constexpr std::uint32_t prompt = 2;

// Rows 0..count-1 of a window, 100 x 20 and 24 apart from (10, 10).
void draw_window(MenuPointer &pointer, int count, int cursor) {
    pointer.begin_widget(window, cursor);
    for (int row = 0; row < count; ++row)
        pointer.add_row(row, {10, 10 + 24.0f * row, 100, 20});
}

void show_window(MenuPointer &pointer, int count, int cursor) {
    pointer.begin_frame();
    draw_window(pointer, count, cursor);
    pointer.frame_shown();
}

// The middle of window row n.
constexpr float row_y(int row) { return 20 + 24.0f * row; }

void click(MenuPointer &pointer, float x, float y) {
    pointer.pressed(x, y);
    pointer.released(x, y);
}

} // namespace

TEST_CASE("moving onto a row takes the cursor there once") {
    MenuPointer pointer;
    show_window(pointer, 4, 0);
    pointer.moved(50, row_y(2));
    CHECK(pointer.take_row(window) == 2);
    CHECK(pointer.take_row(window) == std::nullopt);
}

TEST_CASE("a resting pointer does not take the cursor back from the keyboard") {
    MenuPointer pointer;
    show_window(pointer, 4, 0);
    pointer.moved(50, row_y(2));
    CHECK(pointer.take_row(window) == 2);
    show_window(pointer, 4, 3);   // the keyboard moved the cursor on
    CHECK(pointer.take_row(window) == std::nullopt);
    pointer.moved(51, row_y(2));
    CHECK(pointer.take_row(window) == 2);
}

TEST_CASE("the row under the cursor already is not taken") {
    MenuPointer pointer;
    show_window(pointer, 4, 1);
    pointer.moved(50, row_y(1));
    CHECK(pointer.take_row(window) == std::nullopt);
}

TEST_CASE("the gutter between two rows is no row") {
    MenuPointer pointer;
    show_window(pointer, 4, 0);
    pointer.moved(50, 31);   // row 0 ends at 30, row 1 starts at 34
    CHECK(pointer.take_row(window) == std::nullopt);
}

TEST_CASE("a click confirms only once the cursor is drawn on the row") {
    MenuPointer pointer;
    show_window(pointer, 4, 0);
    click(pointer, 50, row_y(2));
    CHECK(pointer.take_press() == MenuPress::None);   // the cursor is still on row 0
    CHECK(pointer.take_row(window) == 2);
    show_window(pointer, 4, 2);
    CHECK(pointer.take_press() == MenuPress::Confirm);
    CHECK(pointer.take_press() == MenuPress::None);
    show_window(pointer, 4, 2);
    CHECK(pointer.take_press() == MenuPress::None);
}

TEST_CASE("a confirm the menu never draws is dropped") {
    MenuPointer pointer;
    show_window(pointer, 4, 0);
    click(pointer, 50, row_y(2));
    for (int frame = 0; frame < 4; ++frame)
        show_window(pointer, 4, 0);
    show_window(pointer, 4, 2);
    CHECK(pointer.take_press() == MenuPress::None);
}

TEST_CASE("releasing off the row pressed confirms nothing") {
    MenuPointer pointer;
    show_window(pointer, 4, 1);
    pointer.pressed(50, row_y(1));
    pointer.moved(50, row_y(2));
    pointer.released(50, row_y(2));
    show_window(pointer, 4, 1);
    CHECK(pointer.take_press() == MenuPress::None);
}

TEST_CASE("a held row shows held while the pointer is on it") {
    MenuPointer pointer;
    show_window(pointer, 4, 1);
    pointer.pressed(50, row_y(1));
    pointer.begin_frame();
    pointer.begin_widget(window, 1);
    CHECK_FALSE(pointer.add_row(0, {10, 10, 100, 20}));
    CHECK(pointer.add_row(1, {10, 34, 100, 20}));
    pointer.moved(200, row_y(1));
    CHECK_FALSE(pointer.add_row(1, {10, 34, 100, 20}));
}

TEST_CASE("a click clear of every row backs out, and near one does nothing") {
    MenuPointer pointer;
    show_window(pointer, 4, 0);
    click(pointer, 115, row_y(0));   // 5 right of the row
    CHECK(pointer.take_press() == MenuPress::None);
    click(pointer, 200, row_y(0));
    CHECK(pointer.take_press() == MenuPress::Back);
}

TEST_CASE("presses never arrive on two reads in a row") {
    MenuPointer pointer;
    show_window(pointer, 4, 0);
    pointer.back();
    pointer.scroll(2);
    CHECK(pointer.take_press() == MenuPress::Back);
    CHECK(pointer.take_press() == MenuPress::None);
    CHECK(pointer.take_press() == MenuPress::Down);
    CHECK(pointer.take_press() == MenuPress::None);
    CHECK(pointer.take_press() == MenuPress::Down);
    CHECK(pointer.take_press() == MenuPress::None);
    CHECK(pointer.take_press() == MenuPress::None);
    pointer.scroll(-1);
    CHECK(pointer.take_press() == MenuPress::Up);
}

TEST_CASE("only the widget drawn last takes input") {
    MenuPointer pointer;
    pointer.begin_frame();
    draw_window(pointer, 4, 1);
    pointer.begin_widget(prompt, 0);
    pointer.add_row(0, {150, 10, 50, 20});
    pointer.add_row(1, {150, 34, 50, 20});
    pointer.frame_shown();

    pointer.moved(50, row_y(2));
    CHECK(pointer.take_row(window) == std::nullopt);   // the prompt is up
    CHECK(pointer.take_row(prompt) == std::nullopt);   // not over the prompt
    click(pointer, 50, row_y(2));
    CHECK(pointer.take_press() == MenuPress::None);    // on the window: neither confirm nor back
    pointer.moved(160, row_y(1));
    CHECK(pointer.take_row(prompt) == 1);
}

TEST_CASE("a move made under a prompt waits for the loop's own frame") {
    MenuPointer pointer;
    pointer.begin_frame();
    draw_window(pointer, 4, 0);
    pointer.begin_widget(prompt, 0);
    pointer.add_row(0, {150, 10, 50, 20});
    pointer.frame_shown();
    pointer.moved(50, row_y(3));
    CHECK(pointer.take_row(window) == std::nullopt);
    show_window(pointer, 4, 0);
    CHECK(pointer.take_row(window) == 3);
}

TEST_CASE("a finger lands, slides and lifts") {
    MenuPointer pointer;
    show_window(pointer, 4, 0);
    pointer.pressed(50, row_y(1));
    CHECK(pointer.take_row(window) == 1);
    show_window(pointer, 4, 1);
    pointer.moved(50, row_y(3));
    CHECK(pointer.take_row(window) == 3);
    show_window(pointer, 4, 3);
    pointer.released(50, row_y(3));   // not the row it landed on
    show_window(pointer, 4, 3);
    CHECK(pointer.take_press() == MenuPress::None);
    pointer.gone();

    click(pointer, 50, row_y(3));
    show_window(pointer, 4, 3);
    CHECK(pointer.take_press() == MenuPress::Confirm);
}

TEST_CASE("a frame with no rows ends a press and drops the wheel") {
    MenuPointer pointer;
    show_window(pointer, 4, 0);
    pointer.pressed(50, row_y(0));
    pointer.scroll(3);
    pointer.begin_frame();
    pointer.frame_shown();
    show_window(pointer, 4, 0);
    pointer.released(50, row_y(0));
    show_window(pointer, 4, 0);
    CHECK(pointer.take_press() == MenuPress::None);
}

TEST_CASE("a click on a screen with no rows backs out") {
    MenuPointer pointer;
    pointer.begin_frame();
    pointer.frame_shown();
    click(pointer, 50, 50);
    CHECK(pointer.take_press() == MenuPress::Back);
}
