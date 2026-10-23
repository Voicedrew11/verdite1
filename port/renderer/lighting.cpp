#include <kf/renderer/lighting.h>

#include <algorithm>
#include <array>

namespace kf {
namespace {
constexpr unsigned matrix_fraction_bits = 12;
constexpr s32 matrix_unity = 4096;
constexpr unsigned lighting_color_fraction_bits = 4;
constexpr s32 lighting_color_unity = 16;
constexpr s32 color_accumulator_unity = 65536;
constexpr s32 lighting_component_min = -32768;
constexpr s32 lighting_component_max = 32767;
constexpr s32 output_color_max = 255;
}

static s32 environment_color_q4(s32 value) {
    return static_cast<s32>(static_cast<u32>(value) << lighting_color_fraction_bits);
}

static u8 fog_channel(std::int64_t color_q16, s32 far_color, s16 depth_cue) {
    // Quantize and saturate the difference before interpolation. Authored fog
    // boosts may exceed 4096; the signed 16-bit input is not a clamped fraction.
    const auto far_q16 = std::int64_t{environment_color_q4(far_color)} * matrix_unity;
    const auto difference_q4 = std::clamp(
        static_cast<s32>((far_q16 - color_q16) >> matrix_fraction_bits), lighting_component_min, lighting_component_max);
    const auto result_q4 = (color_q16 + std::int64_t{difference_q4} * depth_cue) >> matrix_fraction_bits;
    return static_cast<u8>(std::clamp<std::int64_t>(result_q4 >> lighting_color_fraction_bits, 0, output_color_max));
}

CVECTOR render_fog_color(const LightingEnvironment &environment, CVECTOR color, s32 depth_cue) {
    const auto factor = static_cast<s16>(depth_cue);
    return {
        fog_channel(std::int64_t{color.r} * color_accumulator_unity, environment.fog.r, factor),
        fog_channel(std::int64_t{color.g} * color_accumulator_unity, environment.fog.g, factor),
        fog_channel(std::int64_t{color.b} * color_accumulator_unity, environment.fog.b, factor), color.cd};
}

CVECTOR render_light_normal(const LightingEnvironment &environment, const MATRIX &lights,
    const SVECTOR &normal, CVECTOR base, s32 depth_cue) {
    const std::array<s16, 3> direction = {normal.vx, normal.vy, normal.vz};
    const std::array<s32, 3> ambient = {environment.ambient.r, environment.ambient.g, environment.ambient.b};
    const std::array<s32, 3> far_color = {environment.fog.r, environment.fog.g, environment.fog.b};
    const std::array<u8, 3> tint = {base.r, base.g, base.b};
    std::array<s32, 3> illumination;
    std::array<u8, 3> color;
    for (std::size_t row = 0; row < 3; ++row) {
        std::int64_t dot = 0;
        for (std::size_t axis = 0; axis < 3; ++axis)
            dot += std::int64_t{lights.m[row][axis]} * direction[axis];
        illumination[row] = std::clamp(static_cast<s32>(dot >> matrix_fraction_bits), 0, lighting_component_max);
    }
    for (std::size_t channel = 0; channel < 3; ++channel) {
        std::int64_t dot = std::int64_t{environment_color_q4(ambient[channel])} * matrix_unity;
        for (std::size_t light = 0; light < 3; ++light)
            dot += std::int64_t{environment.color_matrix.m[channel][light]} * illumination[light];
        // Preserve the shifted 32-bit result before positive 16-bit saturation.
        const auto intensity_q4 = std::clamp(static_cast<s32>(dot >> matrix_fraction_bits), 0, lighting_component_max);
        const auto tinted_q16 = std::int64_t{tint[channel]} * intensity_q4 * lighting_color_unity;
        color[channel] = fog_channel(tinted_q16, far_color[channel], static_cast<s16>(depth_cue));
    }
    return {color[0], color[1], color[2], base.cd};
}
}
