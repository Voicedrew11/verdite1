#include "frame_batch.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace kf {
namespace {
// The original rasterizer rejected triangles wider or taller than this before
// clipping; saturated behind-camera faces would otherwise fill the view.
constexpr float maximum_triangle_width = 1023;
constexpr float maximum_triangle_height = 511;
// Outline order of a face's corners; quads are drawn as (0,1,2) and (1,3,2).
constexpr std::array<unsigned, 4> quad_outline = {0, 1, 3, 2};
constexpr std::array<unsigned, 3> triangle_outline = {0, 1, 2};
constexpr int clipped_corner_capacity = 8;

struct Triangle {
    std::array<Vertex, 3> corners;
};

bool is_world(const Vertex &vertex) { return vertex.w > 0 && vertex.cz > 0; }

// Camera-fixed vertices (cz = 0: menus, the held weapon) are never clipped.
bool crosses_near(const DrawFace &face) {
    const auto count = static_cast<unsigned>(face.shape);
    bool crossing = false;
    for (unsigned i = 0; i < count; ++i) {
        const Vertex &vertex = face.vertices[i];
        if (vertex.cz == 0)
            return false;
        // Corners projected past the saturated screen range are re-projected too.
        crossing = crossing || vertex.cz < render_near_depth || vertex.x <= -1024.0f || vertex.x >= 1023.0f ||
                   vertex.y <= -1024.0f || vertex.y >= 1023.0f;
    }
    return crossing;
}

Vertex mix(const Vertex &a, const Vertex &b, float t) {
    const auto lerp = [t](float from, float to) { return from + (to - from) * t; };
    Vertex out = a;
    out.u = lerp(a.u, b.u);
    out.v = lerp(a.v, b.v);
    out.r = lerp(a.r, b.r);
    out.g = lerp(a.g, b.g);
    out.b = lerp(a.b, b.b);
    out.a = lerp(a.a, b.a);
    out.cx = lerp(a.cx, b.cx);
    out.cy = lerp(a.cy, b.cy);
    out.cz = lerp(a.cz, b.cz);
    return out;
}

// Cuts the face at the near plane in camera space, projects the new outline,
// and fans it into triangles.
void clip_at_near(const DrawFace &face, const Projection &projection, bool subpixel, std::vector<Triangle> &out) {
    const auto count = static_cast<unsigned>(face.shape);
    const unsigned *outline = count == 4 ? quad_outline.data() : triangle_outline.data();
    std::array<Vertex, clipped_corner_capacity> polygon{};
    unsigned size = 0;
    for (unsigned i = 0; i < count; ++i) {
        const Vertex &a = face.vertices[outline[i]];
        const Vertex &b = face.vertices[outline[(i + 1) % count]];
        const bool a_in = a.cz >= render_near_depth;
        const bool b_in = b.cz >= render_near_depth;
        if (a_in)
            polygon[size++] = a;
        if (a_in != b_in)
            polygon[size++] = mix(a, b, (render_near_depth - a.cz) / (b.cz - a.cz));
    }
    if (size < 3)
        return;
    const bool perspective = face.vertices[0].w > 0;
    for (unsigned i = 0; i < size; ++i) {
        Vertex &corner = polygon[i];
        const float scale = projection.distance / corner.cz;
        corner.x = corner.cx * scale + static_cast<float>(projection.center_x);
        corner.y = corner.cy * scale + static_cast<float>(projection.center_y);
        if (!subpixel) {
            corner.x = std::floor(corner.x);
            corner.y = std::floor(corner.y);
        }
        corner.w = perspective ? corner.cz : 0.0f;
    }
    for (unsigned i = 1; i + 1 < size; ++i)
        out.push_back({{polygon[0], polygon[i], polygon[i + 1]}});
}

void split_face(const DrawFace &face, const BatchRules &rules, std::vector<Triangle> &out) {
    if (rules.near_clip && crosses_near(face)) {
        clip_at_near(face, *rules.near_clip, rules.subpixel, out);
        return;
    }
    const auto &v = face.vertices;
    out.push_back({{v[0], v[1], v[2]}});
    if (face.shape == FaceShape::Quad)
        out.push_back({{v[1], v[3], v[2]}});
}

// World triangles are exempt: they were clipped at the near plane, so a large
// one is a near floor or wall, not a saturated coordinate.
bool rejected_by_size(const Triangle &triangle) {
    const auto &[a, b, c] = triangle.corners;
    if (a.cz >= render_near_depth && b.cz >= render_near_depth && c.cz >= render_near_depth)
        return false;
    const float width = std::max({a.x, b.x, c.x}) - std::min({a.x, b.x, c.x});
    const float height = std::max({a.y, b.y, c.y}) - std::min({a.y, b.y, c.y});
    return width > maximum_triangle_width || height > maximum_triangle_height;
}

u16 texel(float coordinate) {
    return static_cast<u16>(std::clamp(std::lround(coordinate * texture_uv_scale), 0L, 0xffffL));
}

u8 color_byte(float channel, float unity) {
    return static_cast<u8>(std::clamp(std::lround(channel * unity), 0L, 255L));
}

// What every vertex of a face shares.
struct FaceAttributes {
    std::array<u16, 4> uv_bounds;
    float color_unity;
    u8 layer;
    VertexTexturing texturing;
    u8 dither;
    u8 casts_shadow;
    u16 entity;
};

FaceAttributes face_attributes(const DrawFace &face, u8 layer, u16 entity) {
    const bool textured = face.material.kind == SurfaceKind::Texture;
    const bool modulated = face.material.color_mode == TextureColorMode::Modulated;
    float min_u = 1, min_v = 1, max_u = 0, max_v = 0;
    for (unsigned i = 0; i < static_cast<unsigned>(face.shape); ++i) {
        min_u = std::min(min_u, face.vertices[i].u);
        min_v = std::min(min_v, face.vertices[i].v);
        max_u = std::max(max_u, face.vertices[i].u);
        max_v = std::max(max_v, face.vertices[i].v);
    }
    FaceAttributes out{};
    out.uv_bounds = {texel(min_u), texel(min_v), texel(max_u), texel(max_v)};
    out.color_unity = textured ? texture_color_unity : color8_scale;
    out.layer = layer;
    out.texturing = !textured ? VertexTexturing::Solid : modulated ? VertexTexturing::Modulated : VertexTexturing::Raw;
    // The PS1 dithered modulated textures and gouraud-shaded solids.
    out.dither = (textured ? modulated : face.shading == FaceShading::Gouraud) ? 1 : 0;
    out.casts_shadow = face.casts_shadow ? 1 : 0;
    out.entity = entity;
    return out;
}

GpuVertex gpu_vertex(const Vertex &vertex, const FaceAttributes &face) {
    GpuVertex out{};
    out.x = vertex.x;
    out.y = vertex.y;
    out.w = vertex.w;
    out.cx = vertex.cx;
    out.cy = vertex.cy;
    out.cz = vertex.cz;
    out.u = vertex.u;
    out.v = vertex.v;
    out.uv_bounds = face.uv_bounds;
    out.color = {color_byte(vertex.r, face.color_unity), color_byte(vertex.g, face.color_unity),
                 color_byte(vertex.b, face.color_unity), color_byte(vertex.a, color8_scale)};
    out.layer = face.layer;
    out.texturing = face.texturing;
    out.dither = face.dither;
    out.casts_shadow = face.casts_shadow;
    out.entity = face.entity;
    return out;
}

// Indices into faces, in the order the game's ordering table drew them.
std::vector<std::size_t> draw_order(std::span<const DrawFace> faces) {
    std::vector<std::size_t> order(faces.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        if (faces[a].depth != faces[b].depth)
            return faces[a].depth > faces[b].depth;
        return a > b;
    });
    return order;
}

