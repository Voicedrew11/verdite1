#include <verdite/page_swipe.h>

#include <doctest/doctest.h>

using namespace verdite;

namespace {

PageSwipe three_pages() {
    PageSwipe swipe;
    swipe.set_page_count(3);
    return swipe;
}

// A slow drag from (x, 100) by dx over 400 ms, in 10 ms steps, then a rest and the lift.
void drag(PageSwipe &swipe, float x, float dx, std::uint64_t start_ms) {
    swipe.pressed(x, 100, start_ms);
    for (int step = 1; step <= 40; ++step)
        swipe.moved(x + dx * static_cast<float>(step) / 40, 100, start_ms + 10 * static_cast<std::uint64_t>(step));
    swipe.released(x + dx, 100, start_ms + 500);
}

} // namespace

TEST_CASE("the page follows a sideways drag") {
    PageSwipe swipe = three_pages();
    swipe.pressed(200, 100, 0);
    CHECK_FALSE(swipe.moved(195, 101, 10));
    CHECK(swipe.moved(150, 102, 20));
    CHECK(swipe.offset_px(20) == doctest::Approx(-50));
}

TEST_CASE("a drag up or down first is not a swipe") {
    PageSwipe swipe = three_pages();
    swipe.pressed(200, 100, 0);
    CHECK_FALSE(swipe.moved(202, 130, 10));
    CHECK_FALSE(swipe.moved(100, 130, 20));
    CHECK(swipe.offset_px(20) == 0);
}

TEST_CASE("a swipe left turns on once the page has slid off, then the next slides in") {
    PageSwipe swipe = three_pages();
    drag(swipe, 250, -100, 0);
    CHECK(swipe.take_turn(500) == 0);
    CHECK(swipe.offset_px(500) == doctest::Approx(-100));
    const std::uint64_t off_ms = 500 + static_cast<std::uint64_t>(PageSwipe::slide_off_ms);
    CHECK(swipe.offset_px(off_ms) == doctest::Approx(-PageSwipe::slide_px));
    CHECK(swipe.take_turn(off_ms) == 1);
    CHECK(swipe.take_turn(off_ms) == 0);
    CHECK(swipe.offset_px(off_ms) == doctest::Approx(PageSwipe::slide_px));
    const float halfway = swipe.offset_px(off_ms + 90);
    CHECK(halfway > 0);
    CHECK(halfway < PageSwipe::slide_px / 2);   // eased: fast first
    CHECK(swipe.offset_px(off_ms + 1000) == 0);
}

TEST_CASE("a swipe right turns back") {
    PageSwipe swipe = three_pages();
    drag(swipe, 50, 100, 0);
    CHECK(swipe.take_turn(2000) == -1);
}

TEST_CASE("a short slow drag slides back with no turn") {
    PageSwipe swipe = three_pages();
    drag(swipe, 200, -40, 0);
    CHECK(swipe.take_turn(2000) == 0);
    CHECK(swipe.offset_px(2000) == 0);
}

TEST_CASE("a short quick flick turns") {
    PageSwipe swipe = three_pages();
    swipe.pressed(200, 100, 0);
    swipe.moved(188, 100, 10);
    swipe.moved(170, 100, 20);
    swipe.released(160, 100, 30);
    CHECK(swipe.take_turn(1000) == 1);
}

TEST_CASE("one page takes no swipe, and losing pages ends one") {
    PageSwipe swipe;
    swipe.set_page_count(1);
    swipe.pressed(200, 100, 0);
    CHECK_FALSE(swipe.moved(100, 100, 10));
    CHECK(swipe.offset_px(10) == 0);

    PageSwipe more = three_pages();
    more.pressed(200, 100, 0);
    more.moved(100, 100, 10);
    more.set_page_count(0);
    CHECK(more.offset_px(10) == 0);
    more.released(100, 100, 20);
    CHECK(more.take_turn(1000) == 0);
}

TEST_CASE("a press on the sliding page catches it where it is") {
    PageSwipe swipe = three_pages();
    drag(swipe, 200, -40, 0);   // sliding back from -40
    const float caught = swipe.offset_px(520);
    swipe.pressed(150, 100, 520);
    CHECK(swipe.offset_px(600) == doctest::Approx(caught));
    swipe.moved(130, 100, 610);
    CHECK(swipe.offset_px(610) == doctest::Approx(caught - 20));
}
