#version 450
// The interface's pixels: the vertex colour times the texture (a white pixel
// when the draw has none), both premultiplied, blended with ONE,
// ONE_MINUS_SRC_ALPHA.
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) in vec4 colour;
layout(location = 1) in vec2 uv;
layout(location = 0) out vec4 out_colour;
void main() {
    out_colour = colour * texture(tex, uv);
}
