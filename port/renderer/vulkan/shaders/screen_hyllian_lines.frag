#version 450
#extension GL_GOOGLE_include_directive : require
// Hyllian's CRT, first pass (crt-hyllian-pass0): each game line filtered
// along its length to the letterbox's width, a Catmull-Rom cubic with
// anti-ringing, in linear light.
#include "screen.glsl"
#include "hyllian.glsl"

// Catmull-Rom (HFILTER_PROFILE 1: B = 0, C = 0.5).
const float B = 0.0;
const float C = 0.5;
const mat4 inv_x = mat4((-B - 6.0 * C) / 6.0, (3.0 * B + 12.0 * C) / 6.0, (-3.0 * B - 6.0 * C) / 6.0, B / 6.0,
                        (12.0 - 9.0 * B - 6.0 * C) / 6.0, (-18.0 + 12.0 * B + 6.0 * C) / 6.0, 0.0, (6.0 - 2.0 * B) / 6.0,
                        -(12.0 - 9.0 * B - 6.0 * C) / 6.0, (18.0 - 15.0 * B - 12.0 * C) / 6.0, (3.0 * B + 6.0 * C) / 6.0, B / 6.0,
                        (B + 6.0 * C) / 6.0, -C, 0.0, 0.0);

// A game pixel in linear light; black past the picture's edge (clamp to border).
vec3 source(ivec2 cell) {
    if (cell.x < 0 || cell.y < 0 || cell.x >= int(screen.grid.x) || cell.y >= int(screen.grid.y))
        return vec3(0.0);
    return pow(game_pixel(cell), vec3(input_gamma));
}

void main() {
    vec2 uv = screen_uv();
    if (screen.curvature > 0.5)
        uv = hyllian_warp(uv, vec2(warp_x, 0.0) * 15.0);
    vec2 pix = uv * screen.grid + vec2(-0.5, 0.0);
    ivec2 cell = ivec2(floor(pix));
    float fx = fract(pix.x);

    vec3 c10 = source(cell + ivec2(-1, 0));
    vec3 c11 = source(cell);
    vec3 c12 = source(cell + ivec2(1, 0));
    vec3 c13 = source(cell + ivec2(2, 0));

    vec4 lobes = vec4(fx * fx * fx, fx * fx, fx, 1.0) * inv_x;
    vec3 color = clamp(mat4x3(c10, c11, c12, c13) * lobes, 0.0, 1.0);

    // Anti-ringing (CRT_ANTI_RINGING 1).
    vec3 unclamped = color;
    vec3 low = min(min(c10, c11), min(c12, c13));
    vec3 high = max(max(c10, c11), max(c12, c13));
    color = clamp(color, low, high);
    color = mix(unclamped, color, step(0.0, (c10 - c11) * (c12 - c13)));

    out_color = vec4(color, 1.0);
}
