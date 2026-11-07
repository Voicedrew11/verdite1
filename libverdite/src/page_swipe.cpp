#include <verdite/page_swipe.h>

#include <algorithm>
#include <cmath>

namespace verdite {
namespace {

// A finger resting this long before it lifts has no flick left in it.
constexpr std::uint64_t flick_stale_ms = 60;

float ease_out(float t) {
    const float left = 1 - t;
    return 1 - left * left * left;
}

} // namespace

void PageSwipe::set_page_count(int pages) {
    pages_ = pages;
    if (pages_ < 2)
        stop();
}

void PageSwipe::pressed(float x, float y, std::uint64_t now_ms) {
    // The page sliding off is gone already; the turn is taken before another swipe.
    if (pages_ < 2 || phase_ == Phase::SlidingOff)
        return;
    drag_px_ = offset_px(now_ms);
    down_x_ = x;
    down_y_ = y;
    last_x_ = x;
    last_ms_ = now_ms;
    speed_px_per_ms_ = 0;
    phase_ = Phase::Undecided;
}

bool PageSwipe::moved(float x, float y, std::uint64_t now_ms) {
    if (phase_ == Phase::Undecided) {
        const float dx = x - down_x_, dy = y - down_y_;
        if (std::fabs(dx) >= claim_px && std::fabs(dx) > std::fabs(dy)) {
            phase_ = Phase::Dragging;
            down_x_ -= drag_px_;   // the page stays under the finger from where it was
        } else if (std::fabs(dy) >= claim_px) {
            start_slide(Phase::SlidingBack, drag_px_, 0, slide_back_ms, now_ms);
            return false;
        }
    }
    if (phase_ != Phase::Dragging)
        return false;
    if (now_ms > last_ms_) {
        const float speed = (x - last_x_) / static_cast<float>(now_ms - last_ms_);
        speed_px_per_ms_ = (speed_px_per_ms_ + speed) / 2;
    }
    last_x_ = x;
    last_ms_ = now_ms;
    drag_px_ = x - down_x_;
    return true;
}

void PageSwipe::released(float x, float y, std::uint64_t now_ms) {
    if (phase_ == Phase::Dragging)
        moved(x, y, now_ms);
    if (phase_ == Phase::Undecided) {
        start_slide(Phase::SlidingBack, drag_px_, 0, slide_back_ms, now_ms);
        return;
    }
    if (phase_ != Phase::Dragging)
        return;
    const float speed = now_ms - last_ms_ > flick_stale_ms ? 0 : speed_px_per_ms_;
    int turn = 0;
    if (std::fabs(speed) >= flick_px_per_ms)
        turn = speed < 0 ? 1 : -1;
    else if (std::fabs(drag_px_) >= turn_px)
        turn = drag_px_ < 0 ? 1 : -1;
    if (turn == 0) {
        start_slide(Phase::SlidingBack, drag_px_, 0, slide_back_ms, now_ms);
        return;
    }
    const float off_px = static_cast<float>(-turn) * slide_px;
    const float left = std::clamp(std::fabs(off_px - drag_px_) / slide_px, 0.25f, 1.0f);
    turn_ = turn;
    start_slide(Phase::SlidingOff, drag_px_, off_px, slide_off_ms * left, now_ms);
}

int PageSwipe::take_turn(std::uint64_t now_ms) {
    if (phase_ != Phase::SlidingOff || static_cast<float>(now_ms - slide_.start_ms) < slide_.length_ms)
        return 0;
    const int turn = turn_;
    turn_ = 0;
    start_slide(Phase::SlidingIn, static_cast<float>(turn) * slide_px, 0, slide_in_ms, now_ms);
    return turn;
}

float PageSwipe::offset_px(std::uint64_t now_ms) const {
    switch (phase_) {
    case Phase::Idle: return 0;
    case Phase::Undecided:
    case Phase::Dragging: return drag_px_;
    case Phase::SlidingOff:
    case Phase::SlidingIn:
    case Phase::SlidingBack: return slide_at(now_ms);
    }
    return 0;
}

float PageSwipe::slide_at(std::uint64_t now_ms) const {
    const float elapsed_ms = now_ms > slide_.start_ms ? static_cast<float>(now_ms - slide_.start_ms) : 0;
    const float t = slide_.length_ms > 0 ? std::min(elapsed_ms / slide_.length_ms, 1.0f) : 1;
    return slide_.from_px + (slide_.to_px - slide_.from_px) * ease_out(t);
}

void PageSwipe::start_slide(Phase phase, float from_px, float to_px, float length_ms, std::uint64_t now_ms) {
    phase_ = phase;
    slide_ = {from_px, to_px, now_ms, length_ms};
}

void PageSwipe::stop() {
    phase_ = Phase::Idle;
    drag_px_ = 0;
    turn_ = 0;
}

} // namespace verdite
