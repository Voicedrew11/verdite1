#version 450
#extension GL_GOOGLE_include_directive : require
// The PS1's colour path: texture modulation, dither and 15-bit colour (or the
// true-colour extension), with blending left to the hardware. Each blend mode
// picks an output alpha that makes the fixed blend equation blend only the
// texels with the STP bit (see blend_output).
#include "draw.glsl"

// The pages with their mip chains (texture_mips.h): colour weighted by
// coverage, which is alpha. pages is nearest, smooth_pages trilinear.
layout(set = 0, binding = 0) uniform sampler2DArray pages;
layout(set = 0, binding = 2) uniform sampler2DArray smooth_pages;
// Light left by ambient occlusion and the camera depth it was measured at
// (vulkan_ambient_occlusion.h).
layout(set = 1, binding = 0) uniform sampler2D occlusion;

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec3 in_shade;
layout(location = 2) flat in uvec4 in_uv_bounds;
layout(location = 3) flat in uvec4 in_flags;   // layer, texturing, dither, world
layout(location = 4) in vec3 in_camera;        // z = 0: not world
layout(location = 5) in vec3 in_clear;         // the colour before the depth cue
layout(location = 6) flat in float in_cue_scale;   // 0: not cued

layout(location = 0) out vec4 out_color;

const float rgb8_max = 255.0;
const int rgb5_max = 31;
// Level 0 marks transparent texels with alpha 0, STP with 254, opaque with 255.
const float stp_alpha_threshold = 254.5 / 255.0;
// Depths this far apart, relative to the nearer, are different surfaces.
const float same_surface = 0.05;
const int dither_offsets[16] = int[16](-4, 0, -3, 1, 2, -2, 3, -1, -3, 1, -4, 0, 3, -1, 2, -2);

// A world fragment past the enhanced depth is drawn the game's way.
bool plain() {
    return draw.enhanced_depth > 0.0 && in_camera.z > draw.enhanced_depth;
}

// The face's texel rectangle at level 0: its first and last texel.
ivec4 face_texels() {
    ivec2 low = ivec2(in_uv_bounds.xy);
    return ivec4(low, max(low, ivec2(in_uv_bounds.zw) - 1));
}

// The mip levels whose texels stay inside the smallest aligned square that
// holds the face's rectangle, so a face never takes in a far neighbour's texels.
int square_levels(ivec4 rect) {
    ivec2 spread = rect.xy ^ rect.zw;
    return findMSB(spread.x | spread.y) + 1;
}

// The level a fragment's footprint (dx, dy: in_uv's derivatives) is nearest to,
// held inside the face's square.
int mip_level(vec2 dx, vec2 dy) {
    vec2 size = vec2(textureSize(pages, 0).xy);
    float footprint = max(length(dx * size), length(dy * size));
    int level = int(floor(log2(max(footprint, 1e-6)) + 0.5));
    return clamp(level, 0, square_levels(face_texels()));
}

// A mipmapped texel as its covered texels' colour, nearest's transparency and
// STP kept, so cut-outs keep their original edge. Nothing covered: nearest.
vec4 uncovered(vec4 texel, vec4 nearest) {
    return texel.a > 0.0 ? vec4(texel.rgb / texel.a, nearest.a) : nearest;
}

