#ifndef VERDITE_DISC_IMAGE_H
#define VERDITE_DISC_IMAGE_H
// A CD image's data track read as one flat run of bytes, whether it is stored
// plain (.bin, .iso) or compressed as a CHD (MAME's format, read with libchdr).
// A CHD of a MODE2/2352 track reads back byte for byte as the .bin it was made
// from, so whoever parses the image cannot tell them apart. Resolving a .cue to
// its .bin is the caller's. Not shared between threads: whoever opens one reads it.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace verdite {

class DiscImage {
public:
    virtual ~DiscImage() = default;
    // The plain file's size, or a CHD track's frames times its sector size:
    // 2352 for a raw track, 2048 for MODE1 or MODE2_FORM1.
    virtual std::uint64_t size_bytes() const = 0;
    // Fills `out` with the bytes at `offset`. False past the end, on a read
    // error, or when a CHD block fails to decompress or fails its checksum.
    virtual bool read(std::uint64_t offset, std::span<std::uint8_t> out) = 0;
};

struct DiscImageOpened {
    std::unique_ptr<DiscImage> image;   // null when it could not be opened
    std::string error;                  // what to tell the player when it could not
};

// A CHD is recognised by its header, not its name; anything else is a plain
// image. Of a CHD only the first track is read, and it must be data; a CHD that
// needs a parent CHD is refused. Images over limit_bytes are refused unread.
DiscImageOpened open_disc_image(const std::filesystem::path &path, std::uint64_t limit_bytes);

} // namespace verdite

#endif
