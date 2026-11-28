#ifndef VERDITE_UI_FRAME_H
#define VERDITE_UI_FRAME_H
// One picture of the interface as triangles: what verdite/ui.h records and a
// renderer backend draws over the game (Vulkan now; any later backend reads the
// same). No RmlUi and no GPU API here. From holyland's port/ui/ui_frame.hpp.
//
// Draw each Draw in order with premultiplied-alpha blending (ONE,
// ONE_MINUS_SRC_ALPHA): the vertex colour times the texture's colour (white
// when texture is 0), clipped to clip when clipped is set. Make the frame's
// uploads before its draws and let its releases go after them.
#include <cstdint>
#include <vector>

namespace verdite::ui {

// A position in the frame's pixels, a premultiplied colour, a texture place.
struct Vertex {
    float x, y;
    std::uint8_t r, g, b, a;
    float u, v;
};
static_assert(sizeof(Vertex) == 20, "Vertex is laid out as RmlUi's");

using TextureId = std::uint32_t;   // 0: no texture

// A rectangle in the frame's pixels.
struct Rect {
    std::int32_t x, y, width, height;
};

// index_count indices from first_index, each plus vertex_offset, moved by the
// translation.
struct Draw {
    std::uint32_t first_index;
    std::uint32_t index_count;
    std::uint32_t vertex_offset;
    TextureId texture;
    float translate_x, translate_y;
    bool clipped;
    Rect clip;
};

// A texture made since the last frame (glyphs, images): RGBA8, premultiplied,
// rows top first.
struct TextureUpload {
    TextureId id = 0;
    std::int32_t width_px = 0, height_px = 0;
    std::vector<std::uint8_t> rgba;
};

struct Frame {
    std::int32_t width_px = 0, height_px = 0;
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<Draw> draws;
    std::vector<TextureUpload> uploads;
    std::vector<TextureId> releases;
};

} // namespace verdite::ui

#endif // VERDITE_UI_FRAME_H
