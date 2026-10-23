#ifndef KF_RENDERER_RENDERER_H
#define KF_RENDERER_RENDERER_H

#include <kf/platform/assets.h>
#include <kf/renderer/constants.h>
#include <kf/renderer/textures.h>

#include <array>

namespace kf {
using TextureId = u32;
struct Vertex {
    float x, y, u, v;
    float r, g, b, a;
    // PC extension: camera depth for perspective-correct interpolation; 0 draws
    // the vertex the original way (2D elements and orthographic geometry).
    float w = 0;
    // PC extension: camera-space position of world geometry, used to re-draw
    // the last game update from an in-between camera. cz = 0 marks vertices
    // that stay where they are (2D, HUD, camera-attached models).
    float cx = 0, cy = 0, cz = 0;
};
// PC extensions, set from the Display settings.
// Camera-space depth where faces reaching behind the camera are clipped (PC extension).
inline constexpr float render_near_depth = 32.0f;
// Entity whose faces are being enqueued (PC extension; see DrawFace::entity).
inline u16 render_entity_tag = 0;
inline bool render_subpixel = true;     // keep projected positions' fractions
inline bool render_perspective = true;  // perspective-correct textures and shading
inline bool render_depth_buffer = false; // per-pixel occlusion for 3D faces (experimental)
inline bool render_true_color = false;   // 24-bit shading and blending, no dither
inline bool render_dynamic_lights = false; // per-pixel point lights from torches and spells
inline float render_light_strength = 1.0f;
inline bool render_light_occlusion = false; // walls (solid map cells) block dynamic light
inline bool render_ambient_occlusion = false; // screen-space AO applied at presentation
inline float render_ao_strength = 1.0f;
inline int render_texture_filter = 0;     // world textures: 0 nearest, 1 bilinear, 2 three-point
inline bool render_fxaa = false;           // edge smoothing on world pixels at presentation
inline int render_supersample = 1;         // render N x N pixels per output pixel and average
inline float render_glow = 0.0f;           // soft halo around light sources (0 = off)
inline float render_shadows = 0.0f;        // how much a shadow removes of a light (0 = off)
inline constexpr int render_shadow_lights = 4;   // nearest lights that cast shadows
inline constexpr int render_shadow_face_size = 1024;
inline bool render_smoothing = false;    // draw extra frames between game updates
inline int render_frame_limit = 0;       // smoothing frame cap: 0 = display refresh, < 0 = none
// Set by the game around camera-attached 3D (held weapon, notifications): those
// faces must not move with the in-between camera.
inline bool render_camera_fixed = false;
// Values are shared with the pixel shader's blend-mode uniform.
enum class BlendMode : u8 { opaque = 0, average = 1, add = 2, subtract = 3, add_quarter = 4 };
enum class FrameClear : u8 { Clear, Retain };
struct FrameStyle {
    FrameClear clear = FrameClear::Clear;
    BlendMode initial_blend = BlendMode::average;
    float red = 0, green = 0, blue = 0;
    int clip_x = 0, clip_y = 0, clip_width = render_width, clip_height = render_height;
};
enum class FaceShape : u8 { Triangle = 3, Quad = 4 };
enum class SurfaceKind : u8 { Solid, Texture };
enum class FaceTransparency : u8 { Opaque, Blend };
enum class FaceShading : u8 { Flat, Gouraud };
enum class TextureColorMode : u8 { Modulated, Raw };
struct FaceMaterial {
    SurfaceKind kind;
    TextureSource source;
    BlendMode blend;
    TextureColorMode color_mode = TextureColorMode::Modulated;
};
struct DrawFace {
    std::array<Vertex, 4> vertices;
    FaceShape shape;
    FaceMaterial material;
    FaceTransparency transparency;
    s32 depth;
    FaceShading shading = FaceShading::Flat;
    // PC extension: flat billboards (flames, spell and item sprites) would throw
    // their own quad's shape as a shadow, often from the light they carry.
    bool casts_shadow = true;
    // PC extension: the moving entity this face belongs to (0: none), so frame
    // smoothing can move it between its last two poses.
    u16 entity = 0;
};
struct FaceList {
    const DrawFace *faces;
    std::size_t count;
    FrameStyle style{};
};
// PC extension: a point light in camera space (world units), already scaled by
// strength and flicker.
struct RenderLight {
    float x, y, z, radius;
    float r, g, b;
    // Shadow casters closer than this to the light are ignored: a torch sits in a
    // carved niche whose own walls would otherwise throw wedges across the room.
    // Negative: the light casts no shadows.
    float shadow_near = 260.0f;
};
inline constexpr int render_light_capacity = 32;
struct Renderer {
    u32 program, vao, buffer, framebuffer, color_texture, blend_texture, depth_buffer;
    TextureId white_texture;
    TextureStore textures;
    // PC extension: the target is logical_width x render_height logical pixels
    // (the original 320 columns centred, extra columns either side for widescreen),
    // each drawn as scale x scale physical pixels.
    int logical_width = render_width;
    int scale = 1;
    RenderLight lights[render_light_capacity];
    int light_count = 0;
    // Light occlusion: the map's solid cells as a texture, and the camera of the
    // next draw (camera-to-world rotation and world position).
    u32 occluder_texture = 0;
    int occluder_columns = 0, occluder_rows = 0;
    float occluder_tile = 0;
    float camera_to_world[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    float camera_world[3] = {0, 0, 0};
    // Ambient occlusion: per-pixel camera depth (second colour attachment, packed
    // RGBA8), a half-resolution AO target, and the passes that use them.
    u32 view_depth_texture = 0, ao_texture = 0, ao_framebuffer = 0;
    u32 ao_program = 0, composite_program = 0, post_vao = 0;
    // Shadows: one depth atlas of 6 cube faces x render_shadow_lights lights.
    u32 shadow_texture = 0, shadow_framebuffer = 0, shadow_program = 0, shadow_vao = 0, shadow_buffer = 0;
    float projection_distance = 200;
};
// Screen rectangle of the presented picture inside a window of the given size.
struct PresentRect { int x, y, width, height; };
bool renderer_init(Renderer *renderer, char *error, std::size_t error_size);
void renderer_release(Renderer *renderer);
TextureId renderer_upload(const Image *image);
void renderer_delete_texture(TextureId texture);
void renderer_present_retained(const Renderer *renderer, int width, int height);
// Resizes the render target, keeping the current picture. logical_width >= 320.
bool renderer_configure(Renderer *renderer, int logical_width, int scale);
PresentRect renderer_present_rect(const Renderer *renderer, int width, int height);
// Lights for the next draw; camera space matching the faces' cx/cy/cz.
void renderer_set_lights(Renderer *renderer, const RenderLight *lights, int count);
// solid: columns x rows bytes, non-zero = blocks light. Uploaded only on change.
void renderer_set_occluders(Renderer *renderer, const u8 *solid, int columns, int rows, float tile_size);
// rotation: row-major camera-to-world 3x3; position: camera position in world units.
void renderer_set_camera(Renderer *renderer, const float rotation[9], const float position[3]);
// Projection distance of the world camera (needed to rebuild positions for AO).
void renderer_set_projection_distance(Renderer *renderer, float distance);
inline int renderer_extra_columns(const Renderer *renderer) { return (renderer->logical_width - render_width) / 2; }
bool renderer_draw_faces(Renderer *renderer, const FaceList *faces, int width, int height);
} // namespace kf

#endif // KF_RENDERER_RENDERER_H
