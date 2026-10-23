#ifndef KF_RENDERER_FRONTEND_H
#define KF_RENDERER_FRONTEND_H
// The renderer frontend: takes each game update's faces, in the game's terms
// (DrawFace, FrameStyle), and turns them into batched draws for a backend
// (backend.h). The game and the host call only this; no backend name appears
// outside renderer_start's caller.
//
// A game update's faces are sorted, batched and uploaded once. Every displayed
// frame then redraws them from a camera between the last two updates, in the
// vertex shader, so frame smoothing costs nothing on the CPU.
#include <kf/lib/geometry_types.h>
#include <kf/renderer/projection.h>
#include <kf/renderer/renderer.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>

struct ImDrawData;

namespace kf {
class RenderBackend;

// Feature levels. A backend draws up to one of them; an option above the
// backend's tier is off there, whatever the settings say.
//   Classic:  the PS1 picture: the game's per-vertex lighting, the four blend
//             modes, dither, 15-bit colour, the game's draw order.
//   Clean:    plus depth buffer, true colour, filtering, mipmaps, widescreen,
//             smoothing, supersampling.
//   Enhanced: plus dynamic lights, shadows, AO, glow, FXAA.
// Sub-pixel and perspective are Classic-or-Clean choices the game makes while
// projecting (render_subpixel, render_perspective in renderer.h).
enum class Tier : u8 { Classic, Clean, Enhanced };

enum class TextureFilter : u8 { Nearest, Bilinear, ThreePoint };

// The Display settings, one field per row.
struct PictureOptions {
    // Clean
    bool true_color = false;      // 24-bit shading and blending, no dither
    bool depth_buffer = false;    // per-pixel occlusion for world faces
    TextureFilter texture_filter = TextureFilter::Nearest;
    bool mipmaps = false;
    bool smoothing = false;       // frames drawn between game updates
    int supersample = 1;          // N x N pixels per output pixel
    // Enhanced
    bool dynamic_lights = false;
    float light_strength = 1.0f;
    bool light_occlusion = false; // solid map cells block dynamic light
    float shadows = 0.0f;         // how much a shadow removes of a light (0: off)
    float ambient_occlusion = 0.0f;
    float glow = 0.0f;            // halo around light sources (0: off)
    bool fxaa = false;
    // Presentation
    bool vsync = true;            // off: frames are shown as soon as they are drawn
};

// Where a game update saw the world from.
struct CameraPose {
    VECTOR position;              // world units
    SVECTOR rotation;             // the game's angles, 4096 to a turn
    Projection projection;
};

// A moving entity's pose this update, so smoothing can move its faces
// (DrawFace::entity) between its last two poses. yaw_sign: +1 or -1 for how
// its model applies the yaw, 0 to move it without turning.
struct EntityPose {
    u16 tag;
    s32 x, y, z;
    s32 yaw;
    int yaw_sign;
};

// A light the game placed this update, in camera space at the update's camera
// (world units). Strength and flicker are applied on every displayed frame, so
// a flame keeps moving between updates.
struct GameLight {
    float x, y, z, radius;
    float r, g, b;
    float flicker;                // 0: steady
    u32 seed;                     // tells flames apart, so they do not flicker together
};

// One game update's picture, as the game built it: faces in submission order
// with the game's sort depths, lights in camera space at this update's camera.
struct GameFrame {
    std::span<const DrawFace> faces;
    FrameStyle style;
    std::optional<CameraPose> camera;   // none: a 2D frame (menus, the opening), never smoothed
    std::span<const GameLight> lights;
    std::span<const EntityPose> entities;
    // The look input this update added to camera->rotation (angle units), so a
    // smoothed view keeps showing it while blending towards this update.
    SVECTOR look_applied{0, 0, 0};
};

// Takes the backend for the rest of the run. False if it cannot draw at all.
[[nodiscard]] bool renderer_start(std::unique_ptr<RenderBackend> backend);
void renderer_stop();
Tier renderer_tier();

void renderer_set_options(const PictureOptions &options);
// The target is logical_width x 240 logical pixels (the original 320 columns
// centred, widescreen columns either side), each drawn as scale x scale.
// False, and the old target kept, if logical_width < 320 or scale < 1.
[[nodiscard]] bool renderer_set_target(int logical_width, int scale);
int renderer_extra_columns();
PresentRect renderer_present_rect(int window_width, int window_height);

// The texture store the game uploads TIMs into. A changed page or palette is
// decoded again the next time a face uses it.
TextureStore &renderer_textures();
// Map cells that block dynamic light, row-major, non-zero = solid. Kept until
// the next call.
void renderer_set_occluders(std::span<const u8> solid, int columns, int rows, float tile_size);

// A finished game update, received at at_ns (host_clock_ns). Replaces the
// frame every present draws. Faces that are not a triangle or quad are dropped.
void renderer_submit(const GameFrame &frame, std::uint64_t at_ns);
// Draws the last submitted frame as seen at now_ns, then the overlay (may be
// null), and presents it to a window of the given size. When smoothing,
// positions blend between the last two updates while the view turns by
// pending_look (angle units), the look input the game has not consumed yet,
// so looking has no update of lag.
void renderer_present(std::uint64_t now_ns, const SVECTOR &pending_look,
                      int window_width, int window_height, const ImDrawData *overlay);
// The window as last presented, overlay included, RGBA8 with the top row first
// (KF_CAPTURE). None if nothing has been presented or the backend cannot read back.
std::optional<Image> renderer_capture();
} // namespace kf

#endif // KF_RENDERER_FRONTEND_H
