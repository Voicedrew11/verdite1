// libFuzzer: a disc image the player hands the game, plain or CHD. Opening it and reading it
// through must not crash, whatever its header claims.
#include <verdite/disc_image.h>

#include <unistd.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <string>

namespace {
constexpr std::uint64_t limit_bytes = std::uint64_t{64} * 1024 * 1024;
constexpr std::uint64_t most_read = std::uint64_t{4} * 1024 * 1024;

const std::filesystem::path &image_path() {
    static const auto path = std::filesystem::temp_directory_path() / ("verdite-fuzz-disc-" + std::to_string(::getpid()));
    return path;
}
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    std::FILE *file = std::fopen(image_path().c_str(), "wb");
    if (!file)
        return 0;
    const bool written = std::fwrite(data, 1, size, file) == size;
    std::fclose(file);
    if (!written)
        return 0;
    auto opened = verdite::open_disc_image(image_path(), limit_bytes);
    if (!opened.image)
        return 0;
    std::array<std::uint8_t, 2352> sector{};
    const auto end = std::min(opened.image->size_bytes(), most_read);
    for (std::uint64_t at = 0; at < end; at += sector.size())
        if (!opened.image->read(at, sector))
            break;
    opened.image->read(opened.image->size_bytes() - 1, std::span(sector).first(1));
    return 0;
}
