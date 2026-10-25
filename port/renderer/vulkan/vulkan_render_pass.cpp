#include "vulkan_render_pass.h"

#include <array>

namespace kf {
namespace {
VkAttachmentLoadOp load_op(ColorLoad load) {
    switch (load) {
    case ColorLoad::Keep: return VK_ATTACHMENT_LOAD_OP_LOAD;
    case ColorLoad::Clear: return VK_ATTACHMENT_LOAD_OP_CLEAR;
    case ColorLoad::Discard: return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
    return VK_ATTACHMENT_LOAD_OP_LOAD;
}

VkAttachmentDescription attachment(VkFormat format, VkAttachmentLoadOp load, VkAttachmentStoreOp store,
                                   VkImageLayout layout) {
    VkAttachmentDescription out{};
    out.format = format;
    out.samples = VK_SAMPLE_COUNT_1_BIT;
    out.loadOp = load;
    out.storeOp = store;
    out.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    out.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    out.initialLayout = layout;
    out.finalLayout = layout;
    return out;
}
} // namespace

RenderPass RenderPass::create(VkDevice device, const PassShape &shape) {
    const bool has_depth = shape.depth != VK_FORMAT_UNDEFINED;
    const std::array<VkAttachmentDescription, 2> attachments = {
        attachment(shape.color, load_op(shape.load), VK_ATTACHMENT_STORE_OP_STORE,
                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
        attachment(shape.depth, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                   VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL),
    };
    const VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference depth{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color;
    subpass.pDepthStencilAttachment = has_depth ? &depth : nullptr;
    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = has_depth ? 2 : 1;
    info.pAttachments = attachments.data();
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    RenderPass out;
    if (vkCreateRenderPass(device, &info, nullptr, &out.pass_) != VK_SUCCESS)
        return {};
    out.device_ = device;
    out.shape_ = shape;
    return out;
}

RenderPass &RenderPass::operator=(RenderPass &&other) noexcept {
    if (this != &other) {
        release();
        device_ = std::exchange(other.device_, VK_NULL_HANDLE);
        pass_ = std::exchange(other.pass_, VK_NULL_HANDLE);
        shape_ = other.shape_;
    }
    return *this;
}

void RenderPass::release() {
    if (pass_)
        vkDestroyRenderPass(device_, pass_, nullptr);
    pass_ = VK_NULL_HANDLE;
}

Framebuffer Framebuffer::create(VkDevice device, const RenderPass &pass, std::span<const VkImageView> views,
                                VkExtent2D extent) {
    VkFramebufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    info.renderPass = pass.handle();
    info.attachmentCount = static_cast<uint32_t>(views.size());
    info.pAttachments = views.data();
    info.width = extent.width;
    info.height = extent.height;
    info.layers = 1;
    Framebuffer out;
    if (vkCreateFramebuffer(device, &info, nullptr, &out.framebuffer_) != VK_SUCCESS)
        return {};
    out.device_ = device;
    out.extent_ = extent;
    return out;
}

Framebuffer &Framebuffer::operator=(Framebuffer &&other) noexcept {
    if (this != &other) {
        release();
        device_ = std::exchange(other.device_, VK_NULL_HANDLE);
        framebuffer_ = std::exchange(other.framebuffer_, VK_NULL_HANDLE);
        extent_ = other.extent_;
    }
    return *this;
}

void Framebuffer::release() {
    if (framebuffer_)
        vkDestroyFramebuffer(device_, framebuffer_, nullptr);
    framebuffer_ = VK_NULL_HANDLE;
}

void begin_pass(VkCommandBuffer commands, const RenderPass &pass, const Framebuffer &framebuffer,
                VkClearColorValue clear_color) {
    std::array<VkClearValue, 2> clears{};
    clears[0].color = clear_color;
    clears[1].depthStencil = {1.0f, 0};
    const VkExtent2D extent = framebuffer.extent();
    VkRenderPassBeginInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    info.renderPass = pass.handle();
    info.framebuffer = framebuffer.handle();
    info.renderArea = {{0, 0}, extent};
    info.clearValueCount = pass.shape().depth != VK_FORMAT_UNDEFINED ? 2 : 1;
    info.pClearValues = clears.data();
    vkCmdBeginRenderPass(commands, &info, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport{0, 0, static_cast<float>(extent.width), static_cast<float>(extent.height), 0, 1};
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(commands, 0, 1, &viewport);
    vkCmdSetScissor(commands, 0, 1, &scissor);
}
} // namespace kf
