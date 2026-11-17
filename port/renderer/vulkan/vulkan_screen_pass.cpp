#include "vulkan_screen_pass.h"

#include "vulkan_pipelines.h"

#include <screen_crt_frag.h>
#include <screen_crt_curved_frag.h>
#include <screen_lcd_frag.h>
#include <screen_scanlines_frag.h>
#include <screen_sharp_bilinear_frag.h>

#include <cstdio>
#include <span>

namespace kf {
namespace {
// Each shader's fragment stage (shaders/screen_*.frag), by ScreenShader; None has none.
constexpr std::array<std::span<const uint32_t>, screen_shader_count> fragments = {{
    {},
    screen_scanlines_frag,
    screen_crt_frag,
    screen_crt_curved_frag,
    screen_sharp_bilinear_frag,
    screen_lcd_frag,
}};

// The push constants of shaders/screen.glsl.
struct ScreenConstants {
    std::array<float, 4> rect;   // x, y, width, height in window pixels
    std::array<float, 2> grid;   // the game's columns and lines
};
static_assert(sizeof(ScreenConstants) == 24);

VkDescriptorSetLayout create_set_layout(VkDevice device) {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = 1;
    info.pBindings = &binding;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(device, &info, nullptr, &layout) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return layout;
}

VkDescriptorPool create_pool(VkDevice device) {
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, ScreenPass::frames_in_flight};
    VkDescriptorPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.maxSets = ScreenPass::frames_in_flight;
    info.poolSizeCount = 1;
    info.pPoolSizes = &size;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(device, &info, nullptr, &pool) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return pool;
}

VkPipelineLayout create_layout(VkDevice device, VkDescriptorSetLayout set_layout) {
    const VkPushConstantRange constants{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ScreenConstants)};
    VkPipelineLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = 1;
    info.pSetLayouts = &set_layout;
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &constants;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(device, &info, nullptr, &layout) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return layout;
}

void point_at(VkDevice device, VkDescriptorSet set, VkSampler sampler, const GpuImage &picture) {
    const VkDescriptorImageInfo image{sampler, picture.view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}
} // namespace

std::unique_ptr<ScreenPass> ScreenPass::create(const VulkanDevice &device) {
    VkDevice handle = device.device();
    auto out = std::make_unique<ScreenPass>(Key{}, device);
    // The shaders read whole texels (texelFetch), so any format of picture works.
    out->sampler_ = create_sampler(handle, VK_FILTER_NEAREST);
    out->set_layout_ = create_set_layout(handle);
    out->pool_ = create_pool(handle);
    if (!out->sampler_ || !out->set_layout_ || !out->pool_)
        return nullptr;
    const std::array<VkDescriptorSetLayout, frames_in_flight> layouts = {out->set_layout_, out->set_layout_};
    VkDescriptorSetAllocateInfo sets{};
    sets.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    sets.descriptorPool = out->pool_;
    sets.descriptorSetCount = frames_in_flight;
    sets.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(handle, &sets, out->sets_.data()) != VK_SUCCESS)
        return nullptr;
    out->layout_ = create_layout(handle, out->set_layout_);
    if (!out->layout_)
        return nullptr;
    return out;
}

ScreenPass::~ScreenPass() {
    VkDevice device = device_.device();
    release_pipelines();
    if (layout_)
        vkDestroyPipelineLayout(device, layout_, nullptr);
    if (pool_)
        vkDestroyDescriptorPool(device, pool_, nullptr);
    if (set_layout_)
        vkDestroyDescriptorSetLayout(device, set_layout_, nullptr);
    if (sampler_)
        vkDestroySampler(device, sampler_, nullptr);
}

void ScreenPass::release_pipelines() {
    for (VkPipeline &each : pipelines_) {
        if (each)
            vkDestroyPipeline(device_.device(), each, nullptr);
        each = VK_NULL_HANDLE;
    }
}

// The pass and pipelines are made for the window's format; a new format (a
// rebuilt surface) remakes them once the GPU is done with the old ones.
bool ScreenPass::fit_format(VkFormat window_format) {
    if (pass_ && pass_.shape().color == window_format)
        return true;
    if (pass_ && vkDeviceWaitIdle(device_.device()) != VK_SUCCESS)
        return false;
    release_pipelines();
    pass_ = RenderPass::create(device_.device(), {window_format, VK_FORMAT_UNDEFINED, ColorLoad::Clear});
    return static_cast<bool>(pass_);
}

VkPipeline ScreenPass::pipeline(ScreenShader shader) {
    const auto index = static_cast<std::size_t>(shader);
    if (index >= pipelines_.size() || fragments[index].empty())
        return VK_NULL_HANDLE;
    if (!pipelines_[index])
        pipelines_[index] = create_fullscreen_pipeline(device_, layout_, fragments[index], pass_);
    return pipelines_[index];
}

bool ScreenPass::record(VkCommandBuffer commands, uint32_t frame, const ScreenDraw &draw) {
    if (frame >= frames_in_flight || !draw.picture || !draw.framebuffer || !fit_format(draw.window_format))
        return false;
    VkPipeline chosen = pipeline(draw.shader);
    if (!chosen) {
        std::fprintf(stderr, "[vulkan] cannot make screen shader %d\n", static_cast<int>(draw.shader));
        return false;
    }
    point_at(device_.device(), sets_[frame], sampler_, *draw.picture);
    transition(commands, draw.window_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    begin_pass(commands, pass_, *draw.framebuffer);
    const PresentRect &rect = draw.rect;
    if (rect.width > 0 && rect.height > 0) {
        const VkViewport viewport{static_cast<float>(rect.x), static_cast<float>(rect.y),
                                  static_cast<float>(rect.width), static_cast<float>(rect.height), 0.0f, 1.0f};
        const VkRect2D scissor{{rect.x, rect.y},
                               {static_cast<uint32_t>(rect.width), static_cast<uint32_t>(rect.height)}};
        vkCmdSetViewport(commands, 0, 1, &viewport);
        vkCmdSetScissor(commands, 0, 1, &scissor);
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, chosen);
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &sets_[frame], 0, nullptr);
        const ScreenConstants constants{
            {viewport.x, viewport.y, viewport.width, viewport.height},
            {static_cast<float>(draw.game_columns), static_cast<float>(render_height)}};
        vkCmdPushConstants(commands, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof constants, &constants);
        vkCmdDraw(commands, 3, 1, 0, 0);
    }
    vkCmdEndRenderPass(commands);
    // The overlay draws next, from where a blit leaves the image.
    transition(commands, draw.window_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    return true;
}
} // namespace kf
