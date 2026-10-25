#ifndef KF_RENDERER_VULKAN_DOWNSAMPLE_H
#define KF_RENDERER_VULKAN_DOWNSAMPLE_H
// Supersampling's last step: each N x N block of the render target averaged
// into one pixel of a picture N times smaller, which is what the window shows.
// Captures still read the target itself, at its full size.
#include "vulkan_device.h"
#include "vulkan_render_pass.h"
#include "vulkan_resources.h"

#include <cstdint>
#include <memory>

namespace kf {
class Downsample {
    struct Key {
        explicit Key() = default;
    };

public:
    explicit Downsample(Key, VkDevice device) : device_(device) {}
    // Null on failure.
    static std::unique_ptr<Downsample> create(const VulkanDevice &device);
    ~Downsample();
    Downsample(const Downsample &) = delete;
    Downsample &operator=(const Downsample &) = delete;

    // Readies the picture for this target, whose sides are factor times the
    // picture's (factor >= 2). Remade when either changes; the caller has
    // waited for the frames that read the old one. False on failure.
    [[nodiscard]] bool fit(const GpuImage &target, uint32_t factor);
    // Averages the target, which must be in SHADER_READ_ONLY_OPTIMAL, into
    // picture(), left in TRANSFER_SRC_OPTIMAL for the blit to the window.
    void record(VkCommandBuffer commands);
    const GpuImage &picture() const { return picture_; }

private:
    VkDevice device_;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    RenderPass pass_;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    GpuImage picture_;
    Framebuffer framebuffer_;
    uint32_t factor_ = 0;
};
} // namespace kf

#endif // KF_RENDERER_VULKAN_DOWNSAMPLE_H
