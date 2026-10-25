#include "vulkan_ambient_occlusion.h"

#include <occlusion_blur_frag.h>
#include <occlusion_frag.h>

#include <algorithm>
#include <array>

namespace kf {
namespace {
// Light left, camera depth: depth needs the full float to be compared with a fragment's.
constexpr VkFormat occlusion_format = VK_FORMAT_R32G32_SFLOAT;
// How dark full occlusion goes at strength 1 (GteDepth.AoStrength).
constexpr float judged_strength = 0.8f;

// The push constants of shaders/occlusion.frag.
struct OcclusionConstants {
    std::array<float, 2> view_columns;
    std::array<float, 2> center;
    std::array<float, 2> size;
    float projection_distance;
    float strength;
    uint32_t samples;
};
static_assert(sizeof(OcclusionConstants) == 36);

// Occlusion pixels per game pixel on a side, never finer than the target.
int pixels_per_game_pixel(OcclusionQuality quality, int scale) {
    switch (quality) {
    case OcclusionQuality::Low: return 1;
    case OcclusionQuality::Medium: return std::min(2, scale);
    case OcclusionQuality::High: return scale;
    }
    return 1;
}

uint32_t samples_for(OcclusionQuality quality) { return quality == OcclusionQuality::Low ? 8 : 16; }

VkDescriptorSetLayout create_image_set_layout(VkDevice device) {
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
} // namespace

std::unique_ptr<AmbientOcclusion> AmbientOcclusion::create(const VulkanDevice &device) {
    VkDevice handle = device.device();
    auto out = std::make_unique<AmbientOcclusion>(Key{}, handle);
    out->owner_ = &device;
    out->allocator_ = device.allocator();
    out->sampler_ = create_sampler(handle, VK_FILTER_NEAREST);
    out->set_layout_ = create_image_set_layout(handle);
    if (!out->sampler_ || !out->set_layout_)
        return nullptr;

    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3};
    VkDescriptorPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = 3;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &size;
    if (vkCreateDescriptorPool(handle, &pool, nullptr, &out->pool_) != VK_SUCCESS)
        return nullptr;
    const std::array<VkDescriptorSetLayout, 3> layouts = {out->set_layout_, out->set_layout_, out->set_layout_};
    std::array<VkDescriptorSet, 3> sets{};
    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = out->pool_;
    allocate.descriptorSetCount = static_cast<uint32_t>(sets.size());
    allocate.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(handle, &allocate, sets.data()) != VK_SUCCESS)
        return nullptr;
    out->world_set_ = sets[0];
    out->occlusion_inputs_ = sets[1];
    out->blur_inputs_ = sets[2];

    const VkPushConstantRange constants{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(OcclusionConstants)};
    VkPipelineLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &out->set_layout_;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &constants;
    if (vkCreatePipelineLayout(handle, &layout, nullptr, &out->layout_) != VK_SUCCESS)
        return nullptr;
    out->pass_ = RenderPass::create(handle, {occlusion_format, VK_FORMAT_UNDEFINED, ColorLoad::Discard});
    if (!out->pass_)
        return nullptr;
    out->occlusion_pipeline_ = create_fullscreen_pipeline(device, out->layout_, occlusion_frag, out->pass_);
    out->blur_pipeline_ = create_fullscreen_pipeline(device, out->layout_, occlusion_blur_frag, out->pass_);
    if (!out->occlusion_pipeline_ || !out->blur_pipeline_)
        return nullptr;

    out->unoccluded_ = out->make_unoccluded_image(1, 1);
    if (!out->unoccluded_)
        return nullptr;
    out->point_set(out->world_set_, out->unoccluded_.view());
    return out;
}

AmbientOcclusion::~AmbientOcclusion() {
    if (occlusion_pipeline_)
        vkDestroyPipeline(device_, occlusion_pipeline_, nullptr);
    if (blur_pipeline_)
        vkDestroyPipeline(device_, blur_pipeline_, nullptr);
    if (layout_)
        vkDestroyPipelineLayout(device_, layout_, nullptr);
    if (pool_)
        vkDestroyDescriptorPool(device_, pool_, nullptr);
    if (set_layout_)
        vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
    if (sampler_)
        vkDestroySampler(device_, sampler_, nullptr);
}

