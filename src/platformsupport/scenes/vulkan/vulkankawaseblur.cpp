/*
    SPDX-FileCopyrightText: 2026 Ark Royal <awright42mk1@protonmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "vulkankawaseblur.h"

#include "vulkanbackend.h"
#include "vulkancontext.h"
#include "vulkanframebuffer.h"
#include "vulkanrenderpass.h"

#include "utils/common.h"

#include "vulkankawaseblur_spv.inc"

#include <algorithm>

namespace KWin
{

VulkanKawaseBlur::VulkanKawaseBlur(VulkanContext *ctx, VkFormat colorFormat)
    : m_ctx(ctx)
    , m_colorFormat(colorFormat)
{
}

VulkanKawaseBlur::~VulkanKawaseBlur()
{
    if (!m_ctx || !m_ctx->backend()) {
        return;
    }
    VkDevice device = m_ctx->backend()->device();
    if (m_downsamplePipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_downsamplePipeline, nullptr);
    }
    if (m_upsamplePipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_upsamplePipeline, nullptr);
    }
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, m_pipelineLayout, nullptr);
    }
    if (m_dsLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, m_dsLayout, nullptr);
    }
    if (m_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, m_sampler, nullptr);
    }
    // m_renderPass cleaned up by its unique_ptr.
}

std::unique_ptr<VulkanKawaseBlur> VulkanKawaseBlur::create(VulkanContext *ctx, VkFormat colorFormat)
{
    if (!ctx || !ctx->backend()) {
        return nullptr;
    }
    // Can't use make_unique: the constructor is private.
    std::unique_ptr<VulkanKawaseBlur> self(new VulkanKawaseBlur(ctx, colorFormat));
    if (!self->init()) {
        return nullptr;
    }
    return self;
}

bool VulkanKawaseBlur::init()
{
    auto *backend = m_ctx->backend();
    VkDevice device = backend->device();

    {
        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = VK_FILTER_LINEAR;
        si.minFilter = VK_FILTER_LINEAR;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        if (vkCreateSampler(device, &si, nullptr, &m_sampler) != VK_SUCCESS) {
            qCWarning(KWIN_VULKAN) << "VulkanKawaseBlur: sampler create failed";
            return false;
        }
    }

    {
        // Binding 0: combined image sampler with the immutable linear sampler.
        // Push-descriptor flag tracks the backend so VulkanContext::bindDescriptors
        // can encode the writes inline; without the extension it falls back to a
        // pool allocation, which is why the flag must mirror supportsPushDescriptor.
        VkDescriptorSetLayoutBinding b{};
        b.binding = 0;
        b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        b.pImmutableSamplers = &m_sampler;
        VkDescriptorSetLayoutCreateInfo li{};
        li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 1;
        li.pBindings = &b;
        if (backend->supportsPushDescriptor()) {
            li.flags |= VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        }
        if (vkCreateDescriptorSetLayout(device, &li, nullptr, &m_dsLayout) != VK_SUCCESS) {
            qCWarning(KWIN_VULKAN) << "VulkanKawaseBlur: descriptor set layout create failed";
            return false;
        }
    }

    {
        // 12-byte fragment push constants: vec2 halfpixel + float offset.
        VkPushConstantRange pc{};
        pc.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pc.offset = 0;
        pc.size = 12;
        VkPipelineLayoutCreateInfo li{};
        li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        li.setLayoutCount = 1;
        li.pSetLayouts = &m_dsLayout;
        li.pushConstantRangeCount = 1;
        li.pPushConstantRanges = &pc;
        if (vkCreatePipelineLayout(device, &li, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
            qCWarning(KWIN_VULKAN) << "VulkanKawaseBlur: pipeline layout create failed";
            return false;
        }
    }

    {
        VulkanRenderPass::Config cfg;
        cfg.colorFormat = m_colorFormat;
        cfg.colorLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        cfg.colorStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        cfg.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
        cfg.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
        m_renderPass = VulkanRenderPass::create(m_ctx, cfg);
        if (!m_renderPass) {
            qCWarning(KWIN_VULKAN) << "VulkanKawaseBlur: render pass create failed";
            return false;
        }
    }

    auto makeMod = [&](const uint32_t *code, size_t bytes) -> VkShaderModule {
        VkShaderModuleCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        si.codeSize = bytes;
        si.pCode = code;
        VkShaderModule mod = VK_NULL_HANDLE;
        vkCreateShaderModule(device, &si, nullptr, &mod);
        return mod;
    };
    VkShaderModule vertMod = makeMod(VulkanKawaseBlurShaders::kVertSpv,
                                     sizeof(VulkanKawaseBlurShaders::kVertSpv));
    VkShaderModule dsMod = makeMod(VulkanKawaseBlurShaders::kDownsampleSpv,
                                   sizeof(VulkanKawaseBlurShaders::kDownsampleSpv));
    VkShaderModule usMod = makeMod(VulkanKawaseBlurShaders::kUpsampleSpv,
                                   sizeof(VulkanKawaseBlurShaders::kUpsampleSpv));
    if (!vertMod || !dsMod || !usMod) {
        if (vertMod) {
            vkDestroyShaderModule(device, vertMod, nullptr);
        }
        if (dsMod) {
            vkDestroyShaderModule(device, dsMod, nullptr);
        }
        if (usMod) {
            vkDestroyShaderModule(device, usMod, nullptr);
        }
        qCWarning(KWIN_VULKAN) << "VulkanKawaseBlur: shader module create failed";
        return false;
    }

    auto buildPipeline = [&](VkShaderModule frag, VkPipeline *out) -> bool {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertMod;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag;
        stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{};
        vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState att{};
        att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
            | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1;
        cb.pAttachments = &att;
        VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyni{};
        dyni.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dyni.dynamicStateCount = 2;
        dyni.pDynamicStates = dyn;
        VkGraphicsPipelineCreateInfo gp{};
        gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gp.stageCount = 2;
        gp.pStages = stages;
        gp.pVertexInputState = &vi;
        gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vp;
        gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms;
        gp.pColorBlendState = &cb;
        gp.pDynamicState = &dyni;
        gp.layout = m_pipelineLayout;
        gp.renderPass = m_renderPass->renderPass();
        gp.subpass = 0;
        return vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, nullptr, out)
            == VK_SUCCESS;
    };
    const bool ok = buildPipeline(dsMod, &m_downsamplePipeline)
        && buildPipeline(usMod, &m_upsamplePipeline);
    vkDestroyShaderModule(device, vertMod, nullptr);
    vkDestroyShaderModule(device, dsMod, nullptr);
    vkDestroyShaderModule(device, usMod, nullptr);
    if (!ok) {
        qCWarning(KWIN_VULKAN) << "VulkanKawaseBlur: graphics pipeline create failed";
        return false;
    }
    return true;
}

void VulkanKawaseBlur::recordPass(VkCommandBuffer cmd, VkPipeline pipeline, VkImageView srcView,
                                  const QSize &srcSize, const Level &dst, float offset,
                                  bool flipViewportY) const
{
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    VkDescriptorImageInfo ii{};
    ii.sampler = VK_NULL_HANDLE; // immutable in the DS layout
    ii.imageView = srcView;
    ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet w{};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    m_ctx->bindDescriptors(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
                           m_dsLayout, 0, 1, &w);

    struct
    {
        float halfpixel[2];
        float offset;
    } pc{};
    pc.halfpixel[0] = 0.5f / float(std::max(1, srcSize.width()));
    pc.halfpixel[1] = 0.5f / float(std::max(1, srcSize.height()));
    pc.offset = offset;
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

    const int dw = std::max(1, dst.size.width());
    const int dh = std::max(1, dst.size.height());
    VkViewport vp{};
    vp.x = 0.0f;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vp.width = float(dw);
    if (flipViewportY) {
        // Negative-height viewport for top-down sources (e.g. swapchain
        // captures): flips device-Y so the UV orientation stays consistent
        // across the down/up chain.
        vp.y = float(dh);
        vp.height = -float(dh);
    } else {
        vp.y = 0.0f;
        vp.height = float(dh);
    }
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{{0, 0}, {uint32_t(dw), uint32_t(dh)}};
    vkCmdSetScissor(cmd, 0, 1, &sc);

    const VkRect2D area{{0, 0}, {uint32_t(dw), uint32_t(dh)}};
    m_renderPass->begin(cmd, dst.framebuffer->framebuffer(), area, nullptr, 0);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    m_renderPass->end(cmd);
}

void VulkanKawaseBlur::recordPyramid(VkCommandBuffer cmd, const std::vector<Level> &levels,
                                     float offset, bool flipViewportY) const
{
    const size_t n = levels.size();
    if (n < 2) {
        return;
    }

    // Each pass's colour-attachment write must be visible to the next pass's
    // fragment-shader read; the trailing one after the final upsample also
    // serves consumers that blit or sample levels[0] (hence TRANSFER_READ).
    // The caller is responsible for making levels[0]'s initial contents
    // visible to the first downsample's shader read.
    auto passBarrier = [&]() {
        VkMemoryBarrier mb{};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &mb, 0, nullptr, 0, nullptr);
    };

    // Downsample: 0 → 1 → … → N, barrier after each pass.
    for (size_t i = 1; i < n; ++i) {
        recordPass(cmd, m_downsamplePipeline, levels[i - 1].view, levels[i - 1].size,
                   levels[i], offset, flipViewportY);
        passBarrier();
    }

    // Upsample: N → … → 0, barrier after each pass (the last makes the result
    // in levels[0] visible to the consumer).
    for (size_t i = n - 1; i > 0; --i) {
        recordPass(cmd, m_upsamplePipeline, levels[i].view, levels[i].size,
                   levels[i - 1], offset, flipViewportY);
        passBarrier();
    }
}

} // namespace KWin
