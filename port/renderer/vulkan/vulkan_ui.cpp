// The interface drawn over the window's picture (vulkan_ui.h)
#include "vulkan_ui.h"

#include "vulkan_pipelines.h"
#include "vulkan_resources.h"

#include <ui_frag.h>
#include <ui_vert.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kf {
namespace {
using verdite::ui::Frame;
using verdite::ui::TextureId;
using verdite::ui::TextureUpload;
using verdite::ui::Vertex;

constexpr uint32_t max_textures = 512;   // descriptor sets: glyph pages, images and the white pixel
constexpr int max_slots = 4;

struct Push {
    std::array<float, 2> scale;
    std::array<float, 2> translation;
};

struct Texture {
    GpuImage image;
    VkDescriptorSet set = VK_NULL_HANDLE;
};

struct Slot {
    GpuBuffer vertices;
    GpuBuffer indices;
    std::vector<GpuBuffer> staging;   // this slot's uploads, freed when it comes round again
};

struct Release {
    TextureId id;
    uint64_t when;   // freed at this prepare count, when no frame in flight can draw it
};

// A host-written buffer of at least bytes, grown by half again so a menu that
// grows a little does not make one each frame.
bool fit_buffer(VmaAllocator allocator, GpuBuffer &buffer, VkDeviceSize bytes, VkBufferUsageFlags usage) {
    if (buffer && buffer.size() >= bytes)
        return true;
    buffer = GpuBuffer::create(allocator, bytes + bytes / 2, usage, BufferAccess::HostWrites);
    return buffer && buffer.data();
}
} // namespace

struct UiPass::State {
    const VulkanDevice &device;
    int frames = 0;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    Texture white;
    bool white_uploaded = false;
    std::unordered_map<TextureId, Texture> textures;
    std::vector<Release> releases;
    uint64_t prepared = 0;
    std::array<Slot, max_slots> slots;

    explicit State(const VulkanDevice &owner) : device(owner) {}
    ~State();
    State(const State &) = delete;
    State &operator=(const State &) = delete;

    [[nodiscard]] bool make_layouts();
    [[nodiscard]] bool make_pipeline(const RenderPass &pass);
    [[nodiscard]] bool make_texture(uint32_t width, uint32_t height, Texture &out) const;
    void free_texture(Texture &texture) const;
    [[nodiscard]] bool upload(VkCommandBuffer commands, Slot &slot, const Texture &texture, uint32_t width,
                              uint32_t height, const uint8_t *rgba) const;
    void free_released();
    void make_uploads(VkCommandBuffer commands, Slot &slot, const Frame &frame);
};

UiPass::State::~State() {
    VkDevice handle = device.device();
    for (auto &[id, texture] : textures)
        free_texture(texture);
    free_texture(white);
    if (pipeline)
        vkDestroyPipeline(handle, pipeline, nullptr);
    if (layout)
        vkDestroyPipelineLayout(handle, layout, nullptr);
    if (pool)
        vkDestroyDescriptorPool(handle, pool, nullptr);
    if (set_layout)
        vkDestroyDescriptorSetLayout(handle, set_layout, nullptr);
    if (sampler)
        vkDestroySampler(handle, sampler, nullptr);
}

bool UiPass::State::make_layouts() {
    VkDevice handle = device.device();
    sampler = create_sampler(handle, VK_FILTER_LINEAR);
    const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                               VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo set_info{};
    set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_info.bindingCount = 1;
    set_info.pBindings = &binding;
    const VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, max_textures};
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = max_textures;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    if (!sampler || vkCreateDescriptorSetLayout(handle, &set_info, nullptr, &set_layout) != VK_SUCCESS ||
        vkCreateDescriptorPool(handle, &pool_info, nullptr, &pool) != VK_SUCCESS)
        return false;
    const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &set_layout;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push;
    return vkCreatePipelineLayout(handle, &layout_info, nullptr, &layout) == VK_SUCCESS;
}

