#ifndef KF_RENDERER_VULKAN_SURFACES_H
#define KF_RENDERER_VULKAN_SURFACES_H
// The nearest opaque world surface at each pixel, as the shown camera sees it:
// its camera depth and its own normal. The opaque world faces are drawn again
// for it, with a depth test, so it holds the nearest surface even where the
// game's ordering drew a farther one over it; normals are each face's own
// plane, exact up to its edges. Transparent texels, translucent faces and
// anything that is not world (2D, the HUD, the held weapon) leave no surface.
//
// Ambient occlusion reads it. It is what any other screen-space pass that needs
// the world's shape should read too (per-pixel lights, fog by distance, a
// glow's covering test), rather than measuring the world again.
#include "vulkan_device.h"
#include "vulkan_pipelines.h"
#include "vulkan_resources.h"

#include <kf/renderer/backend.h>

#include <cstdint>
#include <memory>
#include <span>

namespace kf {
// What a frame's surfaces are drawn from: the world pass's own inputs.
struct SurfaceFrame {
    VkBuffer vertices;               // the update's GpuVertex buffer
    std::span<const DrawRun> runs;   // all of them; the opaque ones are drawn
    VkDescriptorSet world_set;       // the frame slot's pages and motions
    DrawConstants constants;         // as the world pass draws them
};

class SurfaceBuffer {
    struct Key {
        explicit Key() = default;
    };

public:
    explicit SurfaceBuffer(Key, VkDevice device) : device_(device) {}
    // world_set_layout: the world pipelines' set 0. Null on failure.
    static std::unique_ptr<SurfaceBuffer> create(const VulkanDevice &device, VkDescriptorSetLayout world_set_layout,
                                                 VkFormat depth_format);
    ~SurfaceBuffer();
    SurfaceBuffer(const SurfaceBuffer &) = delete;
    SurfaceBuffer &operator=(const SurfaceBuffer &) = delete;

    // Readies the buffer at this size: the picture's, so a supersampled target
    // is measured once per shown pixel. Remade when it changes; the caller has
    // waited for the frames that read the old one. False on failure.
    [[nodiscard]] bool fit(uint32_t width, uint32_t height);
    // Draws the frame's surfaces, outside any rendering, and leaves surfaces()
    // in SHADER_READ_ONLY_OPTIMAL.
    void record(VkCommandBuffer commands, const SurfaceFrame &frame);
    // RGBA32F: normal xyz (0 for an edge-on face), camera depth (0: no surface).
    const GpuImage &surfaces() const { return surfaces_; }

private:
    VkDevice device_;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkFormat depth_format_ = VK_FORMAT_UNDEFINED;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    GpuImage surfaces_, depth_;
};
} // namespace kf

#endif // KF_RENDERER_VULKAN_SURFACES_H
