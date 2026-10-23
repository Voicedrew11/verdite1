#ifndef KF_PLATFORM_HOST_H
#define KF_PLATFORM_HOST_H

#include <kf/lib/types.h>
#include <kf/lib/geometry_types.h>
#include <cstddef>

namespace kf {
struct Renderer;
struct FaceList;
struct DrawFace;
struct FrameStyle;
struct Projection;
enum class AppMode : u8 { Opening, Gameplay, Ending };
struct UpdatePacer {
    std::uint64_t deadline_ns;
    u32 steps_per_second;
    u32 fractional_ns;
};
bool host_start();
void host_shutdown();
void host_poll();
std::uint64_t host_clock_ns();
std::uint64_t host_clock_tick();
void host_wait_until_tick(std::uint64_t deadline);
// PC extension: the camera of the world frame being built (frame smoothing).
void host_frame_camera(const VECTOR &position, const SVECTOR &rotation, const Projection &projection);
// PC extension: a point light for the frame being built, in camera space.
// flicker: 0 steady .. 1 strong; seed varies the flicker phase per light.
void host_add_light(float x, float y, float z, float radius, float r, float g, float b, float flicker, u32 seed);
// PC extension: map cells that block dynamic light (non-zero = solid), row-major.
void host_light_occluders(const u8 *solid, int columns, int rows, float tile_size);
// PC extension: an entity's world position and yaw this update, for frame
// smoothing. yaw_sign: +1 or -1 for how its model applies the yaw, 0 to move it
// without turning.
void host_report_entity(u16 tag, s32 x, s32 y, s32 z, s32 yaw, int yaw_sign);
void host_wait_frame();
UpdatePacer host_begin_update_pacer(u32 steps_per_second);
void host_wait_update(UpdatePacer &pacer);
Renderer *host_renderer();
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
