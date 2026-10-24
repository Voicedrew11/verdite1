#include <verdite/memory_card.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace verdite {
namespace {

constexpr std::size_t frame_bytes = 128;
constexpr std::size_t directory_entries = 15;
constexpr std::size_t name_offset = 0x0A;
constexpr std::size_t name_capacity = 20;
constexpr std::size_t checksum_offset = 0x7F;
constexpr std::size_t first_broken_frame = 16;
constexpr std::size_t last_broken_frame = 35;
constexpr std::size_t write_test_frame = 63;
constexpr std::uint16_t no_next = 0xFFFF;

enum BlockState : std::uint8_t {
    first_block = 0x51,
    middle_block = 0x52,
    last_block = 0x53,
    free_block = 0xA0,
};

std::uint32_t le32(const std::uint8_t *p) {
    return p[0] | p[1] << 8 | p[2] << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}

std::uint16_t le16(const std::uint8_t *p) {
    return static_cast<std::uint16_t>(p[0] | p[1] << 8);
}

void put16(std::uint8_t *p, std::uint16_t value) {
    p[0] = static_cast<std::uint8_t>(value);
    p[1] = static_cast<std::uint8_t>(value >> 8);
}

void put32(std::uint8_t *p, std::uint32_t value) {
    put16(p, static_cast<std::uint16_t>(value));
    put16(p + 2, static_cast<std::uint16_t>(value >> 16));
}

// Directory entry `entry` (1..15) is frame `entry` and describes data block `entry`.
const std::uint8_t *frame(std::span<const std::uint8_t> image, std::size_t index) {
    return image.data() + index * frame_bytes;
}

void seal(std::uint8_t *frame_start) {
    std::uint8_t sum = 0;
    for (std::size_t i = 0; i < checksum_offset; ++i)
        sum ^= frame_start[i];
    frame_start[checksum_offset] = sum;
}

std::string entry_name(const std::uint8_t *entry) {
    const auto *name = reinterpret_cast<const char *>(entry + name_offset);
    return {name, static_cast<std::size_t>(std::find(name, name + name_capacity, '\0') - name)};
}

// The blocks of the file that starts at `first`, or empty when its chain is broken.
std::vector<std::size_t> chain(std::span<const std::uint8_t> image, std::size_t first) {
    const std::size_t blocks = le32(frame(image, first) + 4) / memory_card_block_bytes;
    std::vector<std::size_t> found;
    std::array<bool, directory_entries + 1> seen{};
    for (std::size_t block = first;;) {
        if (seen[block] || found.size() == blocks)
            return {};
        seen[block] = true;
        found.push_back(block);
        const std::uint8_t *entry = frame(image, block);
        // A one-block file's only block is marked as a first block.
        const std::uint8_t expected = found.size() == 1        ? first_block
                                      : found.size() == blocks ? last_block
                                                               : middle_block;
        if (entry[0] != expected)
            return {};
        const std::uint16_t next = le16(entry + 8);
        if (next == no_next)
            return found.size() == blocks ? found : std::vector<std::size_t>{};
        if (next >= directory_entries)
            return {};
        block = next + 1u;
    }
}

} // namespace

CardRead read_memory_card(std::span<const std::uint8_t> image) {
    if (image.size() != memory_card_bytes || image[0] != 'M' || image[1] != 'C')
        return {{}, "This is not a 128 KB PlayStation memory card image."};
    CardRead read;
    for (std::size_t entry = 1; entry <= directory_entries; ++entry) {
        const std::uint8_t *start = frame(image, entry);
        const std::uint32_t size = le32(start + 4);
        if (start[0] != first_block || size == 0 || size % memory_card_block_bytes != 0 ||
            size > directory_entries * memory_card_block_bytes)
            continue;
        const auto blocks = chain(image, entry);
        if (blocks.empty())
            continue;
        CardFile file{entry_name(start), {}};
        file.bytes.reserve(size);
        for (const std::size_t block : blocks) {
            const auto *data = image.data() + block * memory_card_block_bytes;
            file.bytes.insert(file.bytes.end(), data, data + memory_card_block_bytes);
        }
        read.files.push_back(std::move(file));
    }
    return read;
}

std::optional<std::vector<std::uint8_t>> write_memory_card(std::span<const CardFile> files) {
    std::size_t total = 0;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto &file = files[i];
        if (file.name.empty() || file.name.size() > name_capacity || file.bytes.empty() ||
            file.bytes.size() % memory_card_block_bytes != 0)
            return std::nullopt;
        for (std::size_t j = 0; j < i; ++j)
            if (files[j].name == file.name)
                return std::nullopt;
        total += file.bytes.size() / memory_card_block_bytes;
    }
    if (total > directory_entries)
        return std::nullopt;

    // Formatted as the BIOS formats a card: free entries, an empty broken-sector list, and the
    // write-test frame a copy of the header.
    std::vector<std::uint8_t> card(memory_card_bytes, 0);
    card[0] = 'M';
    card[1] = 'C';
    seal(card.data());
    for (std::size_t entry = 1; entry <= directory_entries; ++entry) {
        std::uint8_t *start = card.data() + entry * frame_bytes;
        start[0] = free_block;
        put16(start + 8, no_next);
        seal(start);
    }
    for (std::size_t index = first_broken_frame; index <= last_broken_frame; ++index) {
        std::uint8_t *start = card.data() + index * frame_bytes;
        put32(start, 0xFFFFFFFF);
        put16(start + 8, no_next);
        seal(start);
    }
    std::memcpy(card.data() + write_test_frame * frame_bytes, card.data(), frame_bytes);

    std::size_t block = 1;
    for (const auto &file : files) {
        const std::size_t blocks = file.bytes.size() / memory_card_block_bytes;
        for (std::size_t i = 0; i < blocks; ++i, ++block) {
            std::uint8_t *start = card.data() + block * frame_bytes;
            std::memset(start, 0, frame_bytes);
            start[0] = i == 0 ? first_block : i + 1 == blocks ? last_block : middle_block;
            // The next block's index counts from 0 for block 1.
            put16(start + 8, i + 1 == blocks ? no_next : static_cast<std::uint16_t>(block));
            if (i == 0) {
                put32(start + 4, static_cast<std::uint32_t>(file.bytes.size()));
                // The frame was zeroed, so a name shorter than 20 bytes ends in a 0.
                std::copy(file.name.begin(), file.name.end(), start + name_offset);
            }
            seal(start);
            std::memcpy(card.data() + block * memory_card_block_bytes,
                        file.bytes.data() + i * memory_card_block_bytes, memory_card_block_bytes);
        }
    }
    return card;
}

} // namespace verdite
