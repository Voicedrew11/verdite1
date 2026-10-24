#ifndef KF_RENDERER_RENDERER_H
#define KF_RENDERER_RENDERER_H

#include <kf/platform/assets.h>
#include <kf/renderer/constants.h>
#include <kf/renderer/textures.h>

#include <array>

namespace kf {
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
    // PC extension (frame smoothing, VertexCarry): a model part's vertex in
    // model space, and its way back to the update before: model space for a
    // model part, screen pixels (x, y) for camera-fixed and 2D vertices.
    float lx = 0, ly = 0, lz = 0;
    float bx = 0, by = 0, bz = 0;
    // PC extension (fog drawn per pixel): the colour before the game's depth
    // cue, in the units of r, g, b (DrawFace::depth_cue_scale).
    float clear_r = 0, clear_g = 0, clear_b = 0;
    // PC extension (lighting per pixel): the corner's normal in model space, as
    // the model holds it (4096 = unit). Read only when the face has a light set.
    float nx = 0, ny = 0, nz = 0;
};
// PC extensions, set from the Display settings.
// Camera-space depth where faces reaching behind the camera are clipped (PC extension).
inline constexpr float render_near_depth = 32.0f;
// Entity whose faces are being enqueued (PC extension; see DrawFace::entity),
// and what occupies its slot, so frame smoothing never blends one occupant
// into the next (PartOwner).
inline u16 render_entity_tag = 0;
inline u32 render_entity_identity = 0;
inline bool render_subpixel = true;     // keep projected positions' fractions
inline bool render_perspective = true;  // perspective-correct textures and shading
inline bool render_detailed_map = true; // distant map cells drawn with their near (detailed) meshes
inline bool render_dynamic_lights = false; // per-pixel point lights from torches and spells
inline bool render_light_occlusion = false; // walls (solid map cells) block dynamic light
inline int render_supersample = 1;         // render N x N pixels per output pixel and average
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
    // PC extension: the part it was drawn as (GameFrame::parts index + 1, 0: none).
    u16 part = 0;
    // PC extension (fog drawn per pixel): the game cued this face's colours by
    // this many times the depth cue of a point (projection.h): 2 for the map
    // and models, 1 for sprites, 1.5 for floor items. 0: not cued, so its
    // colours are drawn as given.
    float depth_cue_scale = 0;
    // PC extension (lighting per pixel): the host_light_set the game lit this
    // face with (0: lit at the corners only), and the colour it lit (the
    // packet's colour; 128 for a textured face).
    u16 light_set = 0;
    std::array<u8, 3> tint{};
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
// Screen rectangle of the presented picture inside a window of the given size.
struct PresentRect { int x, y, width, height; };
} // namespace kf

#endif // KF_RENDERER_RENDERER_H
