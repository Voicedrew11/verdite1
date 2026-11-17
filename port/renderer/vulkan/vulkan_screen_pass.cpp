#include "vulkan_screen_pass.h"

#include "vulkan_pipelines.h"

#include <screen_crt_frag.h>
#include <screen_crt_curved_frag.h>
#include <screen_hyllian_frag.h>
#include <screen_hyllian_lines_frag.h>
#include <screen_lcd_frag.h>
#include <screen_scanlines_frag.h>
#include <screen_sharp_bilinear_frag.h>

#include <cstdio>
#include <span>

namespace kf {
namespace {
// A shader's fragment stages (shaders/screen_*.frag): the one that draws into
// the window, and for a two-pass shader the one that filters along the lines first.
struct ShaderStages {
    std::span<const uint32_t> window;
    std::span<const uint32_t> along_lines;
};
// By ScreenShader; None has none.
constexpr std::array<ShaderStages, screen_shader_count> stages = {{
    {},
    {screen_scanlines_frag, {}},
    {screen_crt_frag, {}},
    {screen_crt_curved_frag, {}},
    {screen_sharp_bilinear_frag, {}},
    {screen_lcd_frag, {}},
    {screen_hyllian_frag, screen_hyllian_lines_frag},
}};

// The lines are kept linear, with more precision than the window has.
constexpr VkFormat lines_format = VK_FORMAT_R16G16B16A16_SFLOAT;

// Hyllian's PHOSPHOR_LAYOUT for each CrtMask.
constexpr std::array<float, crt_mask_count> hyllian_layouts = {0, 2, 4, 11, 14, 7, 10, 15};

// The push constants of shaders/screen.glsl.
struct ScreenConstants {
    std::array<float, 4> rect;   // x, y, width, height in pixels of the pass's target
    std::array<float, 2> grid;   // the game's columns and lines
    float mask_layout;           // the rest: Hyllian's
    float curvature;
    float scanlines;
    float brightness;
};
static_assert(sizeof(ScreenConstants) == 40);

ScreenConstants constants_for(const ScreenDraw &draw, const PresentRect &rect) {
    const HyllianOptions &h = draw.hyllian;
    const auto mask = static_cast<std::size_t>(h.mask);
    return {{static_cast<float>(rect.x), static_cast<float>(rect.y), static_cast<float>(rect.width),
             static_cast<float>(rect.height)},
            {static_cast<float>(draw.game_columns), static_cast<float>(render_height)},
            mask < hyllian_layouts.size() ? hyllian_layouts[mask] : 0.0f,
            h.curvature ? 1.0f : 0.0f,
            h.scanlines,
            h.brightness};
}

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

// Two sets per frame in flight: the picture, and the lines.
constexpr uint32_t set_count = 2 * ScreenPass::frames_in_flight;

VkDescriptorPool create_pool(VkDevice device) {
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, set_count};
    VkDescriptorPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.maxSets = set_count;
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

void point_at(VkDevice device, VkDescriptorSet set, VkSampler sampler, const GpuImage &image) {
    const VkDescriptorImageInfo info{sampler, image.view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &info;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}

// One full-screen triangle into rect of the pass begun, from set.
void draw_into(VkCommandBuffer commands, VkPipeline pipeline, VkPipelineLayout layout, VkDescriptorSet set,
               const ScreenConstants &constants) {
    const VkViewport viewport{constants.rect[0], constants.rect[1], constants.rect[2], constants.rect[3], 0.0f, 1.0f};
    const VkRect2D scissor{{static_cast<int32_t>(constants.rect[0]), static_cast<int32_t>(constants.rect[1])},
                           {static_cast<uint32_t>(constants.rect[2]), static_cast<uint32_t>(constants.rect[3])}};
    vkCmdSetViewport(commands, 0, 1, &viewport);
    vkCmdSetScissor(commands, 0, 1, &scissor);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(commands, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof constants, &constants);
    vkCmdDraw(commands, 3, 1, 0, 0);
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
    std::array<VkDescriptorSetLayout, set_count> layouts{};
    layouts.fill(out->set_layout_);
    std::array<VkDescriptorSet, set_count> sets{};
    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = out->pool_;
    allocate.descriptorSetCount = set_count;
    allocate.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(handle, &allocate, sets.data()) != VK_SUCCESS)
        return nullptr;
    for (uint32_t i = 0; i < frames_in_flight; ++i) {
        out->sets_[i] = sets[i];
        out->lines_sets_[i] = sets[frames_in_flight + i];
    }
    out->layout_ = create_layout(handle, out->set_layout_);
    out->lines_pass_ = RenderPass::create(handle, {lines_format, VK_FORMAT_UNDEFINED, ColorLoad::Discard});
    if (!out->layout_ || !out->lines_pass_)
        return nullptr;
    return out;
}

ScreenPass::~ScreenPass() {
    VkDevice device = device_.device();
    release_pipelines();
    for (VkPipeline each : lines_pipelines_) {
        if (each)
            vkDestroyPipeline(device, each, nullptr);
    }
    if (layout_)
        vkDestroyPipelineLayout(device, layout_, nullptr);
    if (pool_)
        vkDestroyDescriptorPool(device, pool_, nullptr);
    if (set_layout_)
        vkDestroyDescriptorSetLayout(device, set_layout_, nullptr);
    if (sampler_)
        vkDestroySampler(device, sampler_, nullptr);
}

// The pipelines into the window (the lines' pass never changes format).
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

// The lines image for a letterbox this wide; remade, once the GPU is done
// with the old one, when the letterbox changes width.
bool ScreenPass::fit_lines(uint32_t width_px, uint32_t lines) {
    if (lines_ && lines_.shape().width == width_px && lines_.shape().height == lines)
        return true;
    if (lines_ && vkDeviceWaitIdle(device_.device()) != VK_SUCCESS)
        return false;
    const ImageShape shape{width_px, lines, 1, lines_format,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT};
    GpuImage image = GpuImage::create(device_.device(), device_.allocator(), shape);
    if (!image)
        return false;
    VkImageView view = image.view();
    Framebuffer framebuffer = Framebuffer::create(device_.device(), lines_pass_, std::span(&view, 1), {width_px, lines});
    if (!framebuffer)
        return false;
    lines_framebuffer_ = std::move(framebuffer);
    lines_ = std::move(image);
    return true;
}

VkPipeline ScreenPass::pipeline(ScreenShader shader) {
    const auto index = static_cast<std::size_t>(shader);
    if (index >= pipelines_.size() || stages[index].window.empty())
        return VK_NULL_HANDLE;
    if (!pipelines_[index])
        pipelines_[index] = create_fullscreen_pipeline(device_, layout_, stages[index].window, pass_);
    return pipelines_[index];
}

VkPipeline ScreenPass::lines_pipeline(ScreenShader shader) {
    const auto index = static_cast<std::size_t>(shader);
    if (index >= lines_pipelines_.size() || stages[index].along_lines.empty())
        return VK_NULL_HANDLE;
    if (!lines_pipelines_[index])
        lines_pipelines_[index] = create_fullscreen_pipeline(device_, layout_, stages[index].along_lines, lines_pass_);
    return lines_pipelines_[index];
}

// A two-pass shader's first pass: the picture filtered along each game line
// into lines_, left in SHADER_READ_ONLY_OPTIMAL for the second.
bool ScreenPass::record_lines(VkCommandBuffer commands, uint32_t frame, const ScreenDraw &draw) {
    VkPipeline chosen = lines_pipeline(draw.shader);
    if (!chosen || !fit_lines(static_cast<uint32_t>(draw.rect.width), static_cast<uint32_t>(render_height)))
        return false;
    point_at(device_.device(), sets_[frame], sampler_, *draw.picture);
    point_at(device_.device(), lines_sets_[frame], sampler_, lines_);
    transition(commands, lines_.image(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    begin_pass(commands, lines_pass_, lines_framebuffer_);
    draw_into(commands, chosen, layout_, sets_[frame], constants_for(draw, {0, 0, draw.rect.width, render_height}));
    vkCmdEndRenderPass(commands);
    transition(commands, lines_.image(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

bool ScreenPass::record(VkCommandBuffer commands, uint32_t frame, const ScreenDraw &draw) {
    if (frame >= frames_in_flight || !draw.picture || !draw.framebuffer || !fit_format(draw.window_format))
        return false;
    VkPipeline chosen = pipeline(draw.shader);
    if (!chosen) {
        std::fprintf(stderr, "[vulkan] cannot make screen shader %d\n", static_cast<int>(draw.shader));
        return false;
    }
    const bool drawn = draw.rect.width > 0 && draw.rect.height > 0;
    const bool two_pass = !stages[static_cast<std::size_t>(draw.shader)].along_lines.empty();
    if (drawn && two_pass && !record_lines(commands, frame, draw)) {
        std::fprintf(stderr, "[vulkan] cannot draw screen shader %d's first pass\n", static_cast<int>(draw.shader));
        return false;
    }
    if (drawn && !two_pass)
        point_at(device_.device(), sets_[frame], sampler_, *draw.picture);
    transition(commands, draw.window_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    begin_pass(commands, pass_, *draw.framebuffer);
    if (drawn)
        draw_into(commands, chosen, layout_, two_pass ? lines_sets_[frame] : sets_[frame], constants_for(draw, draw.rect));
    vkCmdEndRenderPass(commands);
    // The overlay draws next, from where a blit leaves the image.
    transition(commands, draw.window_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    return true;
}
} // namespace kf
