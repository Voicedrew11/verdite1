// The Vulkan backend: draws an update's faces into a target of the logical
// size times the scale, then scales that into the window's swapchain image.
// Without a window it draws the target only (tests, headless captures).
#include <kf/renderer/backend.h>

#include "vulkan_ambient_occlusion.h"
#include "vulkan_device.h"
#include "vulkan_downsample.h"
#include "vulkan_pipelines.h"
#include "vulkan_resources.h"
#include "vulkan_surfaces.h"
#include "vulkan_swapchain.h"
#include "../texture_mips.h"

#include <SDL3/SDL_video.h>
#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

namespace kf {
namespace {
constexpr int frames_in_flight = 2;
// 256 x 256 down to 1 x 1.
constexpr uint32_t page_mip_levels = 9;
constexpr VkDeviceSize motion_bytes = sizeof(Motion);
// The target is cleared and drawn at the original 15-bit precision when it can be.
constexpr VkFormat classic_format = VK_FORMAT_A1R5G5B5_UNORM_PACK16;
constexpr VkFormat true_color_format = VK_FORMAT_R8G8B8A8_UNORM;

// The fixed equations each blend mode is drawn with, in order.
struct BlendPass {
    BlendEquation equation;
    uint32_t flags;
};
std::vector<BlendPass> blend_passes(BlendMode mode) {
    switch (mode) {
    case BlendMode::opaque: return {{BlendEquation::None, 0}};
    case BlendMode::average: return {{BlendEquation::Average, 0}};
    case BlendMode::add:
    case BlendMode::add_quarter: return {{BlendEquation::AddWeighted, 0}};
    case BlendMode::subtract:
        return {{BlendEquation::AddWeighted, 0}, {BlendEquation::ReverseSubtract, draw_subtract_stp_pass}};
    }
    return {};
}

// The PS1 cleared in 15-bit colour.
float clear_channel(float value) {
    const auto channel = static_cast<uint32_t>(std::clamp(std::lround(value * color8_max), 0L, 255L)) >> color5_color8_shift;
    return static_cast<float>(channel) / color5_max;
}

u8 expand5(uint32_t channel) {
    return static_cast<u8>((channel << color5_color8_shift) | (channel >> color5_replication_shift));
}

struct FrameSlot {
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkFence done = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkDescriptorSet descriptors = VK_NULL_HANDLE;
    GpuBuffer motions;
};

class VulkanBackend final : public RenderBackend {
public:
    VulkanBackend(std::unique_ptr<VulkanDevice> device, SDL_Window *window);
    ~VulkanBackend() override;
    VulkanBackend(const VulkanBackend &) = delete;
    VulkanBackend &operator=(const VulkanBackend &) = delete;
    [[nodiscard]] bool start(std::string &error);

    Tier tier() const override { return Tier::Balanced; }
    const char *name() const override { return "Vulkan"; }
    PictureOptions drawable(const PictureOptions &wanted) const override {
        PictureOptions out = within_tier(wanted, Tier::Balanced);
        out.ambient_occlusion = wanted.ambient_occlusion;
        out.occlusion_quality = wanted.occlusion_quality;
        out.fog = wanted.fog;
        out.anisotropy = std::clamp(wanted.anisotropy, 1, static_cast<int>(device_->max_anisotropy()));
        out.enhanced_depth = wanted.enhanced_depth;
        return out;
    }
    bool set_target(int logical_width, int scale) override;
    void set_options(const PictureOptions &options) override;
    void upload_layer(u8 layer, const Image &texels) override;
    void set_occluders(std::span<const u8>, int, int, float) override {}
    void upload_frame(std::span<const GpuVertex> vertices, std::span<const DrawRun> runs, const FrameStyle &style,
                      const DepthCue &cue) override;
    void present(const FrameView &view, std::span<const RenderLight> lights, int window_width, int window_height,
                 ImDrawData *overlay) override;
    std::optional<Image> capture() override;
    bool restore(const Image &picture) override;
    bool start_overlay() override;
    void stop_overlay() override;

private:
    VkFormat wanted_color_format() const;
    bool make_target();
    bool make_pages();
    bool make_smooth_sampler();
    void bind_pages(const FrameSlot &slot);
    bool make_frame_slots();
    void fit_downsample();
    void fit_occlusion();
    bool flush_layers();
    bool wait_for_frames();
    bool write_motions(FrameSlot &slot, std::span<const Motion> motions);
    VkRect2D clip_rect() const;
    DrawConstants draw_constants(const FrameView &view) const;
    void measure_occlusion(VkCommandBuffer commands, const DrawConstants &constants, const FrameSlot &slot);
    void draw_world(VkCommandBuffer commands, const FrameView &view, const FrameSlot &slot);
    void draw_runs(VkCommandBuffer commands, DrawConstants constants, const FrameSlot &slot);
    const GpuImage &shown_picture(VkCommandBuffer commands);
    void blit_to(VkCommandBuffer commands, const GpuImage &source, VkImage image, VkExtent2D extent);
    void draw_overlay(VkCommandBuffer commands, const SwapchainImage &image, ImDrawData *overlay);

