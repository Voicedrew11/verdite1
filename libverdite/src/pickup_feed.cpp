#include <verdite/pickup_feed.h>

#include <algorithm>
#include <limits>

namespace verdite {
namespace {

bool same_kind(const PickupLine &a, const PickupLine &b) {
    return a.player == b.player && a.action == b.action && a.item == b.item;
}

// A clock that steps back counts as no time passing.
std::uint64_t since(std::uint64_t then_ms, std::uint64_t now_ms) {
    return now_ms > then_ms ? now_ms - then_ms : 0;
}

std::uint32_t add_counts(std::uint32_t a, std::uint32_t b) {
    const std::uint32_t most = std::numeric_limits<std::uint32_t>::max();
    return b > most - a ? most : a + b;
}

} // namespace

PickupFeed::PickupFeed(PickupFeedShape shape) : shape_(shape) {
    shape_.most_lines = std::max<std::uint32_t>(shape_.most_lines, 1);
}

void PickupFeed::add(const PickupLine &line, std::uint64_t now_ms) {
    const auto like = std::ranges::find_if(entries_, [&](const Entry &entry) {
        return same_kind(entry.line, line) && since(entry.added_ms, now_ms) <= shape_.merge_ms;
    });
    Entry entry{line, now_ms, now_ms};
    if (like != entries_.end()) {
        entry.line.count = add_counts(like->line.count, line.count);
        entries_.erase(like);
    }
    entries_.push_back(entry);
    if (entries_.size() > shape_.most_lines)
        entries_.erase(entries_.begin());
}

std::vector<PickupShown> PickupFeed::shown(std::uint64_t now_ms) {
    const std::uint64_t gone_ms = std::uint64_t{shape_.shown_ms} + shape_.fade_ms;
    std::erase_if(entries_, [&](const Entry &entry) { return since(entry.shown_from_ms, now_ms) >= gone_ms; });
    std::vector<PickupShown> lines;
    lines.reserve(entries_.size());
    for (const Entry &entry : entries_) {
        const std::uint64_t elapsed_ms = since(entry.shown_from_ms, now_ms);
        float opacity = 1.0f;
        if (elapsed_ms > shape_.shown_ms)
            opacity = 1.0f - static_cast<float>(elapsed_ms - shape_.shown_ms) / static_cast<float>(shape_.fade_ms);
        lines.push_back({entry.line, opacity});
    }
    return lines;
}

void PickupFeed::clear() {
    entries_.clear();
}

} // namespace verdite
