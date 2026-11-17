#version 450
#extension GL_GOOGLE_include_directive : require
// Dark gaps between the game's 240 lines, the picture otherwise as drawn.
#include "screen.glsl"

// The beam's width, in lines, and how much brighter its centre is drawn to
// keep the picture's overall brightness.
const float beam_sigma = 0.3;
const float beam_boost = 1.3;

void main() {
    vec2 uv = screen_uv();
    float from_centre = fract(uv.y * screen.grid.y) - 0.5;
    float beam = exp(-from_centre * from_centre / (2.0 * beam_sigma * beam_sigma));
    // Under 3 window pixels a line, the gaps would only beat against the window's.
    float strength = clamp(screen.rect.w / screen.grid.y - 2.0, 0.0, 1.0);
    vec3 color = to_linear(picture_sharp(uv)) * mix(1.0, beam * beam_boost, strength);
    out_color = vec4(to_display(color), 1.0);
}
