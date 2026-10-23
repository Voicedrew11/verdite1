#ifndef KF_RENDERER_TEXTURES_H
#define KF_RENDERER_TEXTURES_H

#include <kf/platform/assets.h>

#include <vector>

namespace kf {
enum class TextureFormat : u8 { Indexed4, Indexed8, Direct16 };
struct TextureSource {
    u16 x, y, palette_x, palette_y;
    TextureFormat format;
};
struct TextureEntry {
    TextureSource source;
    u32 texture;
    bool dirty;
};
struct TextureStore {
    // Authored TIM texels and palettes share this coordinate space. It is not a
    // framebuffer, command memory, or an address space exposed to gameplay.
    std::vector<u16> words;
    std::vector<TextureEntry> entries;
};
bool texture_store_upload_tim(TextureStore *store, const u8 *bytes, std::size_t size);
// Replace only translated texels/palette entries; retain later map/menu uploads.
bool texture_store_translate_tim(TextureStore *store, const u8 *before, std::size_t before_size,
                                 const u8 *after, std::size_t after_size);
bool texture_decode(Image *image, TextureSource source, const u16 *words, std::size_t count);
u32 texture_store_resolve(TextureStore *store, TextureSource source);
void texture_store_release(TextureStore *store);
}

#endif // KF_RENDERER_TEXTURES_H
