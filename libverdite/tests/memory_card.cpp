#include <verdite/memory_card.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using namespace verdite;

namespace {
constexpr std::size_t frame_bytes = 128;

std::vector<std::uint8_t> blocks_of(std::size_t count, std::uint8_t seed) {
    std::vector<std::uint8_t> bytes(count * memory_card_block_bytes);
    for (std::size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<std::uint8_t>(seed + i * 7 + i / memory_card_block_bytes);
    return bytes;
}

// A card the test expects write_memory_card to make.
std::vector<std::uint8_t> card_of(const std::vector<CardFile> &files) {
    auto card = write_memory_card(files);
    if (!card) {
        FAIL("write_memory_card refused the files");
        return {};
    }
    return *card;
}

bool makes_card(const std::vector<CardFile> &files) {
    return write_memory_card(files).has_value();
}

bool frame_sealed(const std::vector<std::uint8_t> &card, std::size_t index) {
    std::uint8_t sum = 0;
    for (std::size_t i = 0; i < frame_bytes - 1; ++i)
        sum ^= card[index * frame_bytes + i];
    return sum == card[index * frame_bytes + frame_bytes - 1];
}

// Rewrites directory entry `block` by hand, for chains write_memory_card never makes.
void set_entry(std::vector<std::uint8_t> &card, std::size_t block, std::uint8_t state,
               std::uint32_t size, std::uint16_t next) {
    std::uint8_t *entry = card.data() + block * frame_bytes;
    entry[0] = state;
    for (int i = 0; i < 4; ++i)
        entry[4 + i] = static_cast<std::uint8_t>(size >> (8 * i));
    entry[8] = static_cast<std::uint8_t>(next);
    entry[9] = static_cast<std::uint8_t>(next >> 8);
    std::uint8_t sum = 0;
    for (std::size_t i = 0; i < frame_bytes - 1; ++i)
        sum ^= entry[i];
    entry[frame_bytes - 1] = sum;
}
} // namespace

TEST_CASE("files written to a card read back in order, names kept byte for byte") {
    const std::vector<CardFile> files = {{"BISLPS-00017KF      ", blocks_of(5, 1)},
                                         {"BASLUS-00158KF2", blocks_of(1, 2)},
                                         {"OTHER", blocks_of(2, 3)}};
    const auto card = card_of(files);
    CHECK(card.size() == memory_card_bytes);
    const auto read = read_memory_card(card);
    CHECK(read.error.empty());
    REQUIRE(read.files.size() == files.size());
    for (std::size_t i = 0; i < files.size(); ++i) {
        CHECK(read.files[i].name == files[i].name);
        CHECK(read.files[i].bytes == files[i].bytes);
    }
}

TEST_CASE("a blank card is formatted as the BIOS formats one") {
    const auto card = card_of({});
    CHECK(card[0] == 'M');
    CHECK(card[1] == 'C');
    for (std::size_t index = 0; index <= 35; ++index) {
        CAPTURE(index);
        CHECK(frame_sealed(card, index));
    }
    for (std::size_t entry = 1; entry <= 15; ++entry)
        CHECK(card[entry * frame_bytes] == 0xA0);
    CHECK(std::equal(card.begin(), card.begin() + frame_bytes, card.begin() + 63 * frame_bytes));
    CHECK(read_memory_card(card).files.empty());
}

TEST_CASE("a file whose blocks are not in order is read along its chain") {
    // B is 2 blocks stored in block 3 then block 2, as a card that has had saves deleted can hold.
    auto card = card_of({{"A", blocks_of(1, 9)}, {"B", blocks_of(2, 4)}});
    const auto b = blocks_of(2, 4);
    set_entry(card, 3, 0x51, 2 * memory_card_block_bytes, 1); // first, then block 2
    set_entry(card, 2, 0x53, 0, 0xFFFF);
    card[3 * frame_bytes + 0x0A] = 'B';
    std::copy(b.begin(), b.begin() + memory_card_block_bytes,
              card.begin() + 3 * memory_card_block_bytes);
    std::copy(b.begin() + memory_card_block_bytes, b.end(),
              card.begin() + 2 * memory_card_block_bytes);
    const auto read = read_memory_card(card);
    REQUIRE(read.files.size() == 2);
    CHECK(read.files[1].bytes == b);
}

TEST_CASE("a file with a broken chain is left out and the others are still read") {
    const auto good = blocks_of(1, 5);
    SUBCASE("a chain that loops") {
        auto card = card_of({{"GOOD", good}, {"BAD", blocks_of(3, 6)}});
        set_entry(card, 3, 0x52, 0, 1); // block 3 points back to block 2
        const auto read = read_memory_card(card);
        REQUIRE(read.files.size() == 1);
        CHECK(read.files[0].bytes == good);
    }
    SUBCASE("a next block past the card") {
        auto card = card_of({{"GOOD", good}, {"BAD", blocks_of(2, 6)}});
        set_entry(card, 2, 0x51, 2 * memory_card_block_bytes, 40);
        CHECK(read_memory_card(card).files.size() == 1);
    }
    SUBCASE("a chain shorter than its size") {
        auto card = card_of({{"GOOD", good}, {"BAD", blocks_of(2, 6)}});
        set_entry(card, 2, 0x51, 3 * memory_card_block_bytes, 2);
        CHECK(read_memory_card(card).files.size() == 1);
    }
    SUBCASE("a size that is not whole blocks") {
        auto card = card_of({{"GOOD", good}, {"BAD", blocks_of(1, 6)}});
        set_entry(card, 2, 0x51, 100, 0xFFFF);
        CHECK(read_memory_card(card).files.size() == 1);
    }
}

TEST_CASE("an image that is not a card is refused") {
    CHECK_FALSE(read_memory_card(std::vector<std::uint8_t>(memory_card_bytes - 1)).error.empty());
    CHECK_FALSE(read_memory_card(std::vector<std::uint8_t>(memory_card_bytes)).error.empty());
}

TEST_CASE("files that cannot make a card are refused") {
    CHECK_FALSE(makes_card({{"BIG", blocks_of(16, 1)}}));
    CHECK_FALSE(makes_card({{"A", blocks_of(10, 1)}, {"B", blocks_of(6, 1)}}));
    CHECK(makes_card({{"A", blocks_of(10, 1)}, {"B", blocks_of(5, 1)}}));
    CHECK_FALSE(makes_card({{"", blocks_of(1, 1)}}));
    CHECK_FALSE(makes_card({{std::string(21, 'N'), blocks_of(1, 1)}}));
    CHECK(makes_card({{std::string(20, 'N'), blocks_of(1, 1)}}));
    CHECK_FALSE(makes_card({{"SAME", blocks_of(1, 1)}, {"SAME", blocks_of(1, 2)}}));
    CHECK_FALSE(makes_card({{"PART", std::vector<std::uint8_t>(100)}}));
}
