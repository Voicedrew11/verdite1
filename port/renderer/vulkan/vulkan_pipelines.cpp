#include "vulkan_pipelines.h"

#include <kf/renderer/backend.h>

#include <world_frag.h>
#include <world_vert.h>

#include <algorithm>
#include <array>
#include <cstddef>

namespace kf {
namespace {
// The push constants of shaders/draw.glsl.
constexpr uint32_t draw_constants_size = 32;

static_assert(sizeof(GpuVertex) == 52);
static_assert(offsetof(GpuVertex, cx) == 12 && offsetof(GpuVertex, u) == 24 && offsetof(GpuVertex, uv_bounds) == 32 &&
              offsetof(GpuVertex, color) == 40 && offsetof(GpuVertex, layer) == 44 && offsetof(GpuVertex, entity) == 48);

// world.vert's inputs, in GpuVertex.
constexpr std::array<VkVertexInputAttributeDescription, 7> vertex_attributes = {{
    {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GpuVertex, x)},
    {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GpuVertex, cx)},
    {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(GpuVertex, u)},
    {3, 0, VK_FORMAT_R16G16B16A16_UINT, offsetof(GpuVertex, uv_bounds)},
    {4, 0, VK_FORMAT_R8G8B8A8_UINT, offsetof(GpuVertex, color)},
    {5, 0, VK_FORMAT_R8G8B8A8_UINT, offsetof(GpuVertex, layer)},
    {6, 0, VK_FORMAT_R16G16_UINT, offsetof(GpuVertex, entity)},
}};

VkShaderModule shader_module(VkDevice device, const uint32_t *code, std::size_t bytes) {
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = bytes;
    info.pCode = code;
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return module;
}

VkPipelineColorBlendAttachmentState blend_state(BlendEquation equation) {
    VkPipelineColorBlendAttachmentState out{};
    // The target's alpha (one bit in 15-bit colour) means nothing; leave it.
    out.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    out.alphaBlendOp = VK_BLEND_OP_ADD;
    out.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    out.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    switch (equation) {
    case BlendEquation::None:
        break;
    case BlendEquation::Average:
        out.blendEnable = VK_TRUE;
        out.colorBlendOp = VK_BLEND_OP_ADD;
        out.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        out.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    case BlendEquation::AddWeighted:
        out.blendEnable = VK_TRUE;
        out.colorBlendOp = VK_BLEND_OP_ADD;
        out.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        out.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        break;
    case BlendEquation::ReverseSubtract:
        out.blendEnable = VK_TRUE;
        out.colorBlendOp = VK_BLEND_OP_REVERSE_SUBTRACT;
        out.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        out.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        break;
    }
    return out;
}

} // namespace

VkDescriptorSetLayout create_world_set_layout(VkDevice device) {
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = static_cast<uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(device, &info, nullptr, &layout) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return layout;
}

std::unique_ptr<WorldPipelines> WorldPipelines::create(VkDevice device, VkDescriptorSetLayout set_layout,
                                                       VkFormat color, VkFormat depth) {
    auto out = std::make_unique<WorldPipelines>(Key{}, device);
    out->color_ = color;
    out->depth_ = depth;
    VkPushConstantRange constants{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, draw_constants_size};
    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &set_layout;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &constants;
    if (vkCreatePipelineLayout(device, &layout_info, nullptr, &out->layout_) != VK_SUCCESS)
        return nullptr;
    out->vertex_ = shader_module(device, world_vert, sizeof world_vert);
    out->fragment_ = shader_module(device, world_frag, sizeof world_frag);
    if (!out->vertex_ || !out->fragment_)
        return nullptr;
    return out;
}

WorldPipelines::~WorldPipelines() {
    for (const Built &built : built_)
        vkDestroyPipeline(device_, built.pipeline, nullptr);
    if (vertex_)
        vkDestroyShaderModule(device_, vertex_, nullptr);
    if (fragment_)
        vkDestroyShaderModule(device_, fragment_, nullptr);
    if (layout_)
        vkDestroyPipelineLayout(device_, layout_, nullptr);
}

VkPipeline WorldPipelines::pipeline(PipelineKey key) {
    const auto found = std::find_if(built_.begin(), built_.end(), [&](const Built &b) { return b.key == key; });
    if (found != built_.end())
        return found->pipeline;
    VkPipeline made = build(key);
    if (made)
        built_.push_back({key, made});
    return made;
}

VkPipeline WorldPipelines::build(PipelineKey key) const {
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex_;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment_;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{0, sizeof(GpuVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = static_cast<uint32_t>(vertex_attributes.size());
    vertex_input.pVertexAttributeDescriptions = vertex_attributes.data();

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
    raster.cullMode = VK_CULL_MODE_NONE;   // the game culls; its faces come either way round
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = key.depth_test ? VK_TRUE : VK_FALSE;
    depth.depthWriteEnable = key.depth_write ? VK_TRUE : VK_FALSE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    const VkPipelineColorBlendAttachmentState attachment = blend_state(key.blend);
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &attachment;

    const std::array<VkDynamicState, 2> dynamic_states = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
    dynamic.pDynamicStates = dynamic_states.data();

    VkPipelineRenderingCreateInfoKHR rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &color_;
    rendering.depthAttachmentFormat = depth_;

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext = &rendering;
    info.stageCount = static_cast<uint32_t>(stages.size());
    info.pStages = stages.data();
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = layout_;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return pipeline;
}
} // namespace kf