// Numbers the game's entity tags from 1 in the order they are first drawn.
class EntityNumbers {
public:
    explicit EntityNumbers(std::vector<u16> &tags) : tags_(tags) {}
    u16 number_of(u16 tag) {
        if (tag == 0)
            return 0;
        if (tag >= numbers_.size())
            numbers_.resize(static_cast<std::size_t>(tag) + 1, 0);
        if (numbers_[tag] == 0) {
            tags_.push_back(tag);
            numbers_[tag] = static_cast<u16>(tags_.size());
        }
        return numbers_[tag];
    }

private:
    std::vector<u16> &tags_;
    std::vector<u16> numbers_;
};

void append_triangle(FrameBatch &batch, const Triangle &triangle, const FaceAttributes &face, DrawState state) {
    if (batch.runs.empty() || !(batch.runs.back().state == state))
        batch.runs.push_back({state, static_cast<u32>(batch.vertices.size()), 0});
    for (const Vertex &corner : triangle.corners)
        batch.vertices.push_back(gpu_vertex(corner, face));
    batch.runs.back().vertex_count += 3;
}
} // namespace

FrameBatch batch_faces(std::span<const DrawFace> faces, const BatchRules &rules, TextureLayers &layers) {
    FrameBatch batch;
    batch.vertices.reserve(faces.size() * 6);
    EntityNumbers entities(batch.entity_tags);
    // An untextured translucent face blends the way the last textured face set.
    BlendMode preceding_blend = rules.initial_blend;
    std::vector<Triangle> triangles;
    for (const std::size_t index : draw_order(faces)) {
        const DrawFace &face = faces[index];
        if (face.shape != FaceShape::Triangle && face.shape != FaceShape::Quad)
            continue;
        const bool textured = face.material.kind == SurfaceKind::Texture;
        if (textured)
            preceding_blend = face.material.blend;
        std::optional<u8> layer = u8{0};
        if (textured)
            layer = layers.layer_for(face.material.source);
        if (!layer)
            continue;
        const BlendMode blend = face.transparency == FaceTransparency::Blend ? preceding_blend : BlendMode::opaque;
        const FaceAttributes attributes = face_attributes(face, *layer, entities.number_of(face.entity));
        triangles.clear();
        split_face(face, rules, triangles);
        for (const Triangle &triangle : triangles) {
            if (rejected_by_size(triangle))
                continue;
            const auto &[a, b, c] = triangle.corners;
            // Only world geometry is depth tested; 2D and camera-fixed faces keep
            // the game's order (the held weapon relies on its sort depth).
            const bool depth_test = rules.depth_buffer && is_world(a) && is_world(b) && is_world(c);
            // Translucent faces are tested but never hide what is drawn after them.
            const DrawState state{blend, depth_test, depth_test && blend == BlendMode::opaque};
            append_triangle(batch, triangle, attributes, state);
        }
    }
    return batch;
}
} // namespace kf
