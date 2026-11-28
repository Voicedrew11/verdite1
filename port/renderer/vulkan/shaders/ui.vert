#version 450
// The interface's triangles (vulkan_ui.cpp): a verdite::ui::Vertex in the
// window's pixels, moved by its draw's translation, put into clip space.
layout(push_constant) uniform Push {
    vec2 scale;         // 2 / the window's size
    vec2 translation;   // the draw's, in pixels
} push;
layout(location = 0) in vec2 position;
layout(location = 1) in vec4 colour;   // premultiplied
layout(location = 2) in vec2 uv;
layout(location = 0) out vec4 out_colour;
layout(location = 1) out vec2 out_uv;
void main() {
    out_colour = colour;
    out_uv = uv;
    gl_Position = vec4((position + push.translation) * push.scale - 1.0, 0.0, 1.0);
}
