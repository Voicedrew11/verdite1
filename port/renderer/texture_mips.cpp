#include "texture_mips.h"

#include <kf/renderer/constants.h>

#include <cstddef>

namespace kf {
namespace {
constexpr unsigned channels = 4;
constexpr unsigned alpha = 3;

Image level_zero(const Image &page) {
    Image out = page;
    for (std::size_t i = alpha; i < out.rgba.size(); i += channels) {
        if (out.rgba[i] == texture_alpha_semitransparent_marker)
            out.rgba[i] = texture_mip_stp_alpha;
    }
    return out;
}

// Each texel the rounded mean of the 2 x 2 above it, colour and coverage alike:
// colour is already weighted by coverage, since transparent texels are black.
Image half_size(const Image &above) {
    const u32 width = above.width / 2, height = above.height / 2;
    Image out{width, height, {}};
    out.rgba.resize(std::size_t{width} * height * channels);
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            for (unsigned c = 0; c < channels; ++c) {
                unsigned sum = 0;
                for (u32 j = 0; j < 2; ++j)
                    for (u32 i = 0; i < 2; ++i)
                        sum += above.rgba[((std::size_t{y} * 2 + j) * above.width + std::size_t{x} * 2 + i) * channels + c];
                out.rgba[(std::size_t{y} * width + x) * channels + c] = static_cast<u8>((sum + 2) / 4);
            }
        }
    }
    return out;
}
} // namespace

std::vector<Image> texture_mip_chain(const Image &page) {
    constexpr auto extent = static_cast<u32>(texture_page_extent);
    if (page.width != extent || page.height != extent || page.rgba.size() != std::size_t{extent} * extent * channels)
        return {};
    std::vector<Image> levels;
    levels.push_back(level_zero(page));
    while (levels.back().width > 1)
        levels.push_back(half_size(levels.back()));
    return levels;
}
} // namespace kf