bool UiPass::State::make_pipeline(const RenderPass &pass) {
    VkDevice handle = device.device();
    VkShaderModule vertex_module = create_shader_module(handle, ui_vert);
    VkShaderModule fragment_module = create_shader_module(handle, ui_frag);
    if (vertex_module && fragment_module) {
        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertex_module;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragment_module;
        stages[1].pName = "main";
        const VkVertexInputBindingDescription vertex{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
        const std::array<VkVertexInputAttributeDescription, 3> attributes = {{
            {0, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(Vertex, x))},
            {1, 0, VK_FORMAT_R8G8B8A8_UNORM, static_cast<uint32_t>(offsetof(Vertex, r))},
            {2, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(Vertex, u))},
        }};
        VkPipelineVertexInputStateCreateInfo input{};
        input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        input.vertexBindingDescriptionCount = 1;
        input.pVertexBindingDescriptions = &vertex;
        input.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
        input.pVertexAttributeDescriptions = attributes.data();
        VkPipelineInputAssemblyStateCreateInfo assembly{};
        assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{};
        viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{};
        raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState attachment{};
        attachment.blendEnable = VK_TRUE;
        attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;   // premultiplied
        attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        attachment.colorBlendOp = VK_BLEND_OP_ADD;
        attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        attachment.alphaBlendOp = VK_BLEND_OP_ADD;
        attachment.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{};
        blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.attachmentCount = 1;
        blend.pAttachments = &attachment;
        const std::array<VkDynamicState, 2> dynamic_states = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
        dynamic.pDynamicStates = dynamic_states.data();
        VkGraphicsPipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.stageCount = static_cast<uint32_t>(stages.size());
        info.pStages = stages.data();
        info.pVertexInputState = &input;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = layout;
        info.renderPass = pass.handle();
        if (vkCreateGraphicsPipelines(handle, device.pipeline_cache(), 1, &info, nullptr, &pipeline) != VK_SUCCESS)
            pipeline = VK_NULL_HANDLE;
    }
    if (vertex_module)
        vkDestroyShaderModule(handle, vertex_module, nullptr);
    if (fragment_module)
        vkDestroyShaderModule(handle, fragment_module, nullptr);
    return pipeline != VK_NULL_HANDLE;
}

