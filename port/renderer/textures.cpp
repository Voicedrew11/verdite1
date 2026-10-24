#include <kf/renderer/constants.h>
#include <kf/renderer/textures.h>

#include <new>
#include <utility>
#include <vector>

namespace kf {
static constexpr std::size_t texture_word_count = texture_store_width * texture_store_height;

bool texture_store_upload_tim(TextureStore *store, const u8 *bytes, std::size_t size) try {
    if (store->words.empty())
        store->words.resize(texture_word_count);
    // The codec validates the entire stream before changing any texels, and
    // copies them before the original loader can rewind its resource arena.
    kf_tim_compose({bytes, size}, store->words);
    ++store->generation;
    return true;
} catch (const codec::Error &error) {
    error.report();
    return false;
} catch (const std::bad_alloc &) {
    return false;
}

bool texture_store_translate_tim(TextureStore *store, const u8 *before, std::size_t before_size,
                                 const u8 *after, std::size_t after_size) try {
    if (store->words.empty())
        return false;
    std::vector<u16> old_words(texture_word_count);
    std::vector<u16> new_words(texture_word_count);
    kf_tim_compose({before, before_size}, old_words);
    kf_tim_compose({after, after_size}, new_words);
    for (std::size_t i = 0; i < texture_word_count; ++i)
        if (old_words[i] != new_words[i])
            store->words[i] = new_words[i];
    ++store->generation;
    return true;
} catch (const codec::Error &error) {
    error.report();
    return false;
} catch (const std::bad_alloc &) {
    return false;
}

bool texture_decode(Image *image, TextureSource source, const u16 *words, std::size_t count) try {
    const auto mode = static_cast<unsigned>(source.format);
    if (!words || count < texture_word_count || source.format > TextureFormat::Direct16 || source.x >= texture_store_width ||
        source.y >= texture_store_height || source.palette_x >= texture_store_width || source.palette_y >= texture_store_height)
        return false;
    Image decoded{texture_page_extent, texture_page_extent, {}};
    decoded.rgba.resize(texture_page_extent * texture_page_extent * 4);
    const unsigned pixels_per_word = 4 >> mode;
    for (unsigned y = 0; y < texture_page_extent; ++y) {
        for (unsigned x = 0; x < texture_page_extent; ++x) {
            auto value = words[((source.y + y) & (texture_store_height - 1)) * texture_store_width +
                               ((source.x + x / pixels_per_word) & (texture_store_width - 1))];
            if (source.format < TextureFormat::Direct16) {
                const unsigned bits = source.format == TextureFormat::Indexed4 ? 4 : 8;
                const unsigned index =
                    (value >> ((x % pixels_per_word) * bits)) & ((1u << bits) - 1);
                value = words[source.palette_y * texture_store_width + ((source.palette_x + index) & (texture_store_width - 1))];
            }
            auto *pixel = decoded.rgba.data() + (y * texture_page_extent + x) * 4;
            for (unsigned c = 0; c < 3; ++c) {
                const unsigned component = (value >> (c * color5_bits)) & color5_max;
                pixel[c] = static_cast<u8>((component << color5_color8_shift) | (component >> color5_replication_shift));
            }
            // Preserve the per-texel transparency category, not ordinary opacity.
            pixel[3] = value == 0 ? 0 : ((value & texture_semitransparent_bit) ? texture_alpha_semitransparent_marker : texture_alpha_opaque_marker);
        }
    }
    *image = std::move(decoded);
    return true;
} catch (const std::bad_alloc &) {
    return false;
}
}
