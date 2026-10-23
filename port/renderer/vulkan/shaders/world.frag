#version 450
#extension GL_GOOGLE_include_directive : require
// The PS1's colour path: texture modulation, dither and 15-bit colour (or the
// true-colour extension), with blending left to the hardware. Each blend mode
// picks an output alpha that makes the fixed blend equation blend only the
// texels with the STP bit (see blend_output).
#include "draw.glsl"

layout(set = 0, binding = 0) uniform sampler2DArray pages;

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec3 in_shade;
layout(location = 2) flat in uvec4 in_uv_bounds;
layout(location = 3) flat in uvec4 in_flags;   // layer, texturing, dither, world

layout(location = 0) out vec4 out_color;

const float rgb8_max = 255.0;
const int rgb5_max = 31;
// Decoded texels mark STP with alpha 128 and opaque texels with 255.
const float stp_alpha_threshold = 0.75;
const int dither_offsets[16] = int[16](-4, 0, -3, 1, 2, -2, 3, -1, -3, 1, -4, 0, 3, -1, 2, -2);

// Smoothed sampling for world surfaces (Balanced). Taps stay inside the face's own
// texel rectangle, transparent texels are left out, and transparency and STP
// come from the nearest texel so cut-outs keep their original edge.
vec4 filtered(vec4 nearest) {
    vec2 size = vec2(textureSize(pages, 0).xy);
    vec2 position = in_uv * size - 0.5;
    vec2 f = fract(position);
    ivec2 base = ivec2(floor(position));
    ivec2 low = ivec2(in_uv_bounds.xy);
    ivec2 high = max(low, ivec2(in_uv_bounds.zw) - 1);
    int layer = int(in_flags.x);
    vec4 t00 = texelFetch(pages, ivec3(clamp(base, low, high), layer), 0);
    vec4 t10 = texelFetch(pages, ivec3(clamp(base + ivec2(1, 0), low, high), layer), 0);
    vec4 t01 = texelFetch(pages, ivec3(clamp(base + ivec2(0, 1), low, high), layer), 0);
    vec4 t11 = texelFetch(pages, ivec3(clamp(base + ivec2(1, 1), low, high), layer), 0);
    vec4 w;
    if ((draw.flags & draw_filter_three_point) != 0u)
        w = f.x + f.y < 1.0 ? vec4(1.0 - f.x - f.y, f.x, f.y, 0.0) : vec4(0.0, 1.0 - f.y, 1.0 - f.x, f.x + f.y - 1.0);
    else
        w = vec4((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y);
    w *= vec4(t00.a > 0.0 ? 1.0 : 0.0, t10.a > 0.0 ? 1.0 : 0.0, t01.a > 0.0 ? 1.0 : 0.0, t11.a > 0.0 ? 1.0 : 0.0);
    float total = w.x + w.y + w.z + w.w;
    if (total <= 0.0)
        return nearest;
    return vec4((t00.rgb * w.x + t10.rgb * w.y + t01.rgb * w.z + t11.rgb * w.w) / total, nearest.a);
}

// The PS1's 15-bit colour: 5-bit channels, dithered on the original pixel grid.
// Returns 0..1 in steps of 1/31.
vec3 classic_color(vec4 texel_color, uint texturing) {
    ivec3 shade = ivec3(clamp(floor(in_shade), 0.0, rgb8_max));
    ivec3 texel = ivec3(floor(texel_color.rgb * float(rgb5_max) + 0.5));
    ivec3 foreground = shade;
    if (texturing == texturing_modulated)
        foreground = (texel * shade) >> 4;
    if (texturing == texturing_raw)
        foreground = texel << 3;
    int offset = 0;
    if (in_flags.z != 0u) {
        ivec2 logical = ivec2(gl_FragCoord.xy) / int(draw.pixel_scale);
        offset = dither_offsets[(logical.y & 3) * 4 + (logical.x & 3)];
    }
    return vec3(clamp((foreground + ivec3(offset)) >> 3, ivec3(0), ivec3(rgb5_max))) / float(rgb5_max);
}

// 24-bit colour, no dither (Balanced's true colour), 0..1.
vec3 true_color(vec4 texel_color, uint texturing) {
    vec3 shade = clamp(floor(in_shade), 0.0, rgb8_max);
    vec3 base = shade;
    if (texturing == texturing_modulated)
        base = texel_color.rgb * float(rgb5_max) * shade / 16.0;
    if (texturing == texturing_raw)
        base = texel_color.rgb * float(rgb5_max << 3);
    return clamp(base, 0.0, rgb8_max) / rgb8_max;
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
    uint texturing = in_flags.y;
    vec4 texel_color = vec4(1.0);
    if (texturing != texturing_solid) {
        texel_color = texture(pages, vec3(in_uv, float(in_flags.x)));
        if (texel_color.a == 0.0)
            discard;
        if ((draw.flags & (draw_filter_bilinear | draw_filter_three_point)) != 0u && in_flags.w != 0u)
            texel_color = filtered(texel_color);
    }
    vec3 color = (draw.flags & draw_true_color) != 0u ? true_color(texel_color, texturing)
                                                      : classic_color(texel_color, texturing);
    // A textured face blends only its STP texels; an untextured one blends whole.
    bool stp = texturing == texturing_solid || texel_color.a < stp_alpha_threshold;
    out_color = blend_output(color, stp);
}
