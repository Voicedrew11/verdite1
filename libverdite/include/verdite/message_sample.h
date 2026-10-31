#ifndef VERDITE_MESSAGE_SAMPLE_H
#define VERDITE_MESSAGE_SAMPLE_H
// Which of a floor's messages a player sees: a fraction of them, at most a few in each block of
// cells and a cap on the floor, drawn at random weighted by rating, from a seed new each session
// (docs/MESSAGES_PLAN.md, "The fraction a player sees"). Game-agnostic: a message is its id, its
// cell and its rating; the game draws and reads them. A message once shown stays shown while the
// game keeps offering it, so marks do not come and go as gossip brings more.
#include <cstdint>
#include <span>
#include <vector>

namespace verdite {

struct OfferedMessage {
    std::uint64_t id = 0;
    int cell_x = 0;
    int cell_z = 0;
    std::uint32_t good = 0;   // ratings: weight 1 + good
    bool mine = false;        // always shown, and not counted against the caps
};

struct MessageSampleShape {
    int block_cells = 8;           // blocks of block_cells by block_cells cells
    std::uint32_t per_block = 2;
    std::uint32_t per_floor = 24;
};

class MessageSample {
public:
    MessageSample(std::uint64_t seed, MessageSampleShape shape = {});

    // The ids to show from what is offered now: every one shown before that is still offered,
    // then the player's own, then new draws until the caps are met. A draw depends only on the
    // seed and the message, not the order offered.
    std::vector<std::uint64_t> shown(std::span<const OfferedMessage> offered);

    // A new floor: forgets what was shown, keeps the seed.
    void clear();

private:
    MessageSampleShape shape_;
    std::uint64_t seed_ = 0;
    std::vector<std::uint64_t> shown_;
};

} // namespace verdite

#endif // VERDITE_MESSAGE_SAMPLE_H
