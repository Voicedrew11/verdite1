#include <kf/lib/codec.h>

#include <algorithm>
#include <optional>

using namespace kf::codec;

namespace {
constexpr u32 tim_magic = 0x10;
constexpr u32 clut_flag = 1 << 3;
constexpr u16 texture_width = 1024, texture_height = 512;
} // namespace

KfTimImage::Block KfTimImage::read_block(Reader &input)
{
    // Low two size bits are ignored by the authored transfer format.
    const u32 size = input.u32_le() & ~3u;
    require(size >= 12, "invalid TIM block size");
    Reader block(input.take(size - 4));
    const auto x = block.s16_le(), y = block.s16_le();
    const auto width = block.s16_le(), height = block.s16_le();
    require(width >= 0 && height >= 0, "invalid TIM rectangle");
    return {x, y, static_cast<u16>(width), static_cast<u16>(height),
        block.take(product(product(width, height), 2))};
}

std::optional<KfTimImage> KfTimImage::read(Reader &input)
{
    if (input.remaining() < 4 || input.u32_le() != tim_magic)
        return std::nullopt;
    const auto start = input.position() - 4;
    const auto mode = input.u32_le();
    require((mode & 7) <= 3 && (mode & ~(7u | clut_flag)) == 0, "unsupported TIM mode");
    KfTimImage image;
    image.format_ = static_cast<PixelFormat>(mode & 7);
    if (mode & clut_flag)
        image.clut_ = read_block(input);
    image.pixels_ = read_block(input);
    const auto size = input.position() - start;
    require(size <= UINT32_MAX, "TIM size exceeds format limit");
    image.encoded_bytes_ = static_cast<u32>(size);
    return image;
}

std::optional<KfTimImage> KfTimImage::parse(Bytes bytes, std::size_t offset)
{
    auto reader = reader_at(bytes, offset);
    return read(reader);
}

std::pair<u32, u32> KfTimImage::dimensions() const
{
    u32 width = pixels_.width;
    const u32 height = pixels_.height;
    switch (format_) {
    case PixelFormat::Indexed4: width *= 4; break;
    case PixelFormat::Indexed8: width *= 2; break;
    case PixelFormat::Direct16: break;
    case PixelFormat::Direct24:
        require(width * 2 % 3 == 0, "invalid TIM dimensions");
        width = width * 2 / 3;
        break;
    }
    require(width != 0 && height != 0, "empty TIM image");
    return {width, height};
}

void KfTimImage::validate_rectangle(const Block &block)
{
    require(block.x >= 0 && block.x < texture_width && block.y >= 0 && block.y < texture_height &&
        block.width <= texture_width && block.height <= texture_height, "invalid texture rectangle");
}

void KfTimImage::copy_block(const Block &block, std::span<u16> words)
{
    Reader pixels(block.pixels);
    for (std::size_t y = 0; y < block.height; ++y) {
        for (std::size_t x = 0; x < block.width; ++x) {
            // Authored CLUT transfers cross row 511 and wrap at the image edges.
            const auto row = (std::size_t(block.y) + y) % texture_height;
            const auto column = (std::size_t(block.x) + x) % texture_width;
            words[row * texture_width + column] = pixels.u16_le();
        }
    }
}

KfTimInfo KfTimImage::info() const
{
    const auto [width, height] = dimensions();
    return {u32(format_) | (clut_ ? clut_flag : 0), width, height, encoded_bytes_,
        pixels_.x, pixels_.y, clut_ ? clut_->x : 0, clut_ ? clut_->y : 0};
}

void KfTimImage::rgba(u32 palette_row, std::span<u8> rgba) const
{
    const auto [width, height] = dimensions();
    const auto count = product(width, height);
    output_fits(rgba.size() >= product(count, 4));
    Bytes palette;
    if (format_ == PixelFormat::Indexed4 || format_ == PixelFormat::Indexed8) {
        require(clut_.has_value(), "indexed TIM has no palette");
        const std::size_t colors = format_ == PixelFormat::Indexed4 ? 16 : 256;
        palette = slice(clut_->pixels, product(palette_row, colors * 2), colors * 2);
    }
    Reader pixels(pixels_.pixels);
    u8 packed = 0;
    for (std::size_t index = 0; index < count; ++index) {
        auto output = rgba.subspan(index * 4, 4);
        if (format_ == PixelFormat::Direct24) {
            std::ranges::copy(pixels.take(3), output.begin());
            output[3] = 255;
            continue;
        }
        u16 word;
        if (format_ == PixelFormat::Direct16) {
            word = pixels.u16_le();
        } else {
            u8 entry;
            if (format_ == PixelFormat::Indexed4) {
                if (index % 2 == 0)
                    packed = pixels.byte();
                entry = field<0, 4>(packed >> ((index % 2) * 4));
            } else {
                entry = pixels.byte();
            }
            word = reader_at(palette, std::size_t(entry) * 2).u16_le();
        }
        const std::array channels {field<0, 5>(word), field<5, 5>(word), field<10, 5>(word)};
        for (std::size_t channel = 0; channel < channels.size(); ++channel)
            output[channel] = (channels[channel] << 3) | (channels[channel] >> 2);
        output[3] = word == 0 ? 0 : 255;
    }
}

void kf_tim_compose(std::span<const u8> bytes, std::span<u16> words)
{
    output_fits(words.size() >= texture_width * texture_height);
    Reader input(bytes);
    std::vector<KfTimImage> images;
    while (auto image = KfTimImage::read(input)) {
        require(image->format_ != KfTimImage::PixelFormat::Direct24, "unsupported texture format");
        KfTimImage::validate_rectangle(image->pixels_);
        if (image->clut_)
            KfTimImage::validate_rectangle(*image->clut_);
        images.push_back(*image);
    }
    require(!images.empty(), "texture contains no TIM images");
    // Validate every borrowed image before changing the destination.
    for (const auto &image : images) {
        if (image.clut_)
            KfTimImage::copy_block(*image.clut_, words);
        KfTimImage::copy_block(image.pixels_, words);
    }
}
