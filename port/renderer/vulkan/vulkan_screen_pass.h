#ifndef KF_RENDERER_VULKAN_SCREEN_PASS_H
#define KF_RENDERER_VULKAN_SCREEN_PASS_H
// The picture's last step to the window through a screen shader (ScreenShader
// in frontend.h): drawn into its letterbox rectangle of a swapchain image, the
// rest cleared to black. ScreenShader::None does not come here; the backend
// blits the picture instead.
#include "vulkan_device.h"
#include "vulkan_render_pass.h"
#include "vulkan_resources.h"

#include <kf/renderer/frontend.h>

#include <array>
#include <cstdint>
#include <memory>

namespace kf {
// One frame's draw to the window.
struct ScreenDraw {
    ScreenShader shader;
    const GpuImage *picture;          // in SHADER_READ_ONLY_OPTIMAL
    uint32_t game_columns;            // the game's grid: this many columns by its 240 lines
    VkImage window_image;             // in any layout; left in TRANSFER_DST_OPTIMAL
    const Framebuffer *framebuffer;   // window_image as an attachment of window_format
    VkFormat window_format;
    PresentRect rect;                 // the letterbox, in window pixels
};

class ScreenPass {
    struct Key {
        explicit Key() = default;
    };

public:
    // One descriptor set per frame in flight, so a frame never rewrites one
    // that another frame still reads.
    static constexpr uint32_t frames_in_flight = 2;

    explicit ScreenPass(Key, const VulkanDevice &device) : device_(device) {}
    // Null on failure.
    static std::unique_ptr<ScreenPass> create(const VulkanDevice &device);
    ~ScreenPass();
    ScreenPass(const ScreenPass &) = delete;
    ScreenPass &operator=(const ScreenPass &) = delete;

    // Records draw for the frame in flight `frame` (whose earlier commands
    // have finished). A shader's pipeline is made the first time it is drawn.
    // False, recording nothing, if that fails; the caller blits instead.
    [[nodiscard]] bool record(VkCommandBuffer commands, uint32_t frame, const ScreenDraw &draw);

private:
    bool fit_format(VkFormat window_format);
    VkPipeline pipeline(ScreenShader shader);
    void release_pipelines();

    const VulkanDevice &device_;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, frames_in_flight> sets_{};
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    RenderPass pass_;   // the window's format, cleared first
    std::array<VkPipeline, screen_shader_count> pipelines_{};
};
} // namespace kf

#endif // KF_RENDERER_VULKAN_SCREEN_PASS_H