// Smoothed sampling for world surfaces (Balanced) at a level: bilinear or
// three-point taps inside the face's rectangle, weighted by their coverage.
// Sharp keeps each texel flat and blends only the screen pixel its edge
// crosses, so magnified texels come out evenly sized.
vec4 filtered(vec4 nearest, int level, vec2 dx, vec2 dy) {
    vec2 size = vec2(textureSize(pages, level).xy);
    vec2 position = in_uv * size - 0.5;
    vec2 f = fract(position);
    if ((draw.flags & draw_filter_sharp) != 0u) {
        vec2 texels_per_pixel = (abs(dx) + abs(dy)) * size;
        f = clamp((f - 0.5) / clamp(texels_per_pixel, 1e-6, 1.0) + 0.5, 0.0, 1.0);
    }
    ivec2 base = ivec2(floor(position));
    ivec4 rect = face_texels() >> level;
    int layer = int(in_flags.x);
    vec4 t00 = texelFetch(pages, ivec3(clamp(base, rect.xy, rect.zw), layer), level);
    vec4 t10 = texelFetch(pages, ivec3(clamp(base + ivec2(1, 0), rect.xy, rect.zw), layer), level);
    vec4 t01 = texelFetch(pages, ivec3(clamp(base + ivec2(0, 1), rect.xy, rect.zw), layer), level);
    vec4 t11 = texelFetch(pages, ivec3(clamp(base + ivec2(1, 1), rect.xy, rect.zw), layer), level);
    vec4 w;
    if ((draw.flags & draw_filter_three_point) != 0u)
        w = f.x + f.y < 1.0 ? vec4(1.0 - f.x - f.y, f.x, f.y, 0.0) : vec4(0.0, 1.0 - f.y, 1.0 - f.x, f.x + f.y - 1.0);
    else
        w = vec4((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y);
    vec4 sum = t00 * w.x + t10 * w.y + t01 * w.z + t11 * w.w;
    return uncovered(sum, nearest);
}

// Texels a fragment's footprint spans at level 0: the short axis, or the long
// one over the anisotropy, as the hardware picks its level.
float anisotropic_footprint(vec2 dx, vec2 dy) {
    vec2 size = vec2(textureSize(pages, 0).xy);
    float x_axis = length(dx * size), y_axis = length(dy * size);
    return max(min(x_axis, y_axis), max(x_axis, y_axis) / draw.anisotropy);
}

// Bilinear filtering from the mip chain by the hardware, trilinear and
// anisotropic. The footprint is held inside the face's square, and the centre
// inside its rectangle.
vec4 trilinear(vec4 nearest, vec2 dx, vec2 dy) {
    vec2 size = vec2(textureSize(pages, 0).xy);
    ivec4 rect = face_texels();
    float footprint = anisotropic_footprint(dx, dy);
    float limit = exp2(float(square_levels(rect)));
    float scale = footprint > limit ? limit / footprint : 1.0;
    // Half a texel of the coarser level blended, so its taps stay in the rectangle.
    float inset = 0.5 * exp2(ceil(max(log2(max(footprint * scale, 1e-6)), 0.0)));
    vec2 centre = vec2(rect.xy + rect.zw + 1) * 0.5;
    vec2 low = min(vec2(rect.xy) + inset, centre), high = max(vec2(rect.zw + 1) - inset, centre);
    vec2 uv = clamp(in_uv * size, low, high) / size;
    return uncovered(textureGrad(smooth_pages, vec3(uv, float(in_flags.x)), dx * scale, dy * scale), nearest);
}

// Nearest (or sharp) filtering with mipmaps: the original texel while it covers
// a pixel or more, fading into trilinear as it shrinks to half a pixel, where
// nearest only shimmers. Measured along the footprint's long axis: nearest takes
// one tap, so a slanted face skips texels along it whatever the anisotropy.
vec4 minified_nearest(vec4 nearest, vec2 dx, vec2 dy) {
    vec2 size = vec2(textureSize(pages, 0).xy);
    float long_axis = max(length(dx * size), length(dy * size));
    float fade = clamp(log2(max(long_axis, 1e-6)), 0.0, 1.0);
    return fade > 0.0 ? mix(nearest, trilinear(nearest, dx, dy), fade) : nearest;
}

// A world surface's texel, as the filter and mipmap options draw it.
vec4 world_texel(vec4 nearest, vec2 dx, vec2 dy) {
    bool mipmapped = (draw.flags & draw_mipmaps) != 0u;
    if ((draw.flags & draw_filter_sharp) != 0u) {
        vec4 sharp = filtered(nearest, 0, dx, dy);
        return mipmapped ? minified_nearest(sharp, dx, dy) : sharp;
    }
    bool smoothed = (draw.flags & (draw_filter_bilinear | draw_filter_three_point)) != 0u;
    if (!smoothed)
        return mipmapped ? minified_nearest(nearest, dx, dy) : nearest;
    if (mipmapped && (draw.flags & draw_filter_bilinear) != 0u)
        return trilinear(nearest, dx, dy);
    return filtered(nearest, mipmapped ? mip_level(dx, dy) : 0, dx, dy);
}

// The light ambient occlusion leaves this fragment: the measured texels around
// it that are its own surface, by bilinear weight. 1 where none are: the HUD,
// the held weapon, translucent faces, a face drawn over a nearer one.
float occlusion_light() {
    if ((draw.flags & draw_occlusion) == 0u || in_camera.z <= 0.0 || plain())
        return 1.0;
    vec2 target = vec2(draw.view_columns.y * 2.0, 240.0) * float(draw.pixel_scale);
    ivec2 size = textureSize(occlusion, 0);
    vec2 position = gl_FragCoord.xy / target * vec2(size) - 0.5;
    ivec2 base = ivec2(floor(position));
    vec2 f = fract(position);
    float sum = 0.0, total = 0.0;
    for (int i = 0; i < 4; ++i) {
        ivec2 offset = ivec2(i & 1, i >> 1);
        vec2 texel = texelFetch(occlusion, clamp(base + offset, ivec2(0), size - 1), 0).rg;
        if (abs(texel.g - in_camera.z) > same_surface * in_camera.z)
            continue;
        float weight = max((offset.x == 1 ? f.x : 1.0 - f.x) * (offset.y == 1 ? f.y : 1.0 - f.y), 1e-3);
        sum += texel.r * weight;
        total += weight;
    }
    return total > 0.0 ? sum / total : 1.0;
}

// The game's colour bytes at this fragment. Fog drawn per pixel takes the depth
// cue render_project_point gives a corner (in floats, without its rounding) at
// this pixel's depth or distance, and moves the clear colour that far, times
// the face's scale, toward the fog colour, as render_fog_color does.
vec3 shade() {
    if ((draw.flags & (draw_fog_depth | draw_fog_distance)) == 0u || in_cue_scale == 0.0 || in_camera.z <= 0.0 ||
        plain())
        return in_shade;
    float measured = (draw.flags & draw_fog_distance) != 0u ? length(in_camera) : in_camera.z;
    float cue = (16.0 * draw.cue_projection_distance * draw.cue_slope / measured + 5120.0) / 4096.0;
    vec3 fog = vec3(draw.fog_r, draw.fog_g, draw.fog_b);
    return in_clear + (fog - in_clear) * clamp(cue, 0.0, 1.0) * in_cue_scale;
}

// The PS1's 15-bit colour: 5-bit channels, dithered on the original pixel grid.
// Returns 0..1 in steps of 1/31. light: what ambient occlusion leaves, 0..1.
vec3 classic_color(vec3 game_shade, vec4 texel_color, uint texturing, float light) {
    ivec3 shade = ivec3(clamp(floor(game_shade), 0.0, rgb8_max));
    ivec3 texel = ivec3(floor(texel_color.rgb * float(rgb5_max) + 0.5));
    ivec3 foreground = shade;
    if (texturing == texturing_modulated)
        foreground = (texel * shade) >> 4;
    if (texturing == texturing_raw)
        foreground = texel << 3;
    foreground = ivec3(floor(vec3(foreground) * light + 0.5));
    int offset = 0;
    if (in_flags.z != 0u) {
        ivec2 logical = ivec2(gl_FragCoord.xy) / int(draw.pixel_scale);
        offset = dither_offsets[(logical.y & 3) * 4 + (logical.x & 3)];
    }
    return vec3(clamp((foreground + ivec3(offset)) >> 3, ivec3(0), ivec3(rgb5_max))) / float(rgb5_max);
}

// 24-bit colour, no dither (Balanced's true colour), 0..1.
vec3 true_color(vec3 game_shade, vec4 texel_color, uint texturing, float light) {
    vec3 shade = clamp(floor(game_shade), 0.0, rgb8_max);
    vec3 base = shade;
    if (texturing == texturing_modulated)
        base = texel_color.rgb * float(rgb5_max) * shade / 16.0;
    if (texturing == texturing_raw)
        base = texel_color.rgb * float(rgb5_max << 3);
    return clamp(base * light, 0.0, rgb8_max) / rgb8_max;
}

// A quarter of the colour, truncated to 15 bits as the PS1 did unless in true colour.
vec3 quarter(vec3 color) {
    if ((draw.flags & draw_true_color) != 0u)
        return color * 0.25;
    return floor(color * float(rgb5_max) / 4.0) / float(rgb5_max);
}

// Colour and alpha for the run's blend pipeline:
//   average: src * a + dst * (1 - a), a = 1/2 for STP texels, 1 otherwise
//   add, add quarter, subtract's opaque pass: src + dst * a, a = 1 STP, 0 otherwise
//   subtract's STP pass: dst - src
// A texel the pass must leave alone outputs what the equation keeps dst with.
vec4 blend_output(vec3 color, bool stp) {
    switch (draw.blend) {
    case blend_average:
        return vec4(color, stp ? 0.5 : 1.0);
    case blend_add:
        return vec4(color, stp ? 1.0 : 0.0);
    case blend_add_quarter:
        return vec4(stp ? quarter(color) : color, stp ? 1.0 : 0.0);
    case blend_subtract:
        if ((draw.flags & draw_subtract_stp_pass) != 0u)
            return vec4(stp ? color : vec3(0.0), 1.0);
        return vec4(stp ? vec3(0.0) : color, stp ? 1.0 : 0.0);
    default:
        return vec4(color, 1.0);
    }
}

void main() {
    // Taken before any fragment discards.
    vec2 uv_dx = dFdx(in_uv), uv_dy = dFdy(in_uv);
    uint texturing = in_flags.y;
    vec4 texel_color = vec4(1.0);
    if (texturing != texturing_solid) {
        texel_color = textureLod(pages, vec3(in_uv, float(in_flags.x)), 0.0);
        if (texel_color.a == 0.0)
            discard;
        if (in_flags.w != 0u && !plain())
            texel_color = world_texel(texel_color, uv_dx, uv_dy);
    }
    float light = occlusion_light();
    vec3 game_shade = shade();
    vec3 color = (draw.flags & draw_true_color) != 0u ? true_color(game_shade, texel_color, texturing, light)
                                                      : classic_color(game_shade, texel_color, texturing, light);
    // A textured face blends only its STP texels; an untextured one blends whole.
    bool stp = texturing == texturing_solid || texel_color.a < stp_alpha_threshold;
    out_color = blend_output(color, stp);
}
