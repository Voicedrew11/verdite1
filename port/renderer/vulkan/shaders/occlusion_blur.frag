#version 450
// Ambient occlusion, second pass (vulkan_ambient_occlusion.h): the 4 x 4 box
// that cancels the first pass's 4 x 4 rotation, over the taps that are this
// pixel's own surface, so occlusion does not cross a silhouette.
layout(set = 0, binding = 0) uniform sampler2D occlusion;   // light left, camera depth

layout(location = 0) out vec2 out_occlusion;

// Depths this far apart, relative to this one, are different surfaces.
const float same_surface = 0.05;

void main() {
    ivec2 here = ivec2(gl_FragCoord.xy);
    ivec2 last = textureSize(occlusion, 0) - 1;
    float z = texelFetch(occlusion, here, 0).g;
    if (z <= 0.0) {
        out_occlusion = vec2(1.0, 0.0);
        return;
    }
    float sum = 0.0, count = 0.0;
    for (int y = -2; y <= 1; ++y) {
        for (int x = -2; x <= 1; ++x) {
            vec2 tap = texelFetch(occlusion, clamp(here + ivec2(x, y), ivec2(0), last), 0).rg;
            if (tap.g <= 0.0 || abs(tap.g - z) > same_surface * z)
                continue;
            sum += tap.r;
            count += 1.0;
        }
    }
    out_occlusion = vec2(sum / count, z);   // the centre tap is always counted
}
