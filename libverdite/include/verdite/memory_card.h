#ifndef VERDITE_MEMORY_CARD_H
#define VERDITE_MEMORY_CARD_H
// PlayStation memory card images: the raw 128 KB dump that emulators keep (.mcd, .mcr) and the
// C# Verdite builds write (carda.sav). A card is 16 blocks of 8 KB; block 0 is the directory,
// the other 15 hold files of whole blocks. Game-agnostic: the game names its own files. Used to
// bring saves in from a card and take them out to one.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace verdite {

inline constexpr std::size_t memory_card_bytes = std::size_t{128} * 1024;
inline constexpr std::size_t memory_card_block_bytes = std::size_t{8} * 1024;

struct CardFile {
    std::string name;                 // as stored, up to 20 bytes: trailing spaces are part of it
    std::vector<std::uint8_t> bytes;  // whole blocks
};

struct CardRead {
    std::vector<CardFile> files;      // in directory order
    std::string error;                // set when the image is not a card; files is then empty
};

// The files on a card. A file whose block chain is broken (out of range, looping, or not the
// length its directory entry gives) is left out rather than read wrong.
CardRead read_memory_card(std::span<const std::uint8_t> image);

// A formatted card holding `files`, each in consecutive blocks. Nullopt when they need more than
// 15 blocks, a name is empty, longer than 20 bytes or repeated, or a file is not whole blocks.
std::optional<std::vector<std::uint8_t>> write_memory_card(std::span<const CardFile> files);

} // namespace verdite

#endif
