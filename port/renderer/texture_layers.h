#ifndef KF_RENDERER_TEXTURE_LAYERS_H
#define KF_RENDERER_TEXTURE_LAYERS_H
// Which texture layer holds which page-and-palette pairing. The frontend asks
// for a layer per face while batching an update, then decodes and uploads the
// layers that were newly assigned or went stale. Knows no graphics API.
#include <kf/renderer/textures.h>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace kf {
inline constexpr int texture_layer_count = 256;

struct LayerUpload {
    u8 layer;
    TextureSource source;
};

class TextureLayers {
public:
    // Starts an update: layers used from here on are kept until the next one.
    void begin_update();
    // The layer holding source, assigning the one least recently drawn when it
    // has none. None when every layer is already in use this update.
    std::optional<u8> layer_for(TextureSource source);
    // The layers assigned since the last call, to decode and upload.
    std::vector<LayerUpload> take_uploads();
    // The texture store changed: every layer is decoded again when next used.
    void invalidate();

private:
    struct Slot {
        TextureSource source{};
        std::uint64_t used_update = 0;   // 0: holds nothing
    };
    std::array<Slot, texture_layer_count> slots_{};
    std::vector<LayerUpload> uploads_;
    std::uint64_t update_ = 1;
};
} // namespace kf

#endif // KF_RENDERER_TEXTURE_LAYERS_H
