// Push constants shared by world.vert and world.frag; DrawConstants in
// vulkan_backend.cpp is the same layout.
layout(push_constant) uniform DrawConstants {
    vec2 view_columns;          // columns left of the original 320, half the logical width
    vec2 center;                // projection centre in the original 320 columns
    float projection_distance;
    uint flags;                 // draw_* below
    uint pixel_scale;
    uint blend;                 // blend_* below
    float carry;                // how much of each vertex's way back is shown
} draw;

const uint draw_reproject = 1u;
const uint draw_whole_pixels = 2u;
const uint draw_true_color = 4u;
// The subtract mode is drawn twice: its opaque texels, then its STP texels.
const uint draw_subtract_stp_pass = 8u;
const uint draw_filter_bilinear = 16u;
const uint draw_filter_three_point = 32u;
// The world pass darkens its faces by the ambient occlusion (world.frag).
const uint draw_occlusion = 64u;
// The surface pass (vulkan_surfaces.h): every world face in perspective.
const uint draw_surface_pass = 128u;

const uint blend_opaque = 0u;
const uint blend_average = 1u;
const uint blend_add = 2u;
const uint blend_subtract = 3u;
const uint blend_add_quarter = 4u;

const uint texturing_solid = 0u;
const uint texturing_modulated = 1u;
const uint texturing_raw = 2u;
