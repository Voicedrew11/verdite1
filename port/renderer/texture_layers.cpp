#include "texture_layers.h"

#include <algorithm>
#include <utility>

namespace kf {
namespace {
// Direct colour pages have no palette: any palette names the same texels.
TextureSource normalized(TextureSource source) {
    if (source.format == TextureFormat::Direct16)
        source.palette_x = source.palette_y = 0;
    return source;
}

bool same_source(const TextureSource &a, const TextureSource &b) {
    return a.x == b.x && a.y == b.y && a.palette_x == b.palette_x && a.palette_y == b.palette_y &&
           a.format == b.format;
}
} // namespace

void TextureLayers::begin_update() { ++update_; }

std::optional<u8> TextureLayers::layer_for(TextureSource source) {
    source = normalized(source);
    const auto held = std::find_if(slots_.begin(), slots_.end(), [&](const Slot &slot) {
        return slot.used_update != 0 && same_source(slot.source, source);
    });
    if (held != slots_.end()) {
        held->used_update = update_;
        return static_cast<u8>(held - slots_.begin());
    }
    const auto oldest = std::min_element(slots_.begin(), slots_.end(),
        [](const Slot &a, const Slot &b) { return a.used_update < b.used_update; });
    if (oldest->used_update == update_)
        return std::nullopt;
    *oldest = {source, update_};
    const auto layer = static_cast<u8>(oldest - slots_.begin());
    uploads_.push_back({layer, source});
    return layer;
}

std::vector<LayerUpload> TextureLayers::take_uploads() { return std::exchange(uploads_, {}); }

void TextureLayers::invalidate() {
    slots_.fill({});
    uploads_.clear();
}
} // namespace kf
