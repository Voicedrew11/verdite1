#ifndef VERDITE_PICKUP_FEED_H
#define VERDITE_PICKUP_FEED_H
// The pickup feed: short lines at the side of the screen saying who in the party found, bought,
// sold or used what, each staying a few seconds and fading out. Game-agnostic: a line is the
// game's small numbers (who, what they did, which item, how many), and the game draws the lines
// in its own fonts; this keeps which lines show, merges repeats and times the fade.
#include <cstdint>
#include <vector>

namespace verdite {

struct PickupLine {
    std::uint32_t player = 0;   // the game's index of who did it
    std::uint8_t action = 0;    // the game's: found, bought, sold, used...
    std::uint16_t item = 0;     // the game's item id, or its own id for gold
    std::uint32_t count = 1;
};

struct PickupFeedShape {
    std::uint32_t most_lines = 5;   // past this, the oldest goes first
    std::uint32_t shown_ms = 4000;  // before the fade
    std::uint32_t fade_ms = 600;
    std::uint32_t merge_ms = 1500;  // the same player, action and item within this adds to the line
};

struct PickupShown {
    PickupLine line;
    float opacity = 1.0f;   // 1 shown .. 0 faded
};

class PickupFeed {
public:
    explicit PickupFeed(PickupFeedShape shape = {});

    // A line at now_ms. One like a line added within merge_ms adds its count to it, which shows
    // again from the start as the newest.
    void add(const PickupLine &line, std::uint64_t now_ms);

    // The lines to draw at now_ms, oldest first; the faded ones are forgotten.
    std::vector<PickupShown> shown(std::uint64_t now_ms);

    void clear();

private:
    struct Entry {
        PickupLine line;
        std::uint64_t added_ms = 0;
        std::uint64_t shown_from_ms = 0;
    };
    PickupFeedShape shape_;
    std::vector<Entry> entries_;
};

} // namespace verdite

#endif // VERDITE_PICKUP_FEED_H
