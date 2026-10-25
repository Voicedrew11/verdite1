#ifndef KF_RENDERER_VULKAN_RENDER_PASS_H
#define KF_RENDERER_VULKAN_RENDER_PASS_H
// Render passes and framebuffers for the backend's passes, which are all one
// subpass: a colour attachment and an optional depth one. Vulkan 1.0's way of
// drawing, so the backend needs no dynamic rendering (ANDROID_PLAN.md).
//
// The attachments enter and leave a pass in their attachment layouts; the
// caller moves them there and back with transition(), as around any draw.
// Depth is always cleared and never stored. Pipelines are made for a pass and
// draw in any pass of the same formats.
#include "vulkan_device.h"

#include <cstdint>
#include <span>
#include <utility>

namespace kf {
// What a pass does with its colour attachment's old contents.
enum class ColorLoad : std::uint8_t {
    Keep,      // drawn over
    Clear,     // cleared to the begin_pass colour first
    Discard,   // every pixel is drawn, so the old ones are not read
};

struct PassShape {
    VkFormat color = VK_FORMAT_UNDEFINED;
    VkFormat depth = VK_FORMAT_UNDEFINED;   // UNDEFINED: no depth attachment
    ColorLoad load = ColorLoad::Keep;
};

class RenderPass {
public:
    RenderPass() = default;
    // Null on failure.
    [[nodiscard]] static RenderPass create(VkDevice device, const PassShape &shape);
    ~RenderPass() { release(); }
    RenderPass(RenderPass &&other) noexcept { *this = std::move(other); }
    RenderPass &operator=(RenderPass &&other) noexcept;
    RenderPass(const RenderPass &) = delete;
    RenderPass &operator=(const RenderPass &) = delete;

    explicit operator bool() const { return pass_ != VK_NULL_HANDLE; }
    VkRenderPass handle() const { return pass_; }
    const PassShape &shape() const { return shape_; }

private:
    void release();
    VkDevice device_ = VK_NULL_HANDLE;
    VkRenderPass pass_ = VK_NULL_HANDLE;
    PassShape shape_;
};

class Framebuffer {
public:
    Framebuffer() = default;
    // views: the colour view, then the depth view if the pass has depth. Null on failure.
    [[nodiscard]] static Framebuffer create(VkDevice device, const RenderPass &pass,
                                            std::span<const VkImageView> views, VkExtent2D extent);
    ~Framebuffer() { release(); }
    Framebuffer(Framebuffer &&other) noexcept { *this = std::move(other); }
    Framebuffer &operator=(Framebuffer &&other) noexcept;
    Framebuffer(const Framebuffer &) = delete;
    Framebuffer &operator=(const Framebuffer &) = delete;

    explicit operator bool() const { return framebuffer_ != VK_NULL_HANDLE; }
    VkFramebuffer handle() const { return framebuffer_; }
    VkExtent2D extent() const { return extent_; }

private:
    void release();
    VkDevice device_ = VK_NULL_HANDLE;
    VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
    VkExtent2D extent_{0, 0};
};

// Begins the pass over the whole framebuffer and sets the viewport and scissor
// to it. clear_color is used by ColorLoad::Clear; depth clears to 1 (far).
void begin_pass(VkCommandBuffer commands, const RenderPass &pass, const Framebuffer &framebuffer,
                VkClearColorValue clear_color = {});
} // namespace kf

#endif // KF_RENDERER_VULKAN_RENDER_PASS_H
