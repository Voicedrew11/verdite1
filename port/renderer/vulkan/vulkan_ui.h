#ifndef KF_RENDERER_VULKAN_UI_H
#define KF_RENDERER_VULKAN_UI_H
// The interface (verdite/ui_frame.h) drawn over the window's picture: one
// pipeline, premultiplied blending, a texture per id the frame uploads, and
// buffers per frame slot. From holyland's port/platform/present_vk_ui.cpp.
#include "vulkan_device.h"
#include "vulkan_render_pass.h"

#include <verdite/ui_frame.h>

#include <memory>

namespace kf {
class UiPass {
public:
    // Draws in pass (the swapchain's, which keeps the picture). Null on failure.
    [[nodiscard]] static std::unique_ptr<UiPass> create(const VulkanDevice &device, const RenderPass &pass,
                                                        int frames_in_flight);
    ~UiPass();
    UiPass(const UiPass &) = delete;
    UiPass &operator=(const UiPass &) = delete;

    // Outside the render pass, before draw: the frame's textures made and
    // uploaded, its triangles copied for this slot, textures it let go of freed
    // once no frame in flight draws them. slot: the frame's, 0 to frames - 1,
    // whose last frame has finished.
    void prepare(VkCommandBuffer commands, int slot, const verdite::ui::Frame &frame);
    // Inside the pass: the frame's draws over a target of extent.
    void draw(VkCommandBuffer commands, int slot, const verdite::ui::Frame &frame, VkExtent2D extent) const;

    struct State;   // vulkan_ui.cpp

private:
    explicit UiPass(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};
} // namespace kf

#endif // KF_RENDERER_VULKAN_UI_H
