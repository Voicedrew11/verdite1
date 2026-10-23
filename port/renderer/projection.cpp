#include <kf/lib/fixed_math.h>
#include <kf/renderer/projection.h>

#include <algorithm>
#include <array>
#include <bit>

namespace kf {
namespace {
constexpr unsigned projection_fraction_bits = 16;
constexpr unsigned depth_cue_fraction_bits = 12;
constexpr s32 depth_cue_unity = 4096;
constexpr s32 depth_max = 65535;
constexpr s32 camera_component_min = -32768;
constexpr s32 camera_component_max = 32767;
constexpr s32 screen_coordinate_min = -1024;
constexpr s32 screen_coordinate_max = 1023;
constexpr u32 reciprocal_seed_numerator = 0x40000u;
constexpr u32 reciprocal_seed_denominator_base = 0x100u;
constexpr s32 reciprocal_seed_bias = 0x101;
constexpr u32 reciprocal_index_origin = 0x7fc0;
constexpr unsigned reciprocal_index_shift = 7;
constexpr unsigned reciprocal_refinement_shift = 8;
constexpr u32 reciprocal_refinement_rounding = 0x80;
constexpr u32 reciprocal_correction_bias = 0x2000080;
constexpr u32 projection_rounding_q16 = 0x8000;
constexpr u32 fog_distance_coefficient = 320u;
constexpr s32 fog_accumulator_bias = 0x1400000;
}

static u32 projection_scale_q16(u16 distance, u16 depth)
{
    constexpr u32 maximum_scale = 0x1ffff;
    if (u32(distance) >= u32(depth) * 2)
        return maximum_scale;

    // The original reciprocal uses an 8-bit seed and two integer refinement
    // steps. Host division, including an exact rational quotient, differs.
    static constexpr auto reciprocal_seeds = [] {
        std::array<u8, reciprocal_seed_denominator_base + 1> seeds{};
        for (u32 index = 0; index < seeds.size(); ++index) {
            const s32 seed = static_cast<s32>((reciprocal_seed_numerator / (index + reciprocal_seed_denominator_base) + 1) / 2)
                - reciprocal_seed_bias;
            seeds[index] = static_cast<u8>(std::max(seed, 0));
        }
        return seeds;
    }();

    const unsigned shift = std::countl_zero(depth);
    const u32 normalized_depth = u32(depth) << shift;
    const u32 seed = reciprocal_seeds[(normalized_depth - reciprocal_index_origin) >> reciprocal_index_shift] + reciprocal_seed_bias;
    const u32 correction = (reciprocal_correction_bias - normalized_depth * seed) >> reciprocal_refinement_shift;
    const u32 reciprocal = (reciprocal_refinement_rounding + correction * seed) >> reciprocal_refinement_shift;
    const std::uint64_t scaled = std::uint64_t(u32(distance) << shift) * reciprocal;
    return static_cast<u32>(std::min<std::uint64_t>((scaled + projection_rounding_q16) >> projection_fraction_bits, maximum_scale));
}

VECTOR render_transform_point(const MATRIX &model, const SVECTOR &point)
{
    VECTOR result = matrix_apply_rotation(model, point);
    result.vx = static_cast<s32>(static_cast<u32>(result.vx) + static_cast<u32>(model.t[0]));
    result.vy = static_cast<s32>(static_cast<u32>(result.vy) + static_cast<u32>(model.t[1]));
    result.vz = static_cast<s32>(static_cast<u32>(result.vz) + static_cast<u32>(model.t[2]));
    return result;
}

void render_place_model(MATRIX &model, const MATRIX &view, const SVECTOR &position)
{
    const VECTOR transformed = render_transform_point(view, position);
    model.t[0] = transformed.vx;
    model.t[1] = transformed.vy;
    model.t[2] = transformed.vz;
}

ProjectedPoint render_project_point(const MATRIX &model, const Projection &projection,
    const SVECTOR &point)
{
    const VECTOR camera = render_transform_point(model, point);
    const u16 depth = static_cast<u16>(std::clamp(camera.vz, 0, depth_max));
    const u32 scale = projection_scale_q16(projection.distance, depth);
    const auto screen = [scale](s32 value, s32 center) {
        const s32 offset_q16 = static_cast<s32>(static_cast<u32>(center) << projection_fraction_bits);
        const std::int64_t position_q16 =
            std::int64_t(std::clamp(value, camera_component_min, camera_component_max)) * scale + offset_q16;
        return static_cast<s16>(std::clamp<std::int64_t>(position_q16 >> projection_fraction_bits, screen_coordinate_min, screen_coordinate_max));
    };
    // Preserve the original wrapped numerator, signed division and 16-bit
    // coefficient before applying the same Q16 scale used for screen X/Y.
    const s32 fog_numerator = static_cast<s32>(static_cast<u32>(projection.fog_near) * (0u - fog_distance_coefficient));
    const s16 fog_slope = static_cast<s16>(fog_numerator / Projection::fog_reference_distance);
    const std::int64_t fog_q12 = (std::int64_t(scale) * fog_slope + fog_accumulator_bias) >> depth_cue_fraction_bits;
    // The drawn (sub-pixel) position keeps the fraction the integer transform
    // drops: each vertex rounding on its own made neighbouring vertices of a
    // model jitter against each other as the camera moved, so thin faces of
    // distant models flickered. Integer results for game logic are unchanged.
    double exact[3];
    for (unsigned row = 0; row < 3; ++row)
        exact[row] = (double(model.m[row][0]) * point.vx + double(model.m[row][1]) * point.vy + double(model.m[row][2]) * point.vz)
            / 4096.0 + model.t[row];
    // Depths near zero belong to faces the host clips at its near plane; the
    // floor keeps their interpolation weights finite.
    constexpr double minimum_perspective_depth = 16;
    const double exact_depth = std::max(exact[2], minimum_perspective_depth);
    const auto precise = [&](double value, s32 center) {
        const double position = value * projection.distance / exact_depth + center;
        return static_cast<float>(std::clamp(position, double(screen_coordinate_min), double(screen_coordinate_max)));
    };
    return {screen(camera.vx, projection.center_x), screen(camera.vy, projection.center_y),
        depth, static_cast<s32>(std::clamp<std::int64_t>(fog_q12, 0, depth_cue_unity)),
        precise(exact[0], projection.center_x), precise(exact[1], projection.center_y),
        static_cast<float>(exact_depth),
        static_cast<float>(exact[0]), static_cast<float>(exact[1]), static_cast<float>(exact[2])};
}
}
