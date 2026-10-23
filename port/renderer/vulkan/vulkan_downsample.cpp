#include "vulkan_downsample.h"

#include "vulkan_pipelines.h"

#include <downsample_frag.h>

namespace kf {
namespace {
// The averages are new colours, so the picture has 8 bits a channel even when
// the target has 5.
constexpr VkFormat picture_format = VK_FORMAT_R8G8B8A8_UNORM;

// The push constants of shaders/downsample.frag.
struct DownsampleConstants {
    uint32_t factor;
};
static_assert(sizeof(DownsampleConstants) == 4);
} // namespace

std::unique_ptr<Downsample> Downsample::create(const VulkanDevice &device) {
    VkDevice handle = device.device();
    auto out = std::make_unique<Downsample>(Key{}, handle);
    out->allocator_ = device.allocator();
    out->sampler_ = create_sampler(handle, VK_FILTER_NEAREST);
    if (!out->sampler_)
        return nullptr;

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo set_layout{};
    set_layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout.bindingCount = 1;
    set_layout.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(handle, &set_layout, nullptr, &out->set_layout_) != VK_SUCCESS)
        return nullptr;
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = 1;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &size;
    if (vkCreateDescriptorPool(handle, &pool, nullptr, &out->pool_) != VK_SUCCESS)
        return nullptr;
    VkDescriptorSetAllocateInfo set{};
    set.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    set.descriptorPool = out->pool_;
    set.descriptorSetCount = 1;
    set.pSetLayouts = &out->set_layout_;
    if (vkAllocateDescriptorSets(handle, &set, &out->set_) != VK_SUCCESS)
        return nullptr;

    const VkPushConstantRange constants{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(DownsampleConstants)};
    VkPipelineLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &out->set_layout_;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &constants;
    if (vkCreatePipelineLayout(handle, &layout, nullptr, &out->layout_) != VK_SUCCESS)
        return nullptr;
    out->pipeline_ = create_fullscreen_pipeline(handle, out->layout_, downsample_frag, picture_format);
    if (!out->pipeline_)
        return nullptr;
    return out;
}

Downsample::~Downsample() {
    if (pipeline_)
        vkDestroyPipeline(device_, pipeline_, nullptr);
    if (layout_)
        vkDestroyPipelineLayout(device_, layout_, nullptr);
    if (pool_)
        vkDestroyDescriptorPool(device_, pool_, nullptr);
    if (set_layout_)
        vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
    if (sampler_)
        vkDestroySampler(device_, sampler_, nullptr);
}

bool Downsample::fit(const GpuImage &target, uint32_t factor) {
    const ImageShape &source = target.shape();
    if (factor < 2 || source.width % factor != 0 || source.height % factor != 0)
        return false;
    const uint32_t width = source.width / factor, height = source.height / factor;
    if (!picture_ || picture_.shape().width != width || picture_.shape().height != height) {
        const ImageShape shape{width, height, 1, picture_format,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT};
        picture_ = GpuImage::create(device_, allocator_, shape);
        if (!picture_)
            return false;
    }
    // Written every time: a remade target can come back with the old view's handle.
    const VkDescriptorImageInfo image{sampler_, target.view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set_;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    factor_ = factor;
    return true;
}

void Downsample::record(VkCommandBuffer commands) {
    const ImageShape &shape = picture_.shape();
    transition(commands, picture_.image(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfoKHR color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
    color.imageView = picture_.view();
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfoKHR rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR;
    rendering.renderArea = {{0, 0}, {shape.width, shape.height}};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    vkCmdBeginRenderingKHR(commands, &rendering);
    const VkViewport viewport{0, 0, static_cast<float>(shape.width), static_cast<float>(shape.height), 0, 1};
    const VkRect2D scissor{{0, 0}, {shape.width, shape.height}};
    vkCmdSetViewport(commands, 0, 1, &viewport);
    vkCmdSetScissor(commands, 0, 1, &scissor);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set_, 0, nullptr);
    const DownsampleConstants constants{factor_};
    vkCmdPushConstants(commands, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof constants, &constants);
    vkCmdDraw(commands, 3, 1, 0, 0);
    vkCmdEndRenderingKHR(commands);
    transition(commands, picture_.image(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
}
} // namespace kf
