#include "vulkan_surfaces.h"

#include <surface_frag.h>
#include <world_vert.h>

#include <array>

namespace kf {
namespace {
constexpr VkFormat surface_format = VK_FORMAT_R32G32B32A32_SFLOAT;

VkPipeline create_surface_pipeline(const VulkanDevice &owner, VkPipelineLayout layout, const RenderPass &pass) {
    VkDevice device = owner.device();
    VkShaderModule vertex_module = create_shader_module(device, world_vert);
    VkShaderModule fragment_module = create_shader_module(device, surface_frag);
    VkPipeline pipeline = VK_NULL_HANDLE;
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
        const VkPipelineVertexInputStateCreateInfo vertex_input = face_vertex_input();
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
        VkPipelineDepthStencilStateCreateInfo depth{};
        depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth.depthTestEnable = VK_TRUE;
        depth.depthWriteEnable = VK_TRUE;
        depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkPipelineColorBlendAttachmentState attachment{};
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
        info.pVertexInputState = &vertex_input;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = &depth;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = layout;
        info.renderPass = pass.handle();
        if (vkCreateGraphicsPipelines(device, owner.pipeline_cache(), 1, &info, nullptr, &pipeline) != VK_SUCCESS)
            pipeline = VK_NULL_HANDLE;
    }
    if (vertex_module)
        vkDestroyShaderModule(device, vertex_module, nullptr);
    if (fragment_module)
        vkDestroyShaderModule(device, fragment_module, nullptr);
    return pipeline;
}
} // namespace

std::unique_ptr<SurfaceBuffer> SurfaceBuffer::create(const VulkanDevice &device, VkDescriptorSetLayout world_set_layout,
                                                     VkFormat depth_format) {
    VkDevice handle = device.device();
    auto out = std::make_unique<SurfaceBuffer>(Key{}, handle);
    out->allocator_ = device.allocator();
    out->pass_ = RenderPass::create(handle, {surface_format, depth_format, ColorLoad::Clear});
    out->layout_ = create_face_pipeline_layout(handle, std::span(&world_set_layout, 1));
    if (!out->pass_ || !out->layout_)
        return nullptr;
    out->pipeline_ = create_surface_pipeline(device, out->layout_, out->pass_);
    if (!out->pipeline_)
        return nullptr;
    return out;
}

SurfaceBuffer::~SurfaceBuffer() {
    if (pipeline_)
        vkDestroyPipeline(device_, pipeline_, nullptr);
    if (layout_)
        vkDestroyPipelineLayout(device_, layout_, nullptr);
}

bool SurfaceBuffer::fit(uint32_t width, uint32_t height) {
    if (surfaces_ && surfaces_.shape().width == width && surfaces_.shape().height == height)
        return true;
    const ImageShape color{width, height, 1, surface_format,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT};
    const ImageShape depth{width, height, 1, pass_.shape().depth, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                           VK_IMAGE_ASPECT_DEPTH_BIT};
    GpuImage new_surfaces = GpuImage::create(device_, allocator_, color);
    GpuImage new_depth = GpuImage::create(device_, allocator_, depth);
    if (!new_surfaces || !new_depth)
        return false;
    const std::array<VkImageView, 2> views = {new_surfaces.view(), new_depth.view()};
    Framebuffer new_framebuffer = Framebuffer::create(device_, pass_, views, {width, height});
    if (!new_framebuffer)
        return false;
    framebuffer_ = std::move(new_framebuffer);
    surfaces_ = std::move(new_surfaces);
    depth_ = std::move(new_depth);
    return true;
}

void SurfaceBuffer::record(VkCommandBuffer commands, const SurfaceFrame &frame) {
    transition(commands, surfaces_.image(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    transition(commands, depth_.image(), VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    begin_pass(commands, pass_, framebuffer_);
    if (frame.vertices) {
        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(commands, 0, 1, &frame.vertices, &offset);
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &frame.world_set, 0, nullptr);
        DrawConstants constants = frame.constants;
        constants.flags = (constants.flags & ~draw_occlusion) | draw_surface_pass;
        constants.blend = static_cast<uint32_t>(BlendMode::opaque);
        vkCmdPushConstants(commands, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof constants, &constants);
        for (const DrawRun &run : frame.runs)
            if (run.state.blend == BlendMode::opaque)
                vkCmdDraw(commands, run.vertex_count, 1, run.first_vertex, 0);
    }
    vkCmdEndRenderPass(commands);
    transition(commands, surfaces_.image(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}
} // namespace kf
