#ifndef KF_PLATFORM_HOST_H
#define KF_PLATFORM_HOST_H

#include <kf/lib/types.h>
#include <kf/lib/geometry_types.h>
#include <array>
#include <cstddef>
#include <span>

namespace kf {
struct Renderer;
struct FaceList;
struct DrawFace;
struct FrameStyle;
struct Projection;
struct LightColor;
struct TextureStore;
struct PictureOptions;
struct Image;
struct CarriedModel;
enum class AppMode : u8 { Opening, Gameplay, Ending };
struct UpdatePacer {
    std::uint64_t deadline_ns;
    u32 steps_per_second;
    u32 fractional_ns;
};
// Before host_start. headless: no window or GL, dummy video and audio drivers,
// nothing drawn. turbo: the game clock jumps to each wait's deadline instead of
// waiting, so the game runs as fast as the CPU allows. Game time no longer
// depends on how long a frame took, only on what the game waited for.
struct HostOptions { bool headless = false; bool turbo = false; };
void host_configure(const HostOptions &options);
bool host_start();
void host_shutdown();
void host_poll();
std::uint64_t host_clock_ns();
std::uint64_t host_clock_tick();
void host_wait_until_tick(std::uint64_t deadline);
// PC extension: the camera of the world frame being built (frame smoothing),
// and the colour its depth cue tends to (fog drawn per pixel).
void host_frame_camera(const VECTOR &position, const SVECTOR &rotation, const Projection &projection,
    const LightColor &fog_color);
// PC extension: a point light for the frame being built, in camera space.
// flicker: 0 steady .. 1 strong; seed varies the flicker phase per light.
void host_add_light(float x, float y, float z, float radius, float r, float g, float b, float flicker, u32 seed);
// PC extension: map cells that block dynamic light (non-zero = solid), row-major.
void host_light_occluders(const u8 *solid, int columns, int rows, float tile_size);
// PC extension: an entity's world position and yaw this update, for frame
// smoothing. yaw_sign: +1 or -1 for how its model applies the yaw, 0 to move it
// without turning.
void host_report_entity(u16 tag, s32 x, s32 y, s32 z, s32 yaw, int yaw_sign);
// PC extension (frame smoothing, VertexCarry in port/renderer): where what the
// game is drawing for render_entity_tag (and render_entity_identity) was in
// the update before. A model drawn in the world: model is its model-to-camera
// transform as projected, vertices its posed vertices (frontend.h:
// CarriedModel). Camera-fixed or 2D points, in screen pixels: each one's way
// back, empty when not carried. Both views stay valid until the next call.
CarriedModel host_carry_model(const MATRIX &model, std::span<const SVECTOR> vertices);
std::span<const std::array<float, 2>> host_carry_screen(std::span<const std::array<float, 2>> points);
// PC extension (frame smoothing): the compass needle, drawn in screen space
// about this point, turns with the shown view. Returns its DrawFace::part.
u16 host_carry_dial(float pivot_x, float pivot_y);
void host_wait_frame();
UpdatePacer host_begin_update_pacer(u32 steps_per_second);
void host_wait_update(UpdatePacer &pacer);
Renderer *host_renderer();
// The texture store TIMs are uploaded into, whichever renderer draws.
TextureStore &host_textures();
// Widescreen columns either side of the original 320.
int host_extra_columns();
// The picture as drawn, to be put back after a page shown over it.
bool host_capture_frame(Image &image);
bool host_restore_frame(const Image &image);
// From the settings: the render target (logical columns, pixels per logical
// pixel) and the picture options, for whichever renderer draws.
bool host_set_render_target(int logical_width, int scale);
void host_set_picture_options(const PictureOptions &options);
// Game frames handed to the host since start (for the agent's state).
std::uint64_t host_frames_presented();
// A short message shown over the picture for a couple of seconds (PC overlay).
void host_show_message(const char *text);
// PC extension: the item in the menu's preview panel for the frame being built,
// for the overlay's item description. top: the first logical row free below
// the panel's stats. Cleared by host_begin_frame.
struct MenuPreviewItem { int item = -1; s32 top = 0; };
void host_menu_preview_item(u8 item_id, s32 top);
MenuPreviewItem host_menu_preview();
void host_present_faces(const FaceList *faces);
void host_begin_frame();
void host_enqueue_face(const DrawFace &face);
void host_present_frame(const FrameStyle &style);
[[noreturn]] void host_fail(const char *message);
}

#endif // KF_PLATFORM_HOST_H
