/*
    SPDX-FileCopyrightText: 2025 KWin Authors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "scene/cursordelegate_vulkan.h"

#include "compositor.h"
#include "core/output.h"
#include "core/pixelgrid.h"
#include "core/renderlayer.h"
#include "core/rendertarget.h"
#include "core/renderviewport.h"
#include "platformsupport/scenes/vulkan/vulkanbackend.h"
#include "platformsupport/scenes/vulkan/vulkanbuffer.h"
#include "platformsupport/scenes/vulkan/vulkancontext.h"
#include "platformsupport/scenes/vulkan/vulkanframebuffer.h"
#include "platformsupport/scenes/vulkan/vulkanpipeline.h"
#include "platformsupport/scenes/vulkan/vulkanpipelinemanager.h"
#include "platformsupport/scenes/vulkan/vulkanrenderpass.h"
#include "platformsupport/scenes/vulkan/vulkanrendertarget.h"
#include "platformsupport/scenes/vulkan/vulkantexture.h"
#include "scene/cursorscene.h"

#include <array>
#include <cstring>

namespace KWin
{

CursorDelegateVulkan::CursorDelegateVulkan(Scene *scene, Output *output, VulkanContext *context)
    : SceneDelegate(scene, nullptr)
    , m_output(output)
    , m_context(context)
{
}

CursorDelegateVulkan::~CursorDelegateVulkan()
{
}

void CursorDelegateVulkan::paint(const RenderTarget &renderTarget, const QRegion &region)
{
    const QRegion dirty = region.intersected(layer()->mapToGlobal(layer()->rect()).toAlignedRect());
    if (dirty.isEmpty()) {
        return;
    }

    CursorScene *cursorScene = Compositor::self()->cursorScene();
    if (!cursorScene || !renderTarget.isVulkan()) {
        return;
    }
    VulkanRenderTarget *mainTarget = renderTarget.vulkanTarget();
    VulkanFramebuffer *mainFb = mainTarget ? mainTarget->framebuffer() : nullptr;
    if (!mainFb) {
        return;
    }

    const QRect cursorRect = snapToPixelGrid(scaledRect(layer()->mapToGlobal(layer()->rect()), m_output->scale()));
    const QSize bufferSize = cursorRect.size();
    if (bufferSize.isEmpty()) {
        return;
    }

    const VkFormat colorFormat = m_context->backend()->colorFormat();

    // Lazy-init offscreen render pass (format-dependent, not size-dependent)
    if (!m_offscreenRenderPass) {
        m_offscreenRenderPass = VulkanRenderPass::createForOffscreen(m_context, colorFormat);
        if (!m_offscreenRenderPass) {
            return;
        }
    }

    // Create/resize offscreen framebuffer when the cursor size changes
    if (!m_offscreenFramebuffer || m_offscreenFramebuffer->size() != bufferSize) {
        m_offscreenFramebuffer = VulkanFramebuffer::createWithTexture(
            m_context, m_offscreenRenderPass.get(), bufferSize, colorFormat);
        if (!m_offscreenFramebuffer) {
            return;
        }
    }

    // Lazy-init overlay render pass (LOAD_OP_LOAD so existing swapchain content is preserved)
    if (!m_overlayRenderPass) {
        VulkanRenderPass::Config cfg;
        cfg.colorFormat = colorFormat;
        cfg.colorLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        cfg.colorStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        // The workspace render pass leaves the image in PRESENT_SRC_KHR.
        // The overlay render pass loads from that layout and returns it to PRESENT_SRC_KHR
        // so present() can proceed without an additional layout transition.
        cfg.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        cfg.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        m_overlayRenderPass = VulkanRenderPass::create(m_context, cfg);
        if (!m_overlayRenderPass) {
            return;
        }
    }

    // Lazy-init white texture for filling unused descriptor sampler slots
    if (!m_defaultWhiteTexture) {
        QImage whiteImg(1, 1, QImage::Format_ARGB32_Premultiplied);
        whiteImg.fill(Qt::white);
        m_defaultWhiteTexture = VulkanTexture::upload(m_context, whiteImg);
        if (!m_defaultWhiteTexture) {
            return;
        }
    }

    // Lazy-init UBO (full opacity, full brightness — set once, reused every frame)
    if (!m_uniformBuffer) {
        m_uniformBuffer = VulkanBuffer::createUniformBuffer(m_context, sizeof(VulkanUniforms));
        if (!m_uniformBuffer) {
            return;
        }
        VulkanUniforms u{};
        u.uniformColor[0] = u.uniformColor[1] = u.uniformColor[2] = u.uniformColor[3] = 1.0f;
        u.opacity = 1.0f;
        u.brightness = 1.0f;
        u.saturation = 1.0f;
        m_uniformBuffer->upload(&u, sizeof(u));
    }

    // Lazy-init vertex buffer (6 vertices for a full-quad, updated each frame)
    if (!m_vertexBuffer) {
        m_vertexBuffer = VulkanBuffer::createStreamingBuffer(m_context, 6 * sizeof(VulkanVertex2D));
        if (!m_vertexBuffer) {
            return;
        }
    }

    // Render the cursor scene to the offscreen framebuffer.
    // The scene's ItemRendererVulkan submits the command buffer non-blockingly.
    VulkanRenderTarget offscreenVkTarget(m_offscreenFramebuffer.get());
    RenderTarget offscreenRenderTarget(&offscreenVkTarget);

    RenderLayer renderLayer(layer()->loop());
    renderLayer.setDelegate(std::make_unique<SceneDelegate>(cursorScene, m_output));
    renderLayer.delegate()->prePaint();
    renderLayer.delegate()->paint(offscreenRenderTarget, infiniteRegion());
    renderLayer.delegate()->postPaint();

    // Write quad vertices covering the cursor's local extent [0,w]×[0,h].
    // The MVP translates these into the correct screen position.
    const float w = static_cast<float>(bufferSize.width());
    const float h = static_cast<float>(bufferSize.height());
    const VulkanVertex2D vertices[6] = {
        {{0.0f, 0.0f}, {0.0f, 0.0f}},
        {{w, 0.0f}, {1.0f, 0.0f}},
        {{0.0f, h}, {0.0f, 1.0f}},
        {{0.0f, h}, {0.0f, 1.0f}},
        {{w, 0.0f}, {1.0f, 0.0f}},
        {{w, h}, {1.0f, 1.0f}},
    };
    m_vertexBuffer->upload(vertices, sizeof(vertices));

    // Record and submit the overlay command buffer.
    // endSingleTimeCommands() now waits on this submission's own fence
    // (Phase 0) — no queue drain. The caller can call present() afterwards
    // knowing the overlay is complete.
    VkCommandBuffer cmd = m_context->beginSingleTimeCommands();

    // Cross-submit visibility: the workspace and cursor-scene command
    // buffers were submitted on this queue before us, and Vulkan submission
    // ordering only guarantees execution order on a queue — memory
    // visibility between submits has to be explicit. One global barrier
    // covers both producers:
    //   * workspace render pass wrote the swapchain image (COLOR_ATTACHMENT
    //     _OUTPUT / COLOR_ATTACHMENT_WRITE), and the overlay render pass
    //     loads from it (COLOR_ATTACHMENT_OUTPUT / COLOR_ATTACHMENT_READ
    //     via loadOp=LOAD).
    //   * cursor-scene render pass wrote the cursor texture (same producer
    //     stage + access), and the overlay's fragment shader samples it
    //     (FRAGMENT_SHADER / SHADER_READ).
    // Replacing the prior vkQueueWaitIdle, this is a cheap GPU-side
    // dependency that lets both producers run concurrently with whatever
    // the queue picks up after them.
    VkMemoryBarrier crossSubmitBarrier{};
    crossSubmitBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    crossSubmitBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    crossSubmitBarrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
        | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
        | VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                             | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0,
                         1, &crossSubmitBarrier,
                         0, nullptr,
                         0, nullptr);

    const QSize mainSize = mainFb->size();
    const VkRect2D renderArea{{0, 0}, {uint32_t(mainSize.width()), uint32_t(mainSize.height())}};
    VkClearValue clearValue{};
    m_overlayRenderPass->begin(cmd, mainFb->framebuffer(), renderArea, &clearValue, 1);

    // Y-flip viewport (VK_KHR_maintenance1 negative-height trick matching ItemRendererVulkan)
    VkViewport vkViewport{};
    vkViewport.x = 0.0f;
    vkViewport.y = static_cast<float>(mainSize.height());
    vkViewport.width = static_cast<float>(mainSize.width());
    vkViewport.height = -static_cast<float>(mainSize.height());
    vkViewport.minDepth = 0.0f;
    vkViewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vkViewport);

    const VkRect2D scissor{{0, 0}, {uint32_t(mainSize.width()), uint32_t(mainSize.height())}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // The pipeline was compiled for the swapchain render pass; the overlay render pass is
    // compatible (same format, attachment count, sample count) per Vulkan spec §8.2.
    VulkanPipeline *pipeline = m_context->pipelineManager()->pipeline(
        VulkanShaderTrait::MapTexture | VulkanShaderTrait::Modulate);
    if (!pipeline || !pipeline->isValid()) {
        m_overlayRenderPass->end(cmd);
        m_context->endSingleTimeCommands(cmd);
        return;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline());

    // Projection matrix: match createProjectionMatrix() convention used by ItemRendererVulkan.
    // The double scale(1,-1) + ortho combination works correctly with the Y-flip viewport.
    QMatrix4x4 mvp;
    mvp.scale(1, -1);
    mvp *= renderTarget.transform().toMatrix();
    mvp.scale(1, -1);
    mvp.ortho(QRectF(QPointF(0, 0), m_output->transform().map(QSizeF(mainSize))));
    mvp.translate(cursorRect.x(), cursorRect.y());

    VulkanPushConstants pc{};
    memcpy(pc.mvp, mvp.data(), sizeof(pc.mvp));
    pc.textureMatrix[0] = pc.textureMatrix[5] = pc.textureMatrix[10] = pc.textureMatrix[15] = 1.0f;
    vkCmdPushConstants(cmd, pipeline->layout(),
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(VulkanPushConstants), &pc);

    VulkanTexture *cursorTex = m_offscreenFramebuffer->colorTexture();
    VulkanTexture *whiteTex = m_defaultWhiteTexture.get();

    // Descriptor binding 0: 4-slot sampler array (slot 0 = cursor, rest = white fallback)
    std::array<VkDescriptorImageInfo, 4> imageInfos{};
    for (auto &info : imageInfos) {
        info.sampler = whiteTex->sampler();
        info.imageView = whiteTex->imageView();
        info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    imageInfos[0].sampler = cursorTex->sampler();
    imageInfos[0].imageView = cursorTex->imageView();
    imageInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // Descriptor binding 1: UBO
    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = m_uniformBuffer->buffer();
    bufferInfo.offset = 0;
    bufferInfo.range = sizeof(VulkanUniforms);

    std::array<VkWriteDescriptorSet, 2> writes{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].descriptorCount = 4;
    writes[0].pImageInfo = imageInfos.data();

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[1].descriptorCount = 1;
    writes[1].pBufferInfo = &bufferInfo;

    if (!m_context->bindDescriptors(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    pipeline->layout(), pipeline->descriptorSetLayout(),
                                    0, writes.size(), writes.data())) {
        m_overlayRenderPass->end(cmd);
        m_context->endSingleTimeCommands(cmd);
        return;
    }

    const VkBuffer vb = m_vertexBuffer->buffer();
    const VkDeviceSize vbOffset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &vbOffset);
    vkCmdDraw(cmd, 6, 1, 0, 0);

    m_overlayRenderPass->end(cmd);
    m_context->endSingleTimeCommands(cmd);
}

} // namespace KWin

#include "moc_cursordelegate_vulkan.cpp"
