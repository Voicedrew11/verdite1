#version 450
#extension GL_GOOGLE_include_directive : require
// One game update's faces, drawn as the update projected them or, when
// smoothing, re-projected from the shown camera (FrameView in backend.h).
#include "draw.glsl"

layout(location = 0) in vec3 in_screen;     // x, y, w
layout(location = 1) in vec3 in_camera;     // cx, cy, cz; cz = 0: stays put
layout(location = 2) in vec2 in_uv;
layout(location = 3) in uvec4 in_uv_bounds;
layout(location = 4) in uvec4 in_color;
layout(location = 5) in uvec4 in_flags;     // layer, texturing, dither, casts_shadow
layout(location = 6) in uvec2 in_part;      // GameFrame::parts index + 1, 0: none
layout(location = 7) in vec3 in_local;      // a model part's vertex, model space
layout(location = 8) in vec3 in_back;       // way back: model space, or screen x, y when camera-fixed
layout(location = 9) in uvec4 in_clear;     // colour before the depth cue, cue scale in quarters
layout(location = 10) in vec3 in_normal;    // model space, 4096 = unit
layout(location = 11) in uvec2 in_light_set;   // FrameView::light_sets index + 1, 0: none
layout(location = 12) in uvec4 in_tint;     // the colour the game lit

layout(set = 0, binding = 1) readonly buffer Motions {
    vec4 rows[];                            // three per motion: row-major 3 x 4
} motions;

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec3 out_shade;    // the game's colour bytes
layout(location = 2) flat out uvec4 out_uv_bounds;
layout(location = 3) flat out uvec4 out_flags;   // layer, texturing, dither, world
layout(location = 4) out vec3 out_camera;        // position in the shown camera's space; z = 0: not world
layout(location = 5) out vec3 out_clear;         // the colour bytes before the depth cue
layout(location = 6) flat out float out_cue_scale;   // 0: not cued
layout(location = 7) out vec4 out_normal;        // model space, and its length
layout(location = 8) flat out uvec4 out_light;   // light set, tint

// Perspective depth for the depth test: 0 at the near plane, 1 at the far one.
const float near_w = 16.0, far_w = 65536.0;

vec4 to_clip(vec2 screen, float w) {
    vec2 ndc = vec2((screen.x + draw.view_columns.x) / draw.view_columns.y - 1.0, screen.y / 120.0 - 1.0);
    if (w <= 0.0)
        return vec4(ndc, 0.0, 1.0);
    // Scaling by w keeps the position but makes the rasterizer interpolate
    // perspective-correctly.
    float z = far_w / (far_w - near_w) * w - far_w * near_w / (far_w - near_w);
    return vec4(ndc * w, z, w);
}

// to_clip of the camera-space point's projection, multiplied out so it stays
// linear in the point: the rasterizer then clips the face at the near plane,
// including corners behind the camera.
vec4 clip_of_camera(vec3 camera) {
    vec2 xy = vec2((camera.x * draw.projection_distance + (draw.center.x + draw.view_columns.x) * camera.z) / draw.view_columns.y,
                   (camera.y * draw.projection_distance + draw.center.y * camera.z) / 120.0) - camera.z;
    float z = far_w / (far_w - near_w) * camera.z - far_w * near_w / (far_w - near_w);
    return vec4(xy, z, camera.z);
}

bool saturated(vec2 screen) {
    return screen.x <= -1024.0 || screen.x >= 1023.0 || screen.y <= -1024.0 || screen.y >= 1023.0;
}

vec3 moved(vec3 position, uint motion) {
    uint row = motion * 3u;
    vec4 p = vec4(position, 1.0);
    return vec3(dot(motions.rows[row], p), dot(motions.rows[row + 1u], p), dot(motions.rows[row + 2u], p));
}

void main() {
    vec2 screen = in_screen.xy;
    float w = in_screen.z;
    bool fixed_to_camera = in_camera.z == 0.0;
    vec3 camera = in_camera;
    uint part = in_part.x;
    if ((draw.flags & draw_reproject) != 0u && fixed_to_camera) {
        screen += in_back.xy * draw.carry;
        if (part != 0u)
            screen = moved(vec3(screen, 0.0), part).xy;
        if ((draw.flags & draw_whole_pixels) != 0u)
            screen = floor(screen);
    } else if ((draw.flags & draw_reproject) != 0u) {
        camera = part != 0u ? moved(in_local + in_back * draw.carry, part) : moved(in_camera, 0u);
        float depth = max(camera.z, near_w);
        screen = camera.xy * draw.projection_distance / depth + draw.center;
        if ((draw.flags & draw_whole_pixels) != 0u)
            screen = floor(screen);
        if (w > 0.0)
            w = depth;
    }
    // Surfaces are measured in perspective even where the picture is affine.
    bool surface = (draw.flags & draw_surface_pass) != 0u;
    if (surface && camera.z > 0.0)
        w = max(camera.z, near_w);
    // Perspective world corners the batch left unclipped (frame_batch.cpp):
    // whole pixels keep the floored screen position where it is on screen.
    bool reprojected = (draw.flags & draw_reproject) != 0u;
    bool whole_pixels = (draw.flags & draw_whole_pixels) != 0u;
    bool perspective_world = !fixed_to_camera && (in_screen.z > 0.0 || surface);
    if (perspective_world && ((reprojected && !whole_pixels) || camera.z < near_w || saturated(in_screen.xy)))
        gl_Position = clip_of_camera(camera);
    else
        gl_Position = to_clip(screen, w);
    out_camera = fixed_to_camera ? vec3(0.0) : camera;
    out_uv = in_uv;
    out_shade = vec3(in_color.rgb);
    out_clear = vec3(in_clear.rgb);
    out_cue_scale = float(in_clear.a) / 4.0;
    out_uv_bounds = in_uv_bounds;
    out_normal = vec4(in_normal, length(in_normal));
    out_light = uvec4(in_light_set.x, in_tint.rgb);
    // Only world surfaces (perspective) are filtered.
    out_flags = uvec4(in_flags.xyz, in_screen.z > 0.0 && !fixed_to_camera ? 1u : 0u);
}
