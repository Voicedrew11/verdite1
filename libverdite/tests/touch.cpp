#include <verdite/touch.h>

#include <doctest/doctest.h>

#include <array>

using namespace verdite;

namespace {

enum : std::uint32_t { move_stick, attack, look, pad, tap };

// A 1000 x 500 safe area: the stick on the left half, Attack at the right
// over the look area, which takes the rest of the right half.
constexpr std::array<TouchControl, 3> walking = {
    TouchControl{move_stick, TouchKind::Stick, {0, 0, 0.5f, 1}, 0.2f},
    TouchControl{attack, TouchKind::Button, {0.8f, 0.6f, 0.2f, 0.4f}, 0.1f},
    TouchControl{look, TouchKind::Drag, {0.5f, 0, 0.5f, 1}, 0},
};

TouchControls laid_out(std::span<const TouchControl> layout) {
    TouchControls touch;
    touch.set_safe_area(0, 0, 1000, 500);
    touch.set_layout(layout);
    return touch;
}

} // namespace

TEST_CASE("a stick centres where the finger lands and reads its throw") {
    auto touch = laid_out(walking);
    touch.finger_down(1, 200, 250, 0);
    auto frame = touch.take_frame();
    CHECK(frame[0].held);
    CHECK(frame[0].centre_x == doctest::Approx(0.2f));
    CHECK(frame[0].stick.x == 0);
    touch.finger_moved(1, 250, 250);   // half the 100 px throw
    frame = touch.take_frame();
    CHECK(frame[0].stick.x == doctest::Approx(0.5f));
    touch.finger_moved(1, 200, 0);     // far up: clamped to full
    frame = touch.take_frame();
    CHECK(frame[0].stick.y == doctest::Approx(-1.0f));
    touch.finger_up(1, 100);
    frame = touch.take_frame();
    CHECK_FALSE(frame[0].held);
    CHECK(frame[0].stick.y == 0);
}

TEST_CASE("a finger keeps the control it landed on") {
    auto touch = laid_out(walking);
    touch.finger_down(1, 900, 400, 0);   // Attack
    touch.finger_moved(1, 100, 100);     // slides onto the stick's half
    auto frame = touch.take_frame();
    CHECK(frame[1].held);
    CHECK_FALSE(frame[0].held);
    touch.finger_up(1, 50);
    CHECK_FALSE(touch.take_frame()[1].held);
}

TEST_CASE("the first row holding a point wins") {
    auto touch = laid_out(walking);
    touch.finger_down(1, 900, 400, 0);   // in both Attack and the look area
    const auto frame = touch.take_frame();
    CHECK(frame[1].held);
    CHECK_FALSE(frame[2].held);
}

TEST_CASE("two fingers on one button hold it until both lift, in either order") {
    auto touch = laid_out(walking);
    touch.finger_down(1, 900, 400, 0);
    touch.finger_down(2, 950, 450, 0);
    touch.finger_up(1, 10);
    CHECK(touch.take_frame()[1].held);
    touch.finger_up(2, 20);
    CHECK_FALSE(touch.take_frame()[1].held);
}

TEST_CASE("a second finger on a taken stick falls through to the next row") {
    constexpr std::array<TouchControl, 2> layout = {
        TouchControl{move_stick, TouchKind::Stick, {0, 0, 1, 1}, 0.2f},
        TouchControl{look, TouchKind::Drag, {0, 0, 1, 1}, 0},
    };
    auto touch = laid_out(layout);
    touch.finger_down(1, 100, 100, 0);
    touch.finger_down(2, 600, 100, 0);
    touch.finger_moved(2, 650, 100);
    const auto frame = touch.take_frame();
    CHECK(frame[0].centre_x == doctest::Approx(0.1f));
    CHECK(frame[1].drag_x_px == doctest::Approx(50.0f));
}

TEST_CASE("a drag counts its movement once") {
    auto touch = laid_out(walking);
    touch.finger_down(1, 600, 100, 0);
    touch.finger_moved(1, 630, 90);
    touch.finger_moved(1, 640, 80);
    auto frame = touch.take_frame();
    CHECK(frame[2].drag_x_px == doctest::Approx(40.0f));
    CHECK(frame[2].drag_y_px == doctest::Approx(-20.0f));
    frame = touch.take_frame();
    CHECK(frame[2].drag_x_px == 0);
    CHECK(frame[2].held);
}

TEST_CASE("a tap is a short touch that stays put") {
    constexpr std::array<TouchControl, 1> layout = {TouchControl{tap, TouchKind::Tap, {0, 0, 1, 1}, 0}};
    auto touch = laid_out(layout);
    touch.finger_down(1, 500, 250, 1000);
    touch.finger_up(1, 1100);
    CHECK(touch.take_frame()[0].tapped);
    CHECK_FALSE(touch.take_frame()[0].tapped);

    touch.finger_down(2, 500, 250, 2000);
    touch.finger_up(2, 2600);   // too long
    CHECK_FALSE(touch.take_frame()[0].tapped);

    touch.finger_down(3, 500, 250, 3000);
    touch.finger_moved(3, 700, 250);   // too far, even back again
    touch.finger_moved(3, 500, 250);
    touch.finger_up(3, 3100);
    CHECK_FALSE(touch.take_frame()[0].tapped);
}

TEST_CASE("a D-pad points eight ways from its middle") {
    constexpr std::array<TouchControl, 1> layout = {TouchControl{pad, TouchKind::Dpad, {0, 0, 0.4f, 0.8f}, 0.4f}};
    auto touch = laid_out(layout);   // middle at (200, 200), radius 200 px
    touch.finger_down(1, 205, 200, 0);
    CHECK(touch.take_frame()[0].stick.x == 0);   // inside the dead middle
    touch.finger_moved(1, 200, 50);
    auto frame = touch.take_frame();
    CHECK(frame[0].stick.x == 0);
    CHECK(frame[0].stick.y == -1);
    touch.finger_moved(1, 350, 350);
    frame = touch.take_frame();
    CHECK(frame[0].stick.x == 1);
    CHECK(frame[0].stick.y == 1);
}

TEST_CASE("a finger on no control, a new layout and release_all") {
    constexpr std::array<TouchControl, 1> small = {TouchControl{attack, TouchKind::Button, {0, 0, 0.1f, 0.1f}, 0.05f}};
    auto touch = laid_out(small);
    touch.finger_down(1, 500, 250, 0);
    CHECK_FALSE(touch.any_held());
    touch.finger_moved(1, 10, 10);   // onto the button: still nothing
    CHECK_FALSE(touch.take_frame()[0].held);

    touch.finger_down(2, 10, 10, 0);
    CHECK(touch.any_held());
    touch.release_all();
    CHECK_FALSE(touch.any_held());
    CHECK_FALSE(touch.take_frame()[0].held);

    touch.finger_down(3, 10, 10, 0);
    touch.set_layout(walking);
    CHECK_FALSE(touch.any_held());
    CHECK(touch.take_frame().size() == walking.size());
}

TEST_CASE("places are measured in the safe area") {
    TouchControls touch;
    touch.set_safe_area(100, 0, 1000, 500);   // a notch on the left
    touch.set_layout(walking);
    touch.finger_down(1, 50, 250, 0);   // left of the safe area
    CHECK_FALSE(touch.any_held());
    touch.finger_down(2, 150, 250, 0);
    CHECK(touch.take_frame()[0].centre_x == doctest::Approx(0.05f));
}
