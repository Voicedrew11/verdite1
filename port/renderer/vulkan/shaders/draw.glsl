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
    // The update's depth cue, for fog drawn per pixel (DepthCue in backend.h).
    float cue_slope;
    float cue_projection_distance;
    float fog_r, fog_g, fog_b;
    float anisotropy;           // smooth_pages' samples along the long axis, 1: none
    float enhanced_depth;       // past this camera depth, the game's own look; 0: nowhere
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
// The world's fog at each pixel from its camera depth, or its distance to the eye.
const uint draw_fog_depth = 256u;
const uint draw_fog_distance = 512u;
// World textures are filtered from their mip chain (texture_mips.h).
const uint draw_mipmaps = 1024u;

const uint blend_opaque = 0u;
const uint blend_average = 1u;
const uint blend_add = 2u;
const uint blend_subtract = 3u;
const uint blend_add_quarter = 4u;

const uint texturing_solid = 0u;
const uint texturing_modulated = 1u;
const uint texturing_raw = 2u;
