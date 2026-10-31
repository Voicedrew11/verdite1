#include <verdite/message_sample.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace verdite {
namespace {

std::uint64_t splitmix64(std::uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

// The top 53 bits plus one, over 2^53: a value in (0, 1].
double unit_interval(std::uint64_t bits) {
    constexpr double two_to_the_53 = 9007199254740992.0;
    return static_cast<double>((bits >> 11) + 1) / two_to_the_53;
}

// Efraimidis-Spirakis: the larger key is drawn first, and weight 1 + good makes good ones likelier.
double draw_key(std::uint64_t seed, const OfferedMessage &message) {
    const double u = unit_interval(splitmix64(seed ^ message.id));
    return std::log(u) / (1.0 + static_cast<double>(message.good));
}

// Rounds down for a positive divisor, so cell -1 is block -1 and not block 0.
int floor_div(int cell, int cells) {
    return cell / cells - (cell % cells < 0 ? 1 : 0);
}

using Block = std::pair<int, int>;

Block block_of(const OfferedMessage &message, int block_cells) {
    return {floor_div(message.cell_x, block_cells), floor_div(message.cell_z, block_cells)};
}

// The counts the caps are checked against: the non-mine shown so far.
class Tally {
public:
    explicit Tally(MessageSampleShape shape) : shape_(shape) {}

    bool has_room(const OfferedMessage &message) const {
        return total_ < shape_.per_floor && count_in(block_of(message, shape_.block_cells)) < shape_.per_block;
    }

    void count(const OfferedMessage &message) {
        ++total_;
        ++per_block_[block_of(message, shape_.block_cells)];
    }

private:
    std::uint32_t count_in(const Block &block) const {
        const auto found = per_block_.find(block);
        return found == per_block_.end() ? 0 : found->second;
    }

    MessageSampleShape shape_;
    std::uint32_t total_ = 0;
    std::map<Block, std::uint32_t> per_block_;
};

// Each id once, the first offered kept, in offered order.
std::vector<const OfferedMessage *> first_of_each(std::span<const OfferedMessage> offered) {
    std::unordered_set<std::uint64_t> seen;
    std::vector<const OfferedMessage *> unique;
    unique.reserve(offered.size());
    for (const OfferedMessage &message : offered) {
        if (seen.insert(message.id).second)
            unique.push_back(&message);
    }
    return unique;
}

struct Candidate {
    double key = 0.0;
    const OfferedMessage *message = nullptr;
};

} // namespace

MessageSample::MessageSample(std::uint64_t seed, MessageSampleShape shape) : shape_(shape), seed_(seed) {
    shape_.block_cells = std::max(shape_.block_cells, 1);
}

std::vector<std::uint64_t> MessageSample::shown(std::span<const OfferedMessage> offered) {
    const std::vector<const OfferedMessage *> unique = first_of_each(offered);
    std::unordered_map<std::uint64_t, const OfferedMessage *> by_id;
    for (const OfferedMessage *message : unique)
        by_id.emplace(message->id, message);

    Tally tally(shape_);
    std::vector<std::uint64_t> result;
    std::vector<std::uint64_t> kept;   // the non-mine ids, for the next call
    std::unordered_set<std::uint64_t> chosen;
    const auto take_counted = [&](const OfferedMessage &message) {
        result.push_back(message.id);
        kept.push_back(message.id);
        chosen.insert(message.id);
        tally.count(message);
    };

    // Shown before, still offered and not the player's own: they stay, whatever the caps say.
    for (const std::uint64_t id : shown_) {
        const auto found = by_id.find(id);
        if (found == by_id.end() || found->second->mine)
            continue;
        take_counted(*found->second);
    }

    for (const OfferedMessage *message : unique) {
        if (message->mine)
            result.push_back(message->id);
    }

    std::vector<Candidate> candidates;
    for (const OfferedMessage *message : unique) {
        if (!message->mine && !chosen.contains(message->id))
            candidates.push_back({draw_key(seed_, *message), message});
    }
    std::ranges::sort(candidates, [](const Candidate &a, const Candidate &b) {
        if (a.key != b.key)
            return a.key > b.key;
        return a.message->id < b.message->id;
    });
    for (const Candidate &candidate : candidates) {
        if (tally.has_room(*candidate.message))
            take_counted(*candidate.message);
    }

    shown_ = std::move(kept);
    return result;
}

void MessageSample::clear() {
    shown_.clear();
}

} // namespace verdite
