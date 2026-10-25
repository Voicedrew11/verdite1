#ifndef KF_RENDERER_VULKAN_AMBIENT_OCCLUSION_H
#define KF_RENDERER_VULKAN_AMBIENT_OCCLUSION_H
// Ambient occlusion over the surface buffer (vulkan_surfaces.h), in two passes:
//   1. occlusion: spiral samples in a disc around each surface point, rotated
//      by a 4 x 4 interleaved pattern, at the quality's resolution;
//   2. blur: the 4 x 4 box that cancels that pattern, not across depth edges.
// world.frag then darkens the game's shading by the result where its own
// fragment is the surface that was measured, so the HUD, the held weapon,
// translucent faces and a face the game's ordering drew over a nearer one keep
// their colour.
//
// The tiers (OcclusionQuality) and constants are Verdite's: RecompOne's
// GteDepth, patches 0040 and 0058. Its floor-plan term (AoWorld) is left out.
#include "vulkan_device.h"
#include "vulkan_pipelines.h"
#include "vulkan_render_pass.h"
#include "vulkan_resources.h"

#include <kf/renderer/frontend.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace kf {
class AmbientOcclusion {
    struct Key {
        explicit Key() = default;
    };

public:
    explicit AmbientOcclusion(Key, VkDevice device) : device_(device) {}
    // Null on failure.
    static std::unique_ptr<AmbientOcclusion> create(const VulkanDevice &device);
    ~AmbientOcclusion();
    AmbientOcclusion(const AmbientOcclusion &) = delete;
    AmbientOcclusion &operator=(const AmbientOcclusion &) = delete;

    // Set 1 of the world pipelines: the light occlusion leaves and its camera
    // depth, which world.frag reads. Valid from create on (one unoccluded texel
    // until the first fit), so the world pipelines can always bind it.
    VkDescriptorSetLayout set_layout() const { return set_layout_; }
    VkDescriptorSet occlusion_set() const { return world_set_; }

    // Readies the passes to read surfaces (vulkan_surfaces.h) for a target of
    // logical_width x 240 logical pixels drawn scale x scale, at this quality.
    // Remade when any of them changes; the caller has waited for the frames
    // that read the old buffers. False on failure.
    [[nodiscard]] bool fit(const GpuImage &surfaces, int logical_width, int scale, OcclusionQuality quality);
    // Records both passes, outside any rendering, after the surfaces were
    // drawn. constants: as the world pass draws; strength: 0..2, 1 is Verdite's
    // judged look.
    void record(VkCommandBuffer commands, const DrawConstants &constants, float strength);

private:
    [[nodiscard]] GpuImage make_unoccluded_image(uint32_t width, uint32_t height) const;
    void point_set(VkDescriptorSet set, VkImageView view) const;
    void draw_pass(VkCommandBuffer commands, VkPipeline pipeline, VkDescriptorSet inputs, const GpuImage &output,
                   const Framebuffer &framebuffer, VkImageLayout output_layout,
                   std::span<const std::byte> constants) const;

    VkDevice device_;
    const VulkanDevice *owner_ = nullptr;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;   // one sampled image: every set here
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet world_set_ = VK_NULL_HANDLE, occlusion_inputs_ = VK_NULL_HANDLE, blur_inputs_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    RenderPass pass_;   // both passes: every pixel drawn
    VkPipeline occlusion_pipeline_ = VK_NULL_HANDLE, blur_pipeline_ = VK_NULL_HANDLE;
    GpuImage unoccluded_;   // 1 x 1, what world.frag binds before the first fit
    GpuImage occlusion_;    // RG32F: light left, camera depth
    GpuImage blurred_;      // RG32F, what world.frag reads; kept in SHADER_READ_ONLY_OPTIMAL
    Framebuffer occlusion_framebuffer_, blurred_framebuffer_;
    VkImageView surfaces_view_ = VK_NULL_HANDLE;
    int logical_width_ = 0, scale_ = 0;
    OcclusionQuality quality_ = OcclusionQuality::Medium;
};
} // namespace kf

#endif // KF_RENDERER_VULKAN_AMBIENT_OCCLUSION_H