bool UiPass::State::make_texture(uint32_t width, uint32_t height, Texture &out) const {
    ImageShape shape;
    shape.width = width;
    shape.height = height;
    shape.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    out.image = GpuImage::create(device.device(), device.allocator(), shape);
    if (!out.image)
        return false;
    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &set_layout;
    if (vkAllocateDescriptorSets(device.device(), &allocate, &out.set) != VK_SUCCESS) {
        out.set = VK_NULL_HANDLE;
        out.image = GpuImage{};
        return false;
    }
    const VkDescriptorImageInfo image{sampler, out.image.view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = out.set;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(device.device(), 1, &write, 0, nullptr);
    return true;
}

void UiPass::State::free_texture(Texture &texture) const {
    if (texture.set)
        vkFreeDescriptorSets(device.device(), pool, 1, &texture.set);
    texture.set = VK_NULL_HANDLE;
    texture.image = GpuImage{};
}

bool UiPass::State::upload(VkCommandBuffer commands, Slot &slot, const Texture &texture, uint32_t width,
                           uint32_t height, const uint8_t *rgba) const {
    const VkDeviceSize bytes = VkDeviceSize{width} * height * 4;
    GpuBuffer staging = GpuBuffer::create(device.allocator(), bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                          BufferAccess::HostWrites);
    if (!staging || !staging.data())
        return false;
    std::memcpy(staging.data(), rgba, static_cast<std::size_t>(bytes));
    staging.flush();
    transition(commands, texture.image.image(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(commands, staging.buffer(), texture.image.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &copy);
    transition(commands, texture.image.image(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    slot.staging.push_back(std::move(staging));
    return true;
}

// The textures let go of before that no frame in flight draws now.
void UiPass::State::free_released() {
    for (std::size_t i = 0; i < releases.size();) {
        if (releases[i].when > prepared) {
            ++i;
            continue;
        }
        auto found = textures.find(releases[i].id);
        if (found != textures.end()) {
            free_texture(found->second);
            textures.erase(found);
        }
        releases[i] = releases.back();
        releases.pop_back();
    }
}

void UiPass::State::make_uploads(VkCommandBuffer commands, Slot &slot, const Frame &frame) {
    if (!white_uploaded) {
        const std::array<uint8_t, 4> pixel = {255, 255, 255, 255};
        white_uploaded = upload(commands, slot, white, 1, 1, pixel.data());
    }
    for (const TextureUpload &made : frame.uploads) {
        const bool whole = made.width_px > 0 && made.height_px > 0 &&
                           made.rgba.size() == static_cast<std::size_t>(made.width_px) *
                                                   static_cast<std::size_t>(made.height_px) * 4;
        Texture texture;
        const auto width = static_cast<uint32_t>(made.width_px);
        const auto height = static_cast<uint32_t>(made.height_px);
        if (!whole || !make_texture(width, height, texture) ||
            !upload(commands, slot, texture, width, height, made.rgba.data())) {
            std::fprintf(stderr, "[ui] texture %u (%dx%d) could not be made\n", made.id, made.width_px,
                         made.height_px);
            free_texture(texture);
            continue;
        }
        textures[made.id] = std::move(texture);
    }
    for (TextureId id : frame.releases)
        releases.push_back({id, prepared + static_cast<uint64_t>(frames)});
}

UiPass::UiPass(std::unique_ptr<State> state) : state_(std::move(state)) {}
UiPass::~UiPass() = default;

std::unique_ptr<UiPass> UiPass::create(const VulkanDevice &device, const RenderPass &pass, int frames_in_flight) {
    if (frames_in_flight <= 0 || frames_in_flight > max_slots)
        return nullptr;
    auto state = std::make_unique<State>(device);
    state->frames = frames_in_flight;
    if (!state->make_layouts() || !state->make_pipeline(pass) || !state->make_texture(1, 1, state->white)) {
        std::fprintf(stderr, "[ui] the interface's pipeline could not be made: it is not drawn\n");
        return nullptr;
    }
    return std::unique_ptr<UiPass>(new UiPass(std::move(state)));
}

void UiPass::prepare(VkCommandBuffer commands, int slot_index, const Frame &frame) {
    State &s = *state_;
    if (slot_index < 0 || slot_index >= s.frames)
        return;
    Slot &slot = s.slots[static_cast<std::size_t>(slot_index)];
    slot.staging.clear();   // this slot's last frame has finished
    ++s.prepared;
    s.free_released();
    s.make_uploads(commands, slot, frame);
    const VkDeviceSize vertex_bytes = frame.vertices.size() * sizeof(Vertex);
    const VkDeviceSize index_bytes = frame.indices.size() * sizeof(uint32_t);
    if (vertex_bytes == 0 || index_bytes == 0 ||
        !fit_buffer(s.device.allocator(), slot.vertices, vertex_bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ||
        !fit_buffer(s.device.allocator(), slot.indices, index_bytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT))
        return;
    std::memcpy(slot.vertices.data(), frame.vertices.data(), static_cast<std::size_t>(vertex_bytes));
    std::memcpy(slot.indices.data(), frame.indices.data(), static_cast<std::size_t>(index_bytes));
    slot.vertices.flush();
    slot.indices.flush();
}

namespace {
// The draw's clip in the target, or nothing when it covers no pixel.
std::optional<VkRect2D> draw_scissor(const verdite::ui::Draw &draw, VkExtent2D extent) {
    if (!draw.clipped)
        return VkRect2D{{0, 0}, extent};
    const int32_t x0 = std::max(draw.clip.x, 0);
    const int32_t y0 = std::max(draw.clip.y, 0);
    const int64_t x1 = std::min<int64_t>(int64_t{draw.clip.x} + draw.clip.width, extent.width);
    const int64_t y1 = std::min<int64_t>(int64_t{draw.clip.y} + draw.clip.height, extent.height);
    if (x1 <= x0 || y1 <= y0)
        return std::nullopt;
    return VkRect2D{{x0, y0}, {static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0)}};
}
} // namespace

void UiPass::draw(VkCommandBuffer commands, int slot_index, const Frame &frame, VkExtent2D extent) const {
    const State &s = *state_;
    if (slot_index < 0 || slot_index >= s.frames || extent.width == 0 || extent.height == 0 || frame.draws.empty() ||
        !s.white_uploaded)
        return;
    const Slot &slot = s.slots[static_cast<std::size_t>(slot_index)];
    if (!slot.vertices || !slot.indices || slot.vertices.size() < frame.vertices.size() * sizeof(Vertex) ||
        slot.indices.size() < frame.indices.size() * sizeof(uint32_t))
        return;   // prepare could not make its buffers
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipeline);
    VkBuffer vertex_buffer = slot.vertices.buffer();
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(commands, 0, 1, &vertex_buffer, &offset);
    vkCmdBindIndexBuffer(commands, slot.indices.buffer(), 0, VK_INDEX_TYPE_UINT32);
    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height),
                              0.0f, 1.0f};
    vkCmdSetViewport(commands, 0, 1, &viewport);
    VkDescriptorSet bound = VK_NULL_HANDLE;
    for (const verdite::ui::Draw &draw : frame.draws) {
        if (uint64_t{draw.first_index} + draw.index_count > frame.indices.size())
            continue;
        VkDescriptorSet set = s.white.set;
        if (draw.texture != 0) {
            auto found = s.textures.find(draw.texture);
            if (found == s.textures.end())
                continue;   // a texture that could not be made
            set = found->second.set;
        }
        const std::optional<VkRect2D> scissor = draw_scissor(draw, extent);
        if (!scissor)
            continue;
        if (set != bound) {
            vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, s.layout, 0, 1, &set, 0, nullptr);
            bound = set;
        }
        vkCmdSetScissor(commands, 0, 1, &*scissor);
        const Push push{{2.0f / static_cast<float>(extent.width), 2.0f / static_cast<float>(extent.height)},
                        {draw.translate_x, draw.translate_y}};
        vkCmdPushConstants(commands, s.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
        vkCmdDrawIndexed(commands, draw.index_count, 1, draw.first_index, static_cast<int32_t>(draw.vertex_offset),
                         0);
    }
}
} // namespace kf
