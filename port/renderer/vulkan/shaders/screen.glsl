// What the screen shaders share (vulkan_screen_pass.h): the picture, where its
// letterbox is in the window, and the game's grid of columns by 240 lines.
layout(set = 0, binding = 0) uniform sampler2D picture;
layout(push_constant) uniform ScreenConstants {
    vec4 rect;   // x, y, width, height in window pixels
    vec2 grid;   // the game's columns and lines
} screen;

layout(location = 0) out vec4 out_color;

// 0..1 across the letterbox, top left first.
vec2 screen_uv() {
    return (gl_FragCoord.xy - screen.rect.xy) / screen.rect.zw;
}

// Window pixels to one of the picture's.
vec2 picture_scale() {
    return screen.rect.zw / vec2(textureSize(picture, 0));
}

// One of the picture's texels, clamped to its edge.
vec3 picture_texel(ivec2 texel) {
    ivec2 size = textureSize(picture, 0);
    return texelFetch(picture, clamp(texel, ivec2(0), size - 1), 0).rgb;
}

// A cell of the game's grid: the picture's texel at the cell's centre, at
// whatever resolution the picture was drawn.
vec3 game_pixel(ivec2 cell) {
    vec2 centre = (vec2(cell) + 0.5) * vec2(textureSize(picture, 0)) / screen.grid;
    return picture_texel(ivec2(floor(centre)));
}

// Bilinear between the picture's texels; pos in texels, their centres at .5.
vec3 picture_bilinear(vec2 pos) {
    vec2 p = pos - 0.5;
    ivec2 i = ivec2(floor(p));
    vec2 f = p - floor(p);
    vec3 top = mix(picture_texel(i), picture_texel(i + ivec2(1, 0)), f.x);
    vec3 bottom = mix(picture_texel(i + ivec2(0, 1)), picture_texel(i + ivec2(1, 1)), f.x);
    return mix(top, bottom, f.y);
}

// Sharp bilinear (after Themaister's and rsn8887's, public domain): each texel
// a flat square, blended with its neighbour over one window pixel at its edge.
vec3 picture_sharp(vec2 uv) {
    vec2 texel = uv * vec2(textureSize(picture, 0));
    vec2 scale = max(picture_scale(), vec2(1.0));
    vec2 region = 0.5 - 0.5 / scale;
    vec2 from_centre = fract(texel) - 0.5;
    vec2 f = (from_centre - clamp(from_centre, -region, region)) * scale + 0.5;
    return picture_bilinear(floor(texel) + f);
}

// The picture's values are display values; light adds up in linear ones.
vec3 to_linear(vec3 c) {
    return pow(c, vec3(2.2));
}
vec3 to_display(vec3 c) {
    return pow(clamp(c, 0.0, 1.0), vec3(1.0 / 2.2));
}
