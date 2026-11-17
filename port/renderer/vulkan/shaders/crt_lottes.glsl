// Timothy Lottes' CRT shader (public domain), after his Shadertoy original:
// each game line a beam, Gaussian across the line and along it, through a
// phosphor mask. Included after screen.glsl with CRT_CURVED 0 or 1.
const float hard_scan = -8.0;   // across a line: -8 soft, -16 hard
const float hard_pix = -3.0;    // along it: -2 soft, -4 hard
const vec2 warp = vec2(1.0 / 32.0, 1.0 / 24.0);   // the tube's curve, x and y
const float mask_dark = 0.5;
const float mask_light = 1.5;
// The mask and the gaps between beams darken the picture; this gives most of it back.
const float bright_boost = 1.4;

vec3 fetch(vec2 pos, vec2 offset) {
    return to_linear(game_pixel(ivec2(floor(pos * screen.grid + offset))));
}

// From the nearest cell's centre, in cells.
vec2 dist(vec2 pos) {
    pos *= screen.grid;
    return -((pos - floor(pos)) - 0.5);
}

float gaus(float pos, float scale) {
    return exp2(scale * pos * pos);
}

vec3 horz3(vec2 pos, float line) {
    vec3 b = fetch(pos, vec2(-1.0, line));
    vec3 c = fetch(pos, vec2(0.0, line));
    vec3 d = fetch(pos, vec2(1.0, line));
    float dst = dist(pos).x;
    float wb = gaus(dst - 1.0, hard_pix);
    float wc = gaus(dst, hard_pix);
    float wd = gaus(dst + 1.0, hard_pix);
    return (b * wb + c * wc + d * wd) / (wb + wc + wd);
}

vec3 horz5(vec2 pos, float line) {
    vec3 a = fetch(pos, vec2(-2.0, line));
    vec3 b = fetch(pos, vec2(-1.0, line));
    vec3 c = fetch(pos, vec2(0.0, line));
    vec3 d = fetch(pos, vec2(1.0, line));
    vec3 e = fetch(pos, vec2(2.0, line));
    float dst = dist(pos).x;
    float wa = gaus(dst - 2.0, hard_pix);
    float wb = gaus(dst - 1.0, hard_pix);
    float wc = gaus(dst, hard_pix);
    float wd = gaus(dst + 1.0, hard_pix);
    float we = gaus(dst + 2.0, hard_pix);
    return (a * wa + b * wb + c * wc + d * wd + e * we) / (wa + wb + wc + wd + we);
}

float scan(vec2 pos, float line) {
    return gaus(dist(pos).y + line, hard_scan);
}

// The three nearest lines' beams.
vec3 tri(vec2 pos) {
    return horz3(pos, -1.0) * scan(pos, -1.0) + horz5(pos, 0.0) * scan(pos, 0.0) +
           horz3(pos, 1.0) * scan(pos, 1.0);
}

vec2 curve(vec2 pos) {
    pos = pos * 2.0 - 1.0;
    pos *= vec2(1.0 + pos.y * pos.y * warp.x, 1.0 + pos.x * pos.x * warp.y);
    return pos * 0.5 + 0.5;
}

// At a window pixel. Flat: an aperture grille, stripes of red, green and blue.
// Curved: a shadow mask, the stripes offset row by row.
vec3 mask(vec2 px) {
#if CRT_CURVED
    px.x += px.y * 3.0;
    px.x = fract(px.x / 6.0);
#else
    px.x = fract(px.x / 3.0);
#endif
    vec3 m = vec3(mask_dark);
    if (px.x < 1.0 / 3.0)
        m.r = mask_light;
    else if (px.x < 2.0 / 3.0)
        m.g = mask_light;
    else
        m.b = mask_light;
    return m;
}

void main() {
    vec2 pos = screen_uv();
#if CRT_CURVED
    pos = curve(pos);
    if (pos.x < 0.0 || pos.y < 0.0 || pos.x > 1.0 || pos.y > 1.0) {
        out_color = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
#endif
    vec3 color = tri(pos) * mask(gl_FragCoord.xy) * bright_boost;
    out_color = vec4(to_display(color), 1.0);
}
