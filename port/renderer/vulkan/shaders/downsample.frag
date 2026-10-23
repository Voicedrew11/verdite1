#version 450
// Supersampling's box filter: the mean of the factor x factor target pixels
// behind this picture pixel.
layout(set = 0, binding = 0) uniform sampler2D target;
layout(push_constant) uniform DownsampleConstants {
    uint factor;
} constants;

layout(location = 0) out vec4 out_color;

void main() {
    int factor = int(constants.factor);
    ivec2 origin = ivec2(gl_FragCoord.xy) * factor;
    vec3 sum = vec3(0.0);
    for (int y = 0; y < factor; ++y)
        for (int x = 0; x < factor; ++x)
            sum += texelFetch(target, origin + ivec2(x, y), 0).rgb;
    out_color = vec4(sum / float(factor * factor), 1.0);
}
