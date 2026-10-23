#version 450
// Ambient occlusion, first pass (vulkan_ambient_occlusion.h): Verdite's spiral
// of samples in a disc around each surface point, rotated by a 4 x 4
// interleaved pattern that the blur cancels.
layout(set = 0, binding = 0) uniform sampler2D surfaces;   // normal xyz, camera depth (0: none)

// OcclusionConstants in vulkan_ambient_occlusion.cpp is the same layout.
layout(push_constant) uniform OcclusionConstants {
    vec2 view_columns;          // columns left of the original 320, half the logical width
    vec2 center;                // projection centre in the original 320 columns
    vec2 size;                  // this pass's pixels
    float projection_distance;
    float strength;             // how dark full occlusion goes
    uint samples;
} constants;

layout(location = 0) out vec2 out_occlusion;   // light left, camera depth

const float radius = 512.0;        // world units: a quarter of a floor tile
const float bias = 0.08;           // a cosine: samples this near the surface's plane are the surface
const float max_depth = 24000.0;   // the game's fog has the picture by here
const float golden_angle = 2.39996323;
const float turn = 6.28318531;

vec2 logical_size() { return vec2(constants.view_columns.y * 2.0, 240.0); }

vec4 surface_at(vec2 uv) { return texture(surfaces, clamp(uv, 0.0, 1.0)); }

// Undoes the game's projection: screen = centre + xy * distance / z.
vec3 camera_at(vec2 uv, float z) {
    vec2 screen = uv * logical_size() - vec2(constants.view_columns.x, 0.0);
    return vec3((screen - constants.center) * z / constants.projection_distance, z);
}

void main() {
    vec2 uv = gl_FragCoord.xy / constants.size;
    vec4 surface = surface_at(uv);
    float z = surface.w;
    if (z <= 0.0 || z > max_depth || dot(surface.xyz, surface.xyz) < 0.25) {
        out_occlusion = vec2(1.0, z);
        return;
    }
    vec3 p = camera_at(uv, z);
    vec3 n = normalize(surface.xyz);
    // The radius as it projects at this depth, in game pixels: never so small
    // that every sample lands on this one, never more than 96.
    float radius_px = clamp(radius * constants.projection_distance / z, 1.5, 96.0);
    vec2 radius_uv = radius_px / logical_size();
    ivec2 cell = ivec2(gl_FragCoord.xy) & 3;
    float start = float((cell.y << 2) | cell.x) * (turn / 16.0);
    float occluded = 0.0;
    for (uint i = 0u; i < constants.samples; ++i) {
        float t = (float(i) + 0.5) / float(constants.samples);
        float angle = start + float(i) * golden_angle;
        vec2 sample_uv = uv + vec2(cos(angle), sin(angle)) * sqrt(t) * radius_uv;
        float sample_z = surface_at(sample_uv).w;
        if (sample_z <= 0.0)
            continue;   // no surface there: it must not occlude
        vec3 v = camera_at(sample_uv, sample_z) - p;
        float len = length(v);
        if (len < 1e-4)
            continue;
        // Falls off past the radius rather than stopping at it.
        occluded += max(0.0, dot(v / len, n) - bias) * radius / max(radius, len);
    }
    out_occlusion = vec2(clamp(1.0 - constants.strength * occluded / float(constants.samples), 0.0, 1.0), z);
}