// An occlusion image that says "nothing occluded, no surface", in
// SHADER_READ_ONLY_OPTIMAL. Null on failure.
GpuImage AmbientOcclusion::make_unoccluded_image(uint32_t width, uint32_t height) const {
    const ImageShape shape{width, height, 1, occlusion_format,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                               VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT};
    GpuImage image = GpuImage::create(device_, allocator_, shape);
    if (!image)
        return {};
    const bool cleared = owner_->run_now([&](VkCommandBuffer commands) {
        transition(commands, image.image(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue unoccluded{};
        unoccluded.float32[0] = 1.0f;
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(commands, image.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &unoccluded, 1, &range);
        transition(commands, image.image(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });
    if (!cleared)
        return {};
    return image;
}

void AmbientOcclusion::point_set(VkDescriptorSet set, VkImageView view) const {
    const VkDescriptorImageInfo image{sampler_, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
}

bool AmbientOcclusion::fit(const GpuImage &surfaces, int logical_width, int scale, OcclusionQuality quality) {
    if (!surfaces || logical_width < 1 || scale < 1)
        return false;
    if (blurred_ && surfaces.view() == surfaces_view_ && logical_width == logical_width_ && scale == scale_ &&
        quality == quality_)
        return true;
    const int per_pixel = pixels_per_game_pixel(quality, scale);
    const auto width = static_cast<uint32_t>(logical_width * per_pixel);
    const auto height = static_cast<uint32_t>(render_height * per_pixel);
    if (!occlusion_ || occlusion_.shape().width != width || occlusion_.shape().height != height) {
        const ImageShape shape{width, height, 1, occlusion_format,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT};
        GpuImage occlusion = GpuImage::create(device_, allocator_, shape);
        GpuImage blurred = make_unoccluded_image(width, height);
        if (!occlusion || !blurred)
            return false;
        VkImageView occlusion_view = occlusion.view(), blurred_view = blurred.view();
        Framebuffer occlusion_framebuffer =
            Framebuffer::create(device_, pass_, std::span(&occlusion_view, 1), {width, height});
        Framebuffer blurred_framebuffer = Framebuffer::create(device_, pass_, std::span(&blurred_view, 1), {width, height});
        if (!occlusion_framebuffer || !blurred_framebuffer)
            return false;
        occlusion_framebuffer_ = std::move(occlusion_framebuffer);
        blurred_framebuffer_ = std::move(blurred_framebuffer);
        occlusion_ = std::move(occlusion);
        blurred_ = std::move(blurred);
    }
    // Written every time: a remade image can come back with the old view's handle.
    point_set(occlusion_inputs_, surfaces.view());
    point_set(blur_inputs_, occlusion_.view());
    point_set(world_set_, blurred_.view());
    surfaces_view_ = surfaces.view();
    logical_width_ = logical_width;
    scale_ = scale;
    quality_ = quality;
    return true;
}

void AmbientOcclusion::draw_pass(VkCommandBuffer commands, VkPipeline pipeline, VkDescriptorSet inputs,
                                 const GpuImage &output, const Framebuffer &framebuffer, VkImageLayout output_layout,
                                 std::span<const std::byte> constants) const {
    transition(commands, output.image(), VK_IMAGE_ASPECT_COLOR_BIT, output_layout,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    begin_pass(commands, pass_, framebuffer);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &inputs, 0, nullptr);
    if (!constants.empty())
        vkCmdPushConstants(commands, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, static_cast<uint32_t>(constants.size()),
                           constants.data());
    vkCmdDraw(commands, 3, 1, 0, 0);
    vkCmdEndRenderPass(commands);
    transition(commands, output.image(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void AmbientOcclusion::record(VkCommandBuffer commands, const DrawConstants &constants, float strength) {
    if (!blurred_)
        return;
    const ImageShape &shape = occlusion_.shape();
    OcclusionConstants occlusion{};
    occlusion.view_columns = constants.view_columns;
    occlusion.center = constants.center;
    occlusion.size = {static_cast<float>(shape.width), static_cast<float>(shape.height)};
    occlusion.projection_distance = constants.projection_distance;
    occlusion.strength = judged_strength * std::clamp(strength, 0.0f, 2.0f);
    occlusion.samples = samples_for(quality_);
    draw_pass(commands, occlusion_pipeline_, occlusion_inputs_, occlusion_, occlusion_framebuffer_,
              VK_IMAGE_LAYOUT_UNDEFINED, std::as_bytes(std::span(&occlusion, 1)));
    draw_pass(commands, blur_pipeline_, blur_inputs_, blurred_, blurred_framebuffer_,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, {});
}
} // namespace kf
