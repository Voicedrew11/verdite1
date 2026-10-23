#ifndef KF_RENDERER_VULKAN_PIPELINES_H
#define KF_RENDERER_VULKAN_PIPELINES_H
// The pipelines that draw a game update's faces: one per blend equation and
// depth state, made when first needed, for one colour and depth format.
#include "vulkan_device.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace kf {
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

// The descriptor set every world pipeline uses: binding 0 the texture pages,
// binding 1 the motions. The caller owns it and keeps it past every pipeline.
VkDescriptorSetLayout create_world_set_layout(VkDevice device);

class WorldPipelines {
    struct Key {
        explicit Key() = default;
    };

public:
    WorldPipelines(Key, VkDevice device) : device_(device) {}
    // Null on failure.
    static std::unique_ptr<WorldPipelines> create(VkDevice device, VkDescriptorSetLayout set_layout, VkFormat color,
                                                  VkFormat depth);
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