    std::unique_ptr<VulkanDevice> device_;
    SDL_Window *window_;
    std::unique_ptr<VulkanSwapchain> swapchain_;
    PictureOptions options_;
    int logical_width_ = render_width, scale_ = 1;
    VkFormat depth_format_ = VK_FORMAT_D32_SFLOAT;
    std::unique_ptr<WorldPipelines> pipelines_;
    std::unique_ptr<Downsample> downsample_;   // window only
    bool downsampled_ = false;                 // the window shows downsample_'s picture
    std::unique_ptr<SurfaceBuffer> surfaces_;
    std::unique_ptr<AmbientOcclusion> occlusion_;
    bool occlusion_ready_ = false;             // surfaces_ and occlusion_ fit the target
    GpuImage target_, depth_;
    VkImageLayout target_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    GpuImage pages_;
    VkSampler nearest_ = VK_NULL_HANDLE;
    VkSampler smooth_ = VK_NULL_HANDLE;   // trilinear, for mipmapped bilinear filtering
    struct PendingLayer {
        u8 layer;
        std::vector<Image> levels;   // texture_mip_chain
    };
    std::vector<PendingLayer> pending_layers_;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    std::array<FrameSlot, frames_in_flight> slots_;
    std::uint64_t frame_number_ = 0;
    GpuBuffer vertices_;
    std::vector<DrawRun> runs_;
    FrameStyle style_;
    DepthCue cue_;
    bool frame_drawn_ = false;
    bool overlay_started_ = false;
    VkFormat overlay_format_ = VK_FORMAT_UNDEFINED;
};

bool format_supports(VkPhysicalDevice physical, VkFormat format, VkFormatFeatureFlags features) {
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(physical, format, &properties);
    return (properties.optimalTilingFeatures & features) == features;
}

VulkanBackend::VulkanBackend(std::unique_ptr<VulkanDevice> device, SDL_Window *window)
    : device_(std::move(device)), window_(window) {}

VulkanBackend::~VulkanBackend() {
    stop_overlay();
    VkDevice device = device_->device();
    vkDeviceWaitIdle(device);
    for (FrameSlot &slot : slots_) {
        if (slot.done)
            vkDestroyFence(device, slot.done, nullptr);
        if (slot.acquired)
            vkDestroySemaphore(device, slot.acquired, nullptr);
    }
    pipelines_.reset();
    occlusion_.reset();
    surfaces_.reset();
    if (descriptor_pool_)
        vkDestroyDescriptorPool(device, descriptor_pool_, nullptr);
    if (set_layout_)
        vkDestroyDescriptorSetLayout(device, set_layout_, nullptr);
    if (nearest_)
        vkDestroySampler(device, nearest_, nullptr);
    if (smooth_)
        vkDestroySampler(device, smooth_, nullptr);
}

bool VulkanBackend::start(std::string &error) {
    const VkFormatFeatureFlags depth_features = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (!format_supports(device_->physical(), depth_format_, depth_features))
        depth_format_ = VK_FORMAT_D16_UNORM;
    if (window_) {
        swapchain_ = std::make_unique<VulkanSwapchain>(*device_);
        downsample_ = Downsample::create(*device_);
        if (!downsample_) {
            error = "supersampling pipeline";
            return false;
        }
    }
    if (!make_pages() || !make_frame_slots()) {
        error = "texture or frame resources";
        return false;
    }
    surfaces_ = SurfaceBuffer::create(*device_, set_layout_, depth_format_);
    occlusion_ = AmbientOcclusion::create(*device_);
    if (!surfaces_ || !occlusion_) {
        error = "ambient occlusion pipelines";
        return false;
    }
    if (!make_target()) {
        error = "render target";
        return false;
    }
    std::printf("[vulkan] %s, target %s\n", device_->name().c_str(),
                wanted_color_format() == classic_format ? "15-bit" : "24-bit");
    return true;
}

VkFormat VulkanBackend::wanted_color_format() const {
    const VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                                        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if (!options_.true_color && format_supports(device_->physical(), classic_format, needed))
        return classic_format;
    return true_color_format;
}

// The target and depth buffer for the current size and colour precision,
// cleared to black, and the pipelines for their formats.
bool VulkanBackend::make_target() {
    if (!wait_for_frames())
        return false;
    const VkFormat format = wanted_color_format();
    ImageShape color{static_cast<uint32_t>(logical_width_ * scale_), static_cast<uint32_t>(render_height * scale_), 1,
                     format,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT};
    ImageShape depth = color;
    depth.format = depth_format_;
    depth.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    depth.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    GpuImage new_target = GpuImage::create(device_->device(), device_->allocator(), color);
    GpuImage new_depth = GpuImage::create(device_->device(), device_->allocator(), depth);
    if (!new_target || !new_depth)
        return false;
    if (!pipelines_ || pipelines_->color_format() != format) {
        const std::array<VkDescriptorSetLayout, 2> sets = {set_layout_, occlusion_->set_layout()};
        auto made = WorldPipelines::create(device_->device(), sets, format, depth_format_);
        if (!made)
            return false;
        pipelines_ = std::move(made);
    }
    const bool cleared = device_->run_now([&](VkCommandBuffer commands) {
        transition(commands, new_target.image(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkClearColorValue black{};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(commands, new_target.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        transition(commands, new_target.image(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    });
    if (!cleared)
        return false;
    target_ = std::move(new_target);
    depth_ = std::move(new_depth);
    target_layout_ = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    frame_drawn_ = false;
    fit_downsample();
    fit_occlusion();
    return true;
}

// Supersampling shows the target averaged down by its factor, when the target
// is a whole multiple of it; otherwise the window gets the target as it is.
// The frames in flight are done with the old picture.
void VulkanBackend::fit_downsample() {
    const auto factor = static_cast<uint32_t>(std::max(options_.supersample, 1));
    downsampled_ = downsample_ && target_ && factor > 1 && downsample_->fit(target_, factor);
}

// Occlusion is measured once per shown pixel: the target's, or the averaged
// picture's when supersampling. The frames in flight are done with the old buffers.
void VulkanBackend::fit_occlusion() {
    occlusion_ready_ = false;
    if (options_.ambient_occlusion <= 0.0f || !target_)
        return;
    const uint32_t factor = downsampled_ ? static_cast<uint32_t>(options_.supersample) : 1;
    const ImageShape &shape = target_.shape();
    occlusion_ready_ = surfaces_->fit(shape.width / factor, shape.height / factor) &&
                       occlusion_->fit(surfaces_->surfaces(), logical_width_, scale_, options_.occlusion_quality);
    if (!occlusion_ready_)
        std::fprintf(stderr, "[vulkan] cannot make the ambient occlusion buffers\n");
}

// One RGBA8 layer per texture page and palette, with its mip chain (texture_mips.h):
// sampled as the PS1 did through nearest_, filtered through smooth_.
bool VulkanBackend::make_pages() {
    ImageShape shape{texture_page_extent,
                     texture_page_extent,
                     texture_layer_capacity,
                     VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT,
                     page_mip_levels};
    pages_ = GpuImage::create(device_->device(), device_->allocator(), shape);
    if (!pages_)
        return false;
    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_NEAREST;
    sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device_->device(), &sampler, nullptr, &nearest_) != VK_SUCCESS || !make_smooth_sampler())
        return false;
    return device_->run_now([&](VkCommandBuffer commands) {
        transition(commands, pages_.image(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });
}

// The trilinear sampler at the anisotropy wanted (options_), replacing the old
// one, which the frames in flight are done with. On failure the old one stays.
bool VulkanBackend::make_smooth_sampler() {
    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.maxLod = VK_LOD_CLAMP_NONE;
    sampler.anisotropyEnable = options_.anisotropy > 1 ? VK_TRUE : VK_FALSE;
    sampler.maxAnisotropy = static_cast<float>(options_.anisotropy);
    VkSampler made = VK_NULL_HANDLE;
    if (vkCreateSampler(device_->device(), &sampler, nullptr, &made) != VK_SUCCESS)
        return false;
    if (smooth_)
        vkDestroySampler(device_->device(), smooth_, nullptr);
    smooth_ = made;
    return true;
}

// The pages through both samplers, into a slot's set.
void VulkanBackend::bind_pages(const FrameSlot &slot) {
    const std::array<VkDescriptorImageInfo, 2> images = {{
        {nearest_, pages_.view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {smooth_, pages_.view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
    }};
    std::array<VkWriteDescriptorSet, 2> writes{};
    for (std::size_t i = 0; i < writes.size(); ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = slot.descriptors;
        writes[i].dstBinding = i == 0 ? world_binding_pages : world_binding_smooth_pages;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(device_->device(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

bool VulkanBackend::make_frame_slots() {
    const std::array<VkDescriptorPoolSize, 2> sizes = {{
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * frames_in_flight},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, frames_in_flight},
    }};
    VkDescriptorPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = frames_in_flight;
    pool.poolSizeCount = static_cast<uint32_t>(sizes.size());
    pool.pPoolSizes = sizes.data();
    VkDevice device = device_->device();
    if (vkCreateDescriptorPool(device, &pool, nullptr, &descriptor_pool_) != VK_SUCCESS)
        return false;
    set_layout_ = create_world_set_layout(device);
    if (!set_layout_)
        return false;
    for (FrameSlot &slot : slots_) {
        VkCommandBufferAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate.commandPool = device_->command_pool();
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        VkFenceCreateInfo fence{};
        fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VkSemaphoreCreateInfo semaphore{};
        semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkDescriptorSetAllocateInfo set{};
        set.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        set.descriptorPool = descriptor_pool_;
        set.descriptorSetCount = 1;
        set.pSetLayouts = &set_layout_;
        if (vkAllocateCommandBuffers(device, &allocate, &slot.commands) != VK_SUCCESS ||
            vkCreateFence(device, &fence, nullptr, &slot.done) != VK_SUCCESS ||
            vkCreateSemaphore(device, &semaphore, nullptr, &slot.acquired) != VK_SUCCESS ||
            vkAllocateDescriptorSets(device, &set, &slot.descriptors) != VK_SUCCESS)
            return false;
        bind_pages(slot);
        if (!write_motions(slot, {}))
            return false;
    }
    return true;
}

bool VulkanBackend::wait_for_frames() {
    std::array<VkFence, frames_in_flight> fences{};
    for (std::size_t i = 0; i < slots_.size(); ++i)
        fences[i] = slots_[i].done;
    if (!fences[0])
        return true;
    return vkWaitForFences(device_->device(), frames_in_flight, fences.data(), VK_TRUE, UINT64_MAX) == VK_SUCCESS;
}

bool VulkanBackend::set_target(int logical_width, int scale) {
    if (logical_width == logical_width_ && scale == scale_ && target_)
        return true;
    const int old_width = logical_width_, old_scale = scale_;
    logical_width_ = logical_width;
    scale_ = scale;
    if (make_target())
        return true;
    logical_width_ = old_width;
    scale_ = old_scale;
    return false;
}

void VulkanBackend::set_options(const PictureOptions &options) {
    const bool precision_changed = options.true_color != options_.true_color;
    const bool supersample_changed = options.supersample != options_.supersample;
    const bool occlusion_changed = (options.ambient_occlusion > 0.0f) != (options_.ambient_occlusion > 0.0f) ||
                                   options.occlusion_quality != options_.occlusion_quality;
    const int old_anisotropy = options_.anisotropy;
    options_ = options;
    if (options.anisotropy != old_anisotropy && smooth_ && wait_for_frames()) {
        if (!make_smooth_sampler()) {
            std::fprintf(stderr, "[vulkan] cannot make the anisotropic sampler\n");
            options_.anisotropy = old_anisotropy;
        }
        for (const FrameSlot &slot : slots_)
            bind_pages(slot);
    }
    if (precision_changed && target_) {
        if (!make_target())
            std::fprintf(stderr, "[vulkan] cannot remake the render target\n");
        return;
    }
    if ((supersample_changed || occlusion_changed) && wait_for_frames()) {
        fit_downsample();
        fit_occlusion();
    }
}

void VulkanBackend::upload_layer(u8 layer, const Image &texels) {
    std::vector<Image> levels = texture_mip_chain(texels);
    if (levels.size() != page_mip_levels)
        return;
    pending_layers_.push_back({layer, std::move(levels)});
}

// Copies the layers uploaded since the last present, every mip level, into the texture array.
bool VulkanBackend::flush_layers() {
    if (pending_layers_.empty())
        return true;
    VkDeviceSize bytes = 0;
    for (const PendingLayer &pending : pending_layers_)
        for (const Image &level : pending.levels)
            bytes += level.rgba.size();
    GpuBuffer staging = GpuBuffer::create(device_->allocator(), bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                          BufferAccess::HostWrites);
    if (!staging || !staging.data())
        return false;
    std::vector<VkBufferImageCopy> copies;
    VkDeviceSize offset = 0;
    for (const PendingLayer &pending : pending_layers_) {
        for (uint32_t level = 0; level < pending.levels.size(); ++level) {
            const Image &texels = pending.levels[level];
            std::memcpy(static_cast<u8 *>(staging.data()) + offset, texels.rgba.data(), texels.rgba.size());
            VkBufferImageCopy copy{};
            copy.bufferOffset = offset;
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, pending.layer, 1};
            copy.imageExtent = {texels.width, texels.height, 1};
            copies.push_back(copy);
            offset += texels.rgba.size();
        }
    }
    staging.flush();
    pending_layers_.clear();
    return device_->run_now([&](VkCommandBuffer commands) {
        transition(commands, pages_.image(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdCopyBufferToImage(commands, staging.buffer(), pages_.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<uint32_t>(copies.size()), copies.data());
        transition(commands, pages_.image(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });
}

void VulkanBackend::upload_frame(std::span<const GpuVertex> vertices, std::span<const DrawRun> runs,
                                 const FrameStyle &style, const DepthCue &cue) {
    if (!wait_for_frames())
        return;
    const VkDeviceSize bytes = std::max<VkDeviceSize>(vertices.size_bytes(), sizeof(GpuVertex));
    if (!vertices_ || vertices_.size() < bytes) {
        vertices_ = GpuBuffer::create(device_->allocator(), bytes * 2, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                      BufferAccess::HostWrites);
        if (!vertices_ || !vertices_.data()) {
            runs_.clear();
            return;
        }
    }
    if (!vertices.empty())
        std::memcpy(vertices_.data(), vertices.data(), vertices.size_bytes());
    vertices_.flush();
    runs_.assign(runs.begin(), runs.end());
    style_ = style;
    cue_ = cue;
    frame_drawn_ = false;
}

bool VulkanBackend::write_motions(FrameSlot &slot, std::span<const Motion> motions) {
    const VkDeviceSize bytes = std::max<VkDeviceSize>(motions.size_bytes(), motion_bytes);
    if (!slot.motions || slot.motions.size() < bytes) {
        slot.motions = GpuBuffer::create(device_->allocator(), bytes * 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                         BufferAccess::HostWrites);
        if (!slot.motions || !slot.motions.data())
            return false;
        VkDescriptorBufferInfo buffer{slot.motions.buffer(), 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = slot.descriptors;
        write.dstBinding = world_binding_motions;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &buffer;
        vkUpdateDescriptorSets(device_->device(), 1, &write, 0, nullptr);
    }
    if (!motions.empty())
        std::memcpy(slot.motions.data(), motions.data(), motions.size_bytes());
    slot.motions.flush();
    return true;
}

// The frame's clip in target pixels. A full-width clip covers the widescreen
// columns too; narrower ones (menus, the opening's panels) stay where they were
// in the original 320 columns.
VkRect2D VulkanBackend::clip_rect() const {
    const int extra = (logical_width_ - render_width) / 2;
    const bool full_width = style_.clip_x <= 0 && style_.clip_x + style_.clip_width >= render_width;
    const std::int64_t clip_left = full_width ? -extra : style_.clip_x;
    const std::int64_t clip_right = full_width ? render_width + extra : std::int64_t{style_.clip_x} + style_.clip_width;
    const auto left = std::clamp<std::int64_t>(clip_left, -extra, render_width + extra);
    const auto top = std::clamp<std::int64_t>(style_.clip_y, 0, render_height);
    const auto right = std::clamp<std::int64_t>(clip_right, left, render_width + extra);
    const auto bottom = std::clamp<std::int64_t>(std::int64_t{style_.clip_y} + style_.clip_height, top, render_height);
    return {{static_cast<int32_t>((left + extra) * scale_), static_cast<int32_t>(top * scale_)},
            {static_cast<uint32_t>((right - left) * scale_), static_cast<uint32_t>((bottom - top) * scale_)}};
}

DrawConstants VulkanBackend::draw_constants(const FrameView &view) const {
    DrawConstants constants{};
    const int extra_columns = (logical_width_ - render_width) / 2;
    constants.view_columns = {static_cast<float>(extra_columns), static_cast<float>(logical_width_) / 2.0f};
    constants.center = {view.center_x, view.center_y};
    constants.projection_distance = view.projection_distance;
    constants.flags = (view.reproject ? draw_reproject : 0) | (view.whole_pixels ? draw_whole_pixels : 0) |
                      (options_.true_color ? draw_true_color : 0) |
                      (options_.texture_filter == TextureFilter::Bilinear ? draw_filter_bilinear : 0) |
                      (options_.texture_filter == TextureFilter::ThreePoint ? draw_filter_three_point : 0) |
                      (options_.texture_filter == TextureFilter::Sharp ? draw_filter_sharp : 0) |
                      (occlusion_ready_ ? draw_occlusion : 0) | (options_.fog == FogMode::Depth ? draw_fog_depth : 0) |
                      (options_.fog == FogMode::Distance ? draw_fog_distance : 0) |
                      (options_.mipmaps ? draw_mipmaps : 0);
    constants.pixel_scale = static_cast<uint32_t>(scale_);
    constants.carry = view.carry;
    constants.cue_slope = cue_.slope;
    constants.cue_projection_distance = cue_.projection_distance;
    constants.fog_color = cue_.far_color;
    constants.anisotropy = static_cast<float>(options_.anisotropy);
    constants.enhanced_depth = options_.enhanced_depth;
    return constants;
}

// The surfaces as this frame shows them, and the occlusion measured over them.
void VulkanBackend::measure_occlusion(VkCommandBuffer commands, const DrawConstants &constants, const FrameSlot &slot) {
    const SurfaceFrame frame{vertices_ ? vertices_.buffer() : VK_NULL_HANDLE, runs_, slot.descriptors, constants};
    surfaces_->record(commands, frame);
    occlusion_->record(commands, constants, options_.ambient_occlusion);
}

void VulkanBackend::draw_world(VkCommandBuffer commands, const FrameView &view, const FrameSlot &slot) {
    const DrawConstants constants = draw_constants(view);
    if (occlusion_ready_)
        measure_occlusion(commands, constants, slot);
    transition(commands, target_.image(), VK_IMAGE_ASPECT_COLOR_BIT, target_layout_,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    transition(commands, depth_.image(), VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    target_layout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    const ImageShape &shape = target_.shape();
    VkRenderingAttachmentInfoKHR color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
    color.imageView = target_.view();
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    // Each update is a complete face list, so depth starts empty.
    VkRenderingAttachmentInfoKHR depth{};
    depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
    depth.imageView = depth_.view();
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.clearValue.depthStencil = {1.0f, 0};
    VkRenderingInfoKHR rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR;
    rendering.renderArea = {{0, 0}, {shape.width, shape.height}};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    rendering.pDepthAttachment = &depth;
    vkCmdBeginRenderingKHR(commands, &rendering);

    const VkViewport viewport{0, 0, static_cast<float>(shape.width), static_cast<float>(shape.height), 0, 1};
    const VkRect2D clip = clip_rect();
    vkCmdSetViewport(commands, 0, 1, &viewport);
    vkCmdSetScissor(commands, 0, 1, &clip);
    if (style_.clear == FrameClear::Clear && clip.extent.width && clip.extent.height) {
        VkClearAttachment clear{};
        clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        clear.clearValue.color = {{clear_channel(style_.red), clear_channel(style_.green), clear_channel(style_.blue), 1}};
        const VkClearRect rect{clip, 0, 1};
        vkCmdClearAttachments(commands, 1, &clear, 1, &rect);
    }
    if (!runs_.empty() && vertices_)
        draw_runs(commands, constants, slot);
    vkCmdEndRenderingKHR(commands);
}

void VulkanBackend::draw_runs(VkCommandBuffer commands, DrawConstants constants, const FrameSlot &slot) {
    const VkDeviceSize offset = 0;
    VkBuffer buffer = vertices_.buffer();
    vkCmdBindVertexBuffers(commands, 0, 1, &buffer, &offset);
    const std::array<VkDescriptorSet, 2> sets = {slot.descriptors, occlusion_->occlusion_set()};
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_->layout(), 0,
                            static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
    const uint32_t base_flags = constants.flags;
    for (const DrawRun &run : runs_) {
        constants.blend = static_cast<uint32_t>(run.state.blend);
        for (const BlendPass &pass : blend_passes(run.state.blend)) {
            VkPipeline pipeline =
                pipelines_->pipeline({pass.equation, run.state.depth_test, run.state.depth_write});
            if (!pipeline)
                continue;
            constants.flags = base_flags | pass.flags;
            vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            vkCmdPushConstants(commands, pipelines_->layout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof constants, &constants);
            vkCmdDraw(commands, run.vertex_count, 1, run.first_vertex, 0);
        }
    }
}

// What the window shows, in TRANSFER_SRC_OPTIMAL: the target, or its average
// when supersampling.
const GpuImage &VulkanBackend::shown_picture(VkCommandBuffer commands) {
    if (!downsampled_) {
        transition(commands, target_.image(), VK_IMAGE_ASPECT_COLOR_BIT, target_layout_,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        target_layout_ = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        return target_;
    }
    transition(commands, target_.image(), VK_IMAGE_ASPECT_COLOR_BIT, target_layout_,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    target_layout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    downsample_->record(commands);
    return downsample_->picture();
}

// Letterboxes source (in TRANSFER_SRC_OPTIMAL) into a swapchain image: nearest
// at whole multiples, smoothed otherwise.
void VulkanBackend::blit_to(VkCommandBuffer commands, const GpuImage &source_image, VkImage image, VkExtent2D extent) {
    transition(commands, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkClearColorValue black{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(commands, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    const PresentRect rect =
        letterbox(logical_width_, static_cast<int>(extent.width), static_cast<int>(extent.height));
    const ImageShape &source = source_image.shape();
    if (rect.width > 0 && rect.height > 0) {
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {static_cast<int32_t>(source.width), static_cast<int32_t>(source.height), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[0] = {rect.x, rect.y, 0};
        blit.dstOffsets[1] = {rect.x + rect.width, rect.y + rect.height, 1};
        const bool whole = rect.width % static_cast<int>(source.width) == 0 &&
                           rect.height % static_cast<int>(source.height) == 0;
        const bool can_smooth = format_supports(device_->physical(), source.format,
                                                VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
        vkCmdBlitImage(commands, source_image.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       whole || !can_smooth ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    }
}

// Dear ImGui over the picture, then the image goes to the window.
void VulkanBackend::draw_overlay(VkCommandBuffer commands, const SwapchainImage &image, ImDrawData *overlay) {
    if (!overlay_started_ || !overlay || overlay->CmdListsCount == 0) {
        transition(commands, image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        return;
    }
    transition(commands, image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfoKHR color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
    color.imageView = image.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfoKHR rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR;
    rendering.renderArea = {{0, 0}, swapchain_->extent()};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    vkCmdBeginRenderingKHR(commands, &rendering);
    ImGui_ImplVulkan_RenderDrawData(overlay, commands);
    vkCmdEndRenderingKHR(commands);
    transition(commands, image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
}

void report_overlay_result(VkResult result) {
    if (result != VK_SUCCESS)
        std::fprintf(stderr, "[vulkan] overlay: VkResult %d\n", static_cast<int>(result));
}

bool VulkanBackend::start_overlay() {
    if (!swapchain_ || overlay_started_)
        return true;
    int width = 0, height = 0;
    if (!SDL_GetWindowSizeInPixels(window_, &width, &height) || width <= 0 || height <= 0)
        return false;
    const PresentPacing pacing = options_.vsync ? PresentPacing::Vsync : PresentPacing::Immediate;
    if (!swapchain_->fit(static_cast<uint32_t>(width), static_cast<uint32_t>(height), pacing))
        return false;
    overlay_format_ = swapchain_->format();
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_1;
    info.Instance = device_->instance();
    info.PhysicalDevice = device_->physical();
    info.Device = device_->device();
    info.QueueFamily = device_->queue_family();
    info.Queue = device_->queue();
    info.DescriptorPoolSize = 16;
    info.MinImageCount = 2;
    info.ImageCount = std::max<uint32_t>(2, swapchain_->image_count());
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &overlay_format_;
    info.CheckVkResultFn = report_overlay_result;
    overlay_started_ = ImGui_ImplVulkan_Init(&info);
    return overlay_started_;
}

void VulkanBackend::stop_overlay() {
    if (!overlay_started_)
        return;
    vkDeviceWaitIdle(device_->device());
    ImGui_ImplVulkan_Shutdown();
    overlay_started_ = false;
}

void VulkanBackend::present(const FrameView &view, std::span<const RenderLight>, int window_width, int window_height,
                            ImDrawData *overlay) {
    const bool to_window = swapchain_ != nullptr;
    if (to_window && (window_width <= 0 || window_height <= 0))
        return;
    if (!flush_layers())
        std::fprintf(stderr, "[vulkan] cannot upload textures\n");
    FrameSlot &slot = slots_[frame_number_ % frames_in_flight];
    if (vkWaitForFences(device_->device(), 1, &slot.done, VK_TRUE, UINT64_MAX) != VK_SUCCESS ||
        !write_motions(slot, view.motions))
        return;
    std::optional<SwapchainImage> image;
    if (to_window) {
        const PresentPacing pacing = options_.vsync ? PresentPacing::Vsync : PresentPacing::Immediate;
        if (!swapchain_->fit(static_cast<uint32_t>(window_width), static_cast<uint32_t>(window_height), pacing))
            return;
        image = swapchain_->acquire(slot.acquired);
        if (!image)
            return;   // rebuilt next time
    }
    if (vkResetFences(device_->device(), 1, &slot.done) != VK_SUCCESS ||
        vkResetCommandBuffer(slot.commands, 0) != VK_SUCCESS)
        return;
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(slot.commands, &begin) != VK_SUCCESS)
        return;
    // A kept picture (FrameClear::Retain) is drawn over once, never again.
    if (!frame_drawn_ || (view.reproject && style_.clear == FrameClear::Clear))
        draw_world(slot.commands, view, slot);
    frame_drawn_ = true;
    if (image) {
        blit_to(slot.commands, shown_picture(slot.commands), image->image, swapchain_->extent());
        draw_overlay(slot.commands, *image, overlay);
    }
    if (vkEndCommandBuffer(slot.commands) != VK_SUCCESS)
        return;
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = image ? 1 : 0;
    submit.pWaitSemaphores = &slot.acquired;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &slot.commands;
    submit.signalSemaphoreCount = image ? 1 : 0;
    submit.pSignalSemaphores = image ? &image->ready_to_present : nullptr;
    if (vkQueueSubmit(device_->queue(), 1, &submit, slot.done) != VK_SUCCESS) {
        std::fprintf(stderr, "[vulkan] submit failed\n");
        return;
    }
    ++frame_number_;
    if (image)
        swapchain_->present(*image);
}

std::optional<Image> VulkanBackend::capture() {
    if (!target_ || !wait_for_frames())
        return std::nullopt;
    const ImageShape &shape = target_.shape();
    const bool fifteen_bit = shape.format == classic_format;
    const VkDeviceSize texel_bytes = fifteen_bit ? 2 : 4;
    GpuBuffer readback = GpuBuffer::create(device_->allocator(), VkDeviceSize{shape.width} * shape.height * texel_bytes,
                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT, BufferAccess::HostReads);
    if (!readback || !readback.data())
        return std::nullopt;
    const VkImageLayout layout = target_layout_;
    const bool copied = device_->run_now([&](VkCommandBuffer commands) {
        transition(commands, target_.image(), VK_IMAGE_ASPECT_COLOR_BIT, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {shape.width, shape.height, 1};
        vkCmdCopyImageToBuffer(commands, target_.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer(), 1,
                               &copy);
    });
    target_layout_ = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (!copied)
        return std::nullopt;
    readback.invalidate();
    Image out{shape.width, shape.height, {}};
    out.rgba.resize(std::size_t{shape.width} * shape.height * 4);
    const auto *bytes = static_cast<const u8 *>(readback.data());
    for (std::size_t i = 0; i < std::size_t{shape.width} * shape.height; ++i) {
        if (!fifteen_bit) {
            std::memcpy(&out.rgba[i * 4], bytes + i * 4, 4);
            continue;
        }
        // A1R5G5B5: alpha in bit 15, then red, green, blue.
        const uint32_t texel = bytes[i * 2] | (uint32_t{bytes[i * 2 + 1]} << 8);
        out.rgba[i * 4] = expand5((texel >> 10) & color5_max);
        out.rgba[i * 4 + 1] = expand5((texel >> 5) & color5_max);
        out.rgba[i * 4 + 2] = expand5(texel & color5_max);
        out.rgba[i * 4 + 3] = 255;
    }
    return out;
}
bool VulkanBackend::restore(const Image &picture) {
    if (!target_ || !wait_for_frames())
        return false;
    const ImageShape &shape = target_.shape();
    const std::size_t texels = std::size_t{shape.width} * shape.height;
    if (picture.width != shape.width || picture.height != shape.height || picture.rgba.size() != texels * 4)
        return false;
    const bool fifteen_bit = shape.format == classic_format;
    GpuBuffer staging = GpuBuffer::create(device_->allocator(), texels * (fifteen_bit ? 2 : 4),
                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT, BufferAccess::HostWrites);
    if (!staging || !staging.data())
        return false;
    auto *bytes = static_cast<u8 *>(staging.data());
    if (fifteen_bit) {
        for (std::size_t i = 0; i < texels; ++i) {
            const u8 *rgba = &picture.rgba[i * 4];
            const uint32_t texel = 0x8000u | (uint32_t{rgba[0]} >> 3 << 10) | (uint32_t{rgba[1]} >> 3 << 5) |
                                   (uint32_t{rgba[2]} >> 3);
            bytes[i * 2] = static_cast<u8>(texel);
            bytes[i * 2 + 1] = static_cast<u8>(texel >> 8);
        }
    } else {
        std::memcpy(bytes, picture.rgba.data(), picture.rgba.size());
    }
    staging.flush();
    const VkImageLayout layout = target_layout_;
    const bool copied = device_->run_now([&](VkCommandBuffer commands) {
        transition(commands, target_.image(), VK_IMAGE_ASPECT_COLOR_BIT, layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {shape.width, shape.height, 1};
        vkCmdCopyBufferToImage(commands, staging.buffer(), target_.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        transition(commands, target_.image(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    });
    target_layout_ = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    frame_drawn_ = true;
    return copied;
}
} // namespace

BackendStart make_vulkan_backend(SDL_Window *window) {
    VulkanDeviceStart device = VulkanDevice::create(window);
    if (!device.device)
        return {nullptr, "Vulkan: " + device.error};
    auto backend = std::make_unique<VulkanBackend>(std::move(device.device), window);
    std::string error;
    if (!backend->start(error))
        return {nullptr, "Vulkan: " + error};
    return {std::move(backend), {}};
}
} // namespace kf
