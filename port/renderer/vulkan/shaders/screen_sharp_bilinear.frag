#version 450
#extension GL_GOOGLE_include_directive : require
// Square pixels at any window size: sharp like nearest, but evenly sized.
#include "screen.glsl"

void main() {
    out_color = vec4(picture_sharp(screen_uv()), 1.0);
}
