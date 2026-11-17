#version 450
#extension GL_GOOGLE_include_directive : require
// A handheld's LCD: each game pixel a flat cell, a faint dark grid between them.
#include "screen.glsl"

const float gap_darkness = 0.35;   // how much the grid takes away
const float cell_boost = 1.08;

void main() {
    vec2 cells = screen_uv() * screen.grid;
    vec3 color = to_linear(game_pixel(ivec2(floor(cells))));
    vec2 cell_px = screen.rect.zw / screen.grid;   // window pixels a cell
    vec2 f = fract(cells);
    vec2 to_edge_px = min(f, 1.0 - f) * cell_px;
    float in_cell = smoothstep(0.0, 1.0, min(to_edge_px.x, to_edge_px.y));
    // Under 3 window pixels a cell there is no room for a grid.
    float strength = clamp(min(cell_px.x, cell_px.y) - 2.0, 0.0, 1.0);
    float light = mix(1.0, mix(1.0 - gap_darkness, cell_boost, in_cell), strength);
    out_color = vec4(to_display(color * light), 1.0);
}
