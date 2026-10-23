#ifndef KF_RENDERER_BACKEND_H
#define KF_RENDERER_BACKEND_H
// What a graphics API implements to draw for the frontend (frontend.h): one
// class per API (Vulkan, later deko3d and citro3d) plus a null one for
// headless runs. Only the frontend calls it.
//
// The frontend hands over finished data: one vertex buffer per game update,
// already in draw order, cut into runs that share a draw state. Everything
// that varied per triangle in the GL renderer is a vertex attribute, and
// textures are layers of one array, so a run needs no binds between draws.
#include <kf/platform/assets.h>
#include <kf/renderer/frontend.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

struct SDL_Window;

namespace kf {

// Texture layers: each decoded page-and-palette pairing is one 256 x 256 RGBA8
// layer (texture_page_extent). Vulkan guarantees 256 array layers. When all are
// in use, the frontend reuses the layer least recently drawn.
inline constexpr int texture_layer_capacity = 256;

enum class VertexTexturing : u8 { Solid, Modulated, Raw };

struct GpuVertex {
    float x, y;                     // screen position at the update's camera
    float w;                        // camera depth, 0: drawn the original (affine) way
    float cx, cy, cz;               // camera-space position, cz = 0: stays put (2D, HUD, held weapon)
    float u, v;                     // 0..1 across the layer
    std::array<u16, 4> uv_bounds;   // the whole face's texel rectangle, for filtering
    std::array<u8, 4> color;        // the game's vertex colour; textured: 128 is unity
    u8 layer;
    VertexTexturing texturing;
    u8 dither;                      // 1: the PS1 dithered this face
    u8 casts_shadow;
    u16 entity;                     // index into the present's entity motions, 0: none
    u16 padding = 0;
};

// The fixed-function state a run of triangles shares (one pipeline in Vulkan).
// All four PS1 blend modes are hardware blending; nothing reads the screen back.
struct DrawState {
    BlendMode blend;
    bool depth_test;
    bool depth_write;
    bool operator==(const DrawState &) const = default;
};

struct DrawRun {
    DrawState state;
    u32 first_vertex;
    u32 vertex_count;
};

// How the uploaded update is seen in this displayed frame: the shown camera
// relative to the update's (row-major 3 x 4: rotation, then translation in
// world units), and the projection to the screen.
struct FrameView {
    std::array<float, 12> update_to_shown;
    float projection_distance;
    float center_x, center_y;
};

// An entity's offset between where it was drawn and where it is shown, in the
// update's camera space, applied around its origin.
struct EntityMotion {
    std::array<float, 3> origin;
    std::array<float, 3> offset;
    float yaw;                      // radians
};

class RenderBackend {
public:
    virtual ~RenderBackend() = default;
    virtual Tier tier() const = 0;
    virtual const char *name() const = 0;

    [[nodiscard]] virtual bool set_target(int logical_width, int scale) = 0;
    virtual void set_options(const PictureOptions &options) = 0;
    // texels: 256 x 256 RGBA8. The backend builds mipmaps if they are on.
    virtual void upload_layer(u8 layer, const Image &texels) = 0;
    virtual void set_occluders(std::span<const u8> solid, int columns, int rows, float tile_size) = 0;

    // A game update's geometry, drawn by every present until the next upload.
    virtual void upload_frame(std::span<const GpuVertex> vertices, std::span<const DrawRun> runs,
                              const FrameStyle &style) = 0;
    // Draws the uploaded frame from view, then the overlay (may be null), and
    // presents. lights: in the update's camera space, with strength and this
    // frame's flicker applied. A window of size 0 (minimised) draws nothing.
    virtual void present(const FrameView &view, std::span<const EntityMotion> entities,
                         std::span<const RenderLight> lights, int window_width, int window_height,
                         const ImDrawData *overlay) = 0;
    // The last presented image, RGBA8, top row first. The null backend has none.
    virtual std::optional<Image> capture() = 0;
};

struct BackendStart {
    std::unique_ptr<RenderBackend> backend;   // null: error says why
    std::string error;
};
// Vulkan 1.1 with dynamic rendering where available; the window must have been
// created with SDL_WINDOW_VULKAN. Validation layers on in debug builds.
BackendStart make_vulkan_backend(SDL_Window *window);
// Headless: keeps nothing, draws nothing.
std::unique_ptr<RenderBackend> make_null_backend();
} // namespace kf

#endif // KF_RENDERER_BACKEND_H
