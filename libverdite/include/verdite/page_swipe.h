#ifndef VERDITE_PAGE_SWIPE_H
#define VERDITE_PAGE_SWIPE_H
// A finger swiping a paged menu sideways, and the slide that shows it.
// Game-agnostic: the game hands in the finger in its own screen pixels and the
// time, and reads back where to draw the page and when to turn it.
//
//   a drag that goes sideways first   the page follows the finger
//   released past turn_px, or flicked the page slides off; once it is off the
//                                     turn is taken and the next page slides in
//   released short of that            the page slides back
//
// A drag that goes up or down first is not a swipe, and a menu with one page
// takes none. Turning on: a swipe to the left (the next page comes from the right).
#include <cstdint>

namespace verdite {

class PageSwipe {
public:
    // The menu on screen and its pages; fewer than two ends any swipe at once.
    void set_page_count(int pages);

    // The finger. moved returns whether the drag is a swipe, so the caller
    // stops treating it as pointing at rows or scrolling them.
    void pressed(float x, float y, std::uint64_t now_ms);
    bool moved(float x, float y, std::uint64_t now_ms);
    void released(float x, float y, std::uint64_t now_ms);

    // The turn the swipe made, once the page has slid off: 1 on, -1 back, 0 none.
    // The game turns its page then; the next page's slide in starts there.
    int take_turn(std::uint64_t now_ms);
    // Where to draw the page now: game pixels right of its place.
    float offset_px(std::uint64_t now_ms) const;

    static constexpr float claim_px = 10;      // sideways this far, before up or down, is a swipe
    static constexpr float turn_px = 60;       // released past this, the page turns
    static constexpr float flick_px_per_ms = 0.5f;   // or released moving this fast
    static constexpr float slide_px = 320;     // a page slides this far off
    static constexpr float slide_off_ms = 120, slide_in_ms = 180, slide_back_ms = 150;

private:
    enum class Phase : std::uint8_t { Idle, Undecided, Dragging, SlidingOff, SlidingIn, SlidingBack };
    struct Slide {
        float from_px = 0, to_px = 0;
        std::uint64_t start_ms = 0;
        float length_ms = 0;
    };
    float slide_at(std::uint64_t now_ms) const;
    void start_slide(Phase phase, float from_px, float to_px, float length_ms, std::uint64_t now_ms);
    void stop();

    int pages_ = 0;
    Phase phase_ = Phase::Idle;
    float down_x_ = 0, down_y_ = 0;
    float drag_px_ = 0;
    float last_x_ = 0;
    std::uint64_t last_ms_ = 0;
    float speed_px_per_ms_ = 0;   // sideways, right positive
    int turn_ = 0;                // waiting for the slide off to end
    Slide slide_;
};

} // namespace verdite

#endif // VERDITE_PAGE_SWIPE_H
