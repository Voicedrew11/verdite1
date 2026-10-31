#include <verdite/message_sample.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <vector>

using namespace verdite;

namespace {

OfferedMessage message(std::uint64_t id, int cell_x, int cell_z, std::uint32_t good = 0, bool mine = false) {
    return {id, cell_x, cell_z, good, mine};
}

std::vector<std::uint64_t> sorted(std::vector<std::uint64_t> ids) {
    std::ranges::sort(ids);
    return ids;
}

std::size_t count_of(const std::vector<std::uint64_t> &ids, std::uint64_t id) {
    return static_cast<std::size_t>(std::ranges::count(ids, id));
}

} // namespace

TEST_CASE("at most per_block in a block, and per_floor on the floor") {
    MessageSample crowded(1, {8, 2, 24});
    std::vector<OfferedMessage> one_block;
    one_block.reserve(50);
    for (int i = 0; i < 50; ++i)
        one_block.push_back(message(static_cast<std::uint64_t>(i) + 1, i % 8, 0));
    CHECK(crowded.shown(one_block).size() == 2);

    MessageSample spread(2, {8, 2, 24});
    std::vector<OfferedMessage> many_blocks;
    many_blocks.reserve(100);
    for (int i = 0; i < 100; ++i)
        many_blocks.push_back(message(static_cast<std::uint64_t>(i) + 1, i * 8, 0));
    CHECK(spread.shown(many_blocks).size() == 24);
}

TEST_CASE("the player's own are always shown and do not count against the caps") {
    MessageSample sample(3, {8, 2, 4});
    std::vector<OfferedMessage> offered;
    offered.reserve(15);
    for (std::uint64_t id = 100; id < 105; ++id)
        offered.push_back(message(id, 0, 0, 0, true));
    for (int i = 0; i < 10; ++i)
        offered.push_back(message(static_cast<std::uint64_t>(i) + 1, i * 8, 0));
    const auto ids = sample.shown(offered);
    CHECK(ids.size() == 9);
    for (std::uint64_t id = 100; id < 105; ++id)
        CHECK(count_of(ids, id) == 1);
    std::size_t others = 0;
    for (std::uint64_t id = 1; id <= 10; ++id)
        others += count_of(ids, id);
    CHECK(others == 4);
}

TEST_CASE("a message shown before stays while the offer grows") {
    MessageSample sample(4, {8, 2, 24});
    std::vector<OfferedMessage> offered;
    offered.reserve(40);
    for (int i = 0; i < 10; ++i)
        offered.push_back(message(static_cast<std::uint64_t>(i) + 1, 0, 0));
    const auto first = sample.shown(offered);
    REQUIRE(first.size() == 2);

    for (int i = 0; i < 30; ++i)
        offered.push_back(message(static_cast<std::uint64_t>(i) + 50, (i + 1) * 8, 0));
    const auto later = sample.shown(offered);
    for (const std::uint64_t id : first)
        CHECK(count_of(later, id) == 1);
    // The contested block still holds two, not a third.
    std::size_t in_block_zero = 0;
    for (const std::uint64_t id : later) {
        if (id <= 10)
            ++in_block_zero;
    }
    CHECK(in_block_zero == 2);
}

TEST_CASE("a shown message no longer offered disappears") {
    MessageSample sample(5, {8, 2, 24});
    REQUIRE(sample.shown(std::vector{message(1, 0, 0)}) == std::vector<std::uint64_t>{1});
    CHECK(sample.shown(std::vector{message(2, 0, 0)}) == std::vector<std::uint64_t>{2});
}

TEST_CASE("the same seed and offer in another order give the same set") {
    std::vector<OfferedMessage> offered;
    offered.reserve(60);
    for (int i = 0; i < 60; ++i)
        offered.push_back(message(static_cast<std::uint64_t>(i) * 7 + 3, i % 5 - 2, i % 3, static_cast<std::uint32_t>(i % 4)));
    std::vector<OfferedMessage> reversed(offered.rbegin(), offered.rend());

    MessageSample forward(6, {8, 2, 24});
    MessageSample backward(6, {8, 2, 24});
    CHECK(sorted(forward.shown(offered)) == sorted(backward.shown(reversed)));
}

TEST_CASE("another seed draws another set for a large offer") {
    std::vector<OfferedMessage> offered;
    offered.reserve(200);
    for (int i = 0; i < 200; ++i)
        offered.push_back(message(static_cast<std::uint64_t>(i) + 1, i * 8, 0));
    MessageSample one(7, {8, 2, 24});
    MessageSample two(8, {8, 2, 24});
    CHECK(sorted(one.shown(offered)) != sorted(two.shown(offered)));
}

TEST_CASE("a higher good is drawn more often") {
    const std::vector<OfferedMessage> offered = {message(1, 0, 0, 0), message(2, 1, 1, 9)};
    int good_shown = 0;
    for (std::uint64_t seed = 0; seed < 1000; ++seed) {
        MessageSample sample(seed, {8, 1, 24});
        const auto ids = sample.shown(offered);
        REQUIRE(ids.size() == 1);
        if (ids[0] == 2)
            ++good_shown;
    }
    // Weights 10 and 1: about 909 of 1000 expected.
    CHECK(good_shown > 800);
}

TEST_CASE("negative cells fall into the block below them") {
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        MessageSample sample(seed, {8, 1, 24});
        const std::vector<OfferedMessage> offered = {
            message(1, -1, -1), message(2, -8, -8),   // both in block (-1, -1)
            message(3, 0, 0), message(4, 7, 7),       // both in block (0, 0)
        };
        const auto ids = sample.shown(offered);
        CHECK(count_of(ids, 1) + count_of(ids, 2) == 1);
        CHECK(count_of(ids, 3) + count_of(ids, 4) == 1);
        CHECK(ids.size() == 2);
    }
}

TEST_CASE("a block below one cell counts as one cell") {
    MessageSample sample(9, {0, 1, 24});
    const std::vector<OfferedMessage> offered = {message(1, 0, 0), message(2, 1, 0), message(3, 0, 0)};
    const auto ids = sample.shown(offered);
    CHECK(ids.size() == 2);
    CHECK(count_of(ids, 2) == 1);
}

TEST_CASE("a duplicate id is one message") {
    MessageSample sample(10, {8, 2, 24});
    CHECK(sample.shown(std::vector{message(1, 0, 0), message(1, 0, 0)}) == std::vector<std::uint64_t>{1});
}

TEST_CASE("clear forgets what was shown and keeps the seed") {
    std::vector<OfferedMessage> offered;
    offered.reserve(10);
    for (int i = 0; i < 10; ++i)
        offered.push_back(message(static_cast<std::uint64_t>(i) + 1, 0, 0));
    MessageSample sample(11, {8, 2, 24});
    const auto first = sample.shown(offered);
    sample.clear();
    MessageSample fresh(11, {8, 2, 24});
    CHECK(sample.shown(offered) == fresh.shown(offered));
    CHECK(sorted(first) == sorted(fresh.shown(offered)));
}
