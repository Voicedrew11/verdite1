#ifndef KF_RENDERER_VULKAN_PIPELINES_H
#define KF_RENDERER_VULKAN_PIPELINES_H
// The pipelines that draw a game update's faces: one per blend equation and
// depth state, made when first needed, for one colour and depth format. Also
// what the backend's other pipelines share with them.
#include "vulkan_device.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace kf {
// The push constants of shaders/draw.glsl.
struct DrawConstants {
    std::array<float, 2> view_columns;   // columns left of the original 320, half the logical width
    std::array<float, 2> center;         // projection centre in the original 320 columns
    float projection_distance;
    uint32_t flags;                      // draw_* below
    uint32_t pixel_scale;
    uint32_t blend;
    float carry;                         // FrameView::carry
    // The update's DepthCue, for fog drawn per pixel (draw_fog_*).
    float cue_slope;
    float cue_projection_distance;
    std::array<float, 3> fog_color;
    float anisotropy;                    // the trilinear sampler's, 1: none
    float enhanced_depth;                // PictureOptions::enhanced_depth
};
static_assert(sizeof(DrawConstants) == 64);
constexpr uint32_t draw_reproject = 1, draw_whole_pixels = 2, draw_true_color = 4, draw_subtract_stp_pass = 8,
                   draw_filter_bilinear = 16, draw_filter_three_point = 32, draw_occlusion = 64,
                   draw_surface_pass = 128, draw_fog_depth = 256, draw_fog_distance = 512, draw_mipmaps = 1024;

// The fixed blend equations world.frag's outputs are written for.
enum class BlendEquation : std::uint8_t {
    None,              // src
    Average,           // src * a + dst * (1 - a)
    AddWeighted,       // src + dst * a
    ReverseSubtract,   // dst - src
};

struct PipelineKey {
    BlendEquation blend = BlendEquation::None;
    bool depth_test = false;
    bool depth_write = false;
    bool operator==(const PipelineKey &) const = default;
};

// SPIR-V as glslang embeds it (generated/shaders/<name>.h). VK_NULL_HANDLE on failure.
VkShaderModule create_shader_module(VkDevice device, std::span<const uint32_t> code);

// How GpuVertex reaches world.vert, for every pipeline that draws faces.
VkPipelineVertexInputStateCreateInfo face_vertex_input();

// Clamped to the edge, mipmaps unused. VK_NULL_HANDLE on failure.
VkSampler create_sampler(VkDevice device, VkFilter filter);

// One triangle over the whole of a target of this format (fullscreen.vert),
// drawn with fragment and no blending; viewport and scissor are dynamic.
// VK_NULL_HANDLE on failure.
VkPipeline create_fullscreen_pipeline(VkDevice device, VkPipelineLayout layout, std::span<const uint32_t> fragment,
                                      VkFormat format);

// Set 0 of every pipeline that draws faces: the texture pages through a
// nearest sampler, the motions, and the pages again through a trilinear one.
// The caller owns it and keeps it past every pipeline.
constexpr uint32_t world_binding_pages = 0, world_binding_motions = 1, world_binding_smooth_pages = 2;
VkDescriptorSetLayout create_world_set_layout(VkDevice device);
// A pipeline layout over these sets with DrawConstants pushed to both stages.
VkPipelineLayout create_face_pipeline_layout(VkDevice device, std::span<const VkDescriptorSetLayout> sets);

class WorldPipelines {
    struct Key {
        explicit Key() = default;
    };

public:
    WorldPipelines(Key, VkDevice device) : device_(device) {}
    // sets: the world set, then the occlusion set (vulkan_ambient_occlusion.h).
    // Null on failure.
    static std::unique_ptr<WorldPipelines> create(VkDevice device, std::span<const VkDescriptorSetLayout> sets,
                                                  VkFormat color, VkFormat depth);
    ~WorldPipelines();
    WorldPipelines(const WorldPipelines &) = delete;
    WorldPipelines &operator=(const WorldPipelines &) = delete;

    // VK_NULL_HANDLE if it cannot be made.
    VkPipeline pipeline(PipelineKey key);
    VkPipelineLayout layout() const { return layout_; }
    VkFormat color_format() const { return color_; }

private:
    VkPipeline build(PipelineKey key) const;
    VkDevice device_;
    VkFormat color_ = VK_FORMAT_UNDEFINED, depth_ = VK_FORMAT_UNDEFINED;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkShaderModule vertex_ = VK_NULL_HANDLE, fragment_ = VK_NULL_HANDLE;
    struct Built {
        PipelineKey key;
        VkPipeline pipeline;
    };
    std::vector<Built> built_;
};
} // namespace kf

#endif // KF_RENDERER_VULKAN_PIPELINES_H
