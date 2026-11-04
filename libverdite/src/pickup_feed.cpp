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
        drop(like, now_ms);
    }
    entries_.push_back(entry);
    if (entries_.size() > shape_.most_lines)
        drop(entries_.begin(), now_ms);
}

// The next line becomes the oldest, and its time starts now.
void PickupFeed::drop(std::vector<Entry>::iterator at, std::uint64_t now_ms) {
    const bool oldest = at == entries_.begin();
    entries_.erase(at);
    if (oldest && !entries_.empty())
        entries_.front().shown_from_ms = std::max(entries_.front().shown_from_ms, now_ms);
}

std::vector<PickupShown> PickupFeed::shown(std::uint64_t now_ms) {
    const std::uint64_t gone_ms = std::uint64_t{shape_.shown_ms} + shape_.fade_ms;
    while (!entries_.empty() && since(entries_.front().shown_from_ms, now_ms) >= gone_ms) {
        const std::uint64_t left_ms = entries_.front().shown_from_ms + gone_ms;
        drop(entries_.begin(), left_ms);
    }
    std::vector<PickupShown> lines;
    lines.reserve(entries_.size());
    for (const Entry &entry : entries_)
        lines.push_back({entry.line, 1.0f});
    if (!entries_.empty()) {
        const std::uint64_t elapsed_ms = since(entries_.front().shown_from_ms, now_ms);
        if (elapsed_ms > shape_.shown_ms)
            lines.front().opacity = 1.0f - static_cast<float>(elapsed_ms - shape_.shown_ms) / static_cast<float>(shape_.fade_ms);
    }
    return lines;
}

void PickupFeed::clear() {
    entries_.clear();
}

} // namespace verdite
