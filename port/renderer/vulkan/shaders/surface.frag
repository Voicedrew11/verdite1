#version 450
#extension GL_GOOGLE_include_directive : require
// The surface pass (vulkan_surfaces.h): each opaque world face's camera depth
// and its own normal, the nearest kept by the depth test.
#include "draw.glsl"

layout(set = 0, binding = 0) uniform sampler2DArray pages;

layout(location = 0) in vec2 in_uv;
layout(location = 3) flat in uvec4 in_flags;   // layer, texturing, dither, world
layout(location = 4) in vec3 in_camera;        // z = 0: not world

layout(location = 0) out vec4 out_surface;     // normal xyz, camera depth

void main() {
    // Taken before any fragment discards. A face is flat, so the derivatives
    // of its camera position span its own plane.
    vec3 normal = cross(dFdx(in_camera), dFdy(in_camera));
    if (in_camera.z <= 0.0)
        discard;
    if (in_flags.y != texturing_solid && texture(pages, vec3(in_uv, float(in_flags.x))).a == 0.0)
        discard;
    // An edge-on face has no plane to speak of: its depth, no normal.
    if (dot(normal, normal) < 1e-12) {
        out_surface = vec4(0.0, 0.0, 0.0, in_camera.z);
        return;
    }
    normal = normalize(normal);
    // The camera looks down +z from the origin; a surface faces it.
    if (dot(normal, in_camera) > 0.0)
        normal = -normal;
    out_surface = vec4(normal, in_camera.z);
}
