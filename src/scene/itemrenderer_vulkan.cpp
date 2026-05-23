/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2025 Joseph <author@example.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "itemrenderer_vulkan.h"
#include "core/pixelgrid.h"
#include "core/rendertarget.h"
#include "core/renderviewport.h"
#include "effect/effect.h"
#include "effect/globals.h"
#include "platformsupport/scenes/vulkan/vulkanbackend.h"
#include "platformsupport/scenes/vulkan/vulkanbuffer.h"
#include "platformsupport/scenes/vulkan/vulkancontext.h"
#include "platformsupport/scenes/vulkan/vulkanframebuffer.h"
#include "platformsupport/scenes/vulkan/vulkanpipeline.h"
#include "platformsupport/scenes/vulkan/vulkanpipelinemanager.h"
#include "platformsupport/scenes/vulkan/vulkanrenderpass.h"
#include "platformsupport/scenes/vulkan/vulkanrendertarget.h"
#include "platformsupport/scenes/vulkan/vulkansurfacetexture.h"
#include "platformsupport/scenes/vulkan/vulkansurfacetexture_x11.h"
#include "platformsupport/scenes/vulkan/vulkanswapchain.h"
#include "platformsupport/scenes/vulkan/vulkantexture.h"
#include "scene/decorationitem.h"
#include "scene/imageitem.h"
#include "scene/item.h"
#include "scene/itemgeometry.h"

#include "scene/outlinedborderitem.h"
#include "scene/rootitem.h"
#include "scene/shadowitem.h"
#include "scene/surfaceitem.h"
#include "scene/windowitem.h"
#include "scene/workspacescene_vulkan.h"
#include "utils/common.h"
#include "window.h"
#include "workspace.h"
#include <algorithm>
#include <array>
#include <vector>

#include <cstring>
#include <typeinfo>

namespace KWin
{

namespace
{
// Tracks recursive paint-screen / paint-window flows on this thread. Incremented
// by ItemRendererVulkan::RecursivePaintScope. Effects that recurse into the paint
// chain with their own offscreen RenderTarget must enclose the recursion in such
// a scope so currentCommandBuffer() can flag misuse — see the header doc.
thread_local int t_recursivePaintDepth = 0;
}

ItemRendererVulkan::RecursivePaintScope::RecursivePaintScope()
{
    ++t_recursivePaintDepth;
}

ItemRendererVulkan::RecursivePaintScope::~RecursivePaintScope()
{
    Q_ASSERT(t_recursivePaintDepth > 0);
    --t_recursivePaintDepth;
}

VkCommandBuffer ItemRendererVulkan::currentCommandBuffer() const
{
    // Catch effects that grab the swapchain command buffer while a parent
    // effect has handed us an offscreen RenderTarget. Use activeCommandBuffer()
    // when participating in paintScreen / drawWindow so the draw lands in the
    // caller's render pass instead of the swapchain one. See the bug fixed
    // for ZoomEffect+InvertEffect (silent: only the cursor survived because
    // the inverted window quads went to the wrong cmd buffer).
    Q_ASSERT_X(t_recursivePaintDepth == 0,
               "ItemRendererVulkan::currentCommandBuffer",
               "called inside a recursive paint flow; use activeCommandBuffer(renderTarget) instead");
    return m_currentCommandBuffer;
}

VkCommandBuffer ItemRendererVulkan::activeCommandBuffer(const RenderTarget &renderTarget) const
{
    if (const auto *vt = renderTarget.vulkanTarget(); vt && vt->commandBuffer() != VK_NULL_HANDLE) {
        return vt->commandBuffer();
    }
    return m_currentCommandBuffer;
}

void ItemRendererVulkan::pushOffscreenSlot()
{
    m_offscreenSlotStack.push_back({m_currentFrameIndex, m_vertexBufferOffset, m_uniformDrawIndex});
    m_currentFrameIndex = kOffscreenSlot;
    // Resume where the last offscreen render in this frame left off, so
    // independent async offscreen submissions pack into the same slot
    // without aliasing each other's still-in-flight vertex/uniform data.
    m_vertexBufferOffset = m_offscreenVertexOffset;
    m_uniformDrawIndex = m_offscreenUniformDrawIndex;
}

void ItemRendererVulkan::popOffscreenSlot()
{
    Q_ASSERT(!m_offscreenSlotStack.empty());
    if (m_offscreenSlotStack.empty()) {
        return;
    }
    // Persist the offscreen-slot cursors so a subsequent pushOffscreenSlot
    // in the same frame appends instead of overwriting.
    m_offscreenVertexOffset = m_vertexBufferOffset;
    m_offscreenUniformDrawIndex = m_uniformDrawIndex;
    const auto save = m_offscreenSlotStack.back();
    m_offscreenSlotStack.pop_back();
    m_currentFrameIndex = save.frameIndex;
    m_vertexBufferOffset = save.vertexOffset;
    m_uniformDrawIndex = save.uniformDrawIndex;
}

VkRenderPass ItemRendererVulkan::activeRenderPass(const RenderTarget &renderTarget) const
{
    // Mirror activeCommandBuffer's routing so a recursive offscreen caller
    // gets the render pass that matches its own framebuffer, not the
    // swapchain's. Pipelines are looked up by (traits, renderPass) and a
    // mismatch here would silently violate render-pass compatibility.
    if (const auto *vt = renderTarget.vulkanTarget()) {
        if (auto *fb = vt->framebuffer(); fb && fb->renderPass()) {
            return fb->renderPass()->renderPass();
        }
    }
    if (m_currentFramebuffer && m_currentFramebuffer->renderPass()) {
        return m_currentFramebuffer->renderPass()->renderPass();
    }
    return VK_NULL_HANDLE;
}

ItemRendererVulkan::ItemRendererVulkan(VulkanBackend *backend)
    : m_backend(backend)
    , m_context(backend->vulkanContext())
{
    // The frame slot index handed to us via VulkanSyncInfo comes straight from
    // the swapchain's currentFrame(), so our per-slot buffer count must cover
    // every swapchain frame-in-flight.
    static_assert(kFramesInFlight == VulkanSwapchain::MAX_FRAMES_IN_FLIGHT,
                  "kFramesInFlight must match the swapchain's MAX_FRAMES_IN_FLIGHT");

    // Create a streaming vertex buffer and a uniform buffer per frame slot, so a
    // frame never writes over a buffer region a previous in-flight frame's GPU
    // work is still reading. See FrameResources in the header.
    for (FrameResources &fr : m_frameResources) {
        fr.streamingBuffer = VulkanBuffer::createStreamingBuffer(m_context, 4 * 1024 * 1024);
        fr.uniformBuffer = VulkanBuffer::createUniformBuffer(
            m_context,
            sizeof(VulkanUniforms) * 1024); // Support up to 1024 draws per frame
    }

    // Create default 1x1 white texture for non-textured draws
    QImage whiteImage(1, 1, QImage::Format_ARGB32_Premultiplied);
    whiteImage.fill(Qt::white);
    m_defaultWhiteTexture = VulkanTexture::upload(m_context, whiteImage);
    if (!m_defaultWhiteTexture || !m_defaultWhiteTexture->isValid()) {
        qCWarning(KWIN_VULKAN) << "Failed to create default white texture";
    } else {
        qCDebug(KWIN_VULKAN) << "Created default 1x1 white texture";
    }
}

ItemRendererVulkan::~ItemRendererVulkan()
{
    if (m_context) {
        m_context->makeCurrent();
    }
    if (m_prevCommandBuffer != VK_NULL_HANDLE) {
        m_context->freeCommandBuffer(m_prevCommandBuffer);
    }
    if (m_currentCommandBuffer != VK_NULL_HANDLE) {
        m_context->freeCommandBuffer(m_currentCommandBuffer);
    }
    // Release the per-frame buffers while the context (and its VMA allocator /
    // deferred-destruction queue) is still alive.
    for (FrameResources &fr : m_frameResources) {
        fr.streamingBuffer.reset();
        fr.uniformBuffer.reset();
    }
}

std::unique_ptr<ImageItem> ItemRendererVulkan::createImageItem(Item *parent)
{
    return std::make_unique<ImageItemVulkan>(parent);
}

void ItemRendererVulkan::beginFrame(const RenderTarget &renderTarget, const RenderViewport &viewport, const QRegion &damage)
{
    if (!m_context->makeCurrent()) {
        qCWarning(KWIN_CORE) << "Failed to make Vulkan context current";
        return;
    }

    // Free the previous frame's command buffer.
    // doBeginFrame() calls swapchain->waitForFence() before invoking beginFrame(), so the
    // GPU is guaranteed to have finished using m_prevCommandBuffer by the time we get here.
    if (m_prevCommandBuffer != VK_NULL_HANDLE) {
        m_context->freeCommandBuffer(m_prevCommandBuffer);
        m_prevCommandBuffer = VK_NULL_HANDLE;
    }

    // Clean up any pending resources from previous frames (samplers that were in use)
    m_context->cleanupPendingResources();

    // Reset descriptor pool when all previous frames have completed
    // This prevents pool exhaustion over time while avoiding GPU stalls
    if (m_outputsInFlight == 0) {
        m_context->resetDescriptorPool();
    }

    m_outputsInFlight++;
    m_frameNumber++;

    // Periodic VMA memory dump for leak diagnosis. Flag-agnostic (independent of
    // KWIN_VULKAN_PARTIAL_REPAINT) so usage can be compared between modes.
    // Enable with KWIN_VULKAN_VMA_STATS=1.
    static const bool s_vmaStats = qEnvironmentVariableIsSet("KWIN_VULKAN_VMA_STATS");
    if (s_vmaStats && (m_frameNumber == 1 || m_frameNumber % 300 == 0)) {
        VulkanAllocator::logStatistics("frame");
    }

    // Reset vertex buffer offset for new frame - all items will upload vertices sequentially
    m_vertexBufferOffset = 0;
    // Same for the offscreen slot's accumulating cursors: independent of the
    // swapchain frame's offset, they only need to last across the offscreen
    // submissions a single frame may queue up before being recycled.
    m_offscreenVertexOffset = 0;
    m_offscreenUniformDrawIndex = 0;
    m_offscreenSlotStack.clear();

    // Clear descriptor sets from previous frame (should be empty after endFrame cleanup)
    m_frameDescriptorSets.clear();

    // Allocate command buffer for this frame
    m_currentCommandBuffer = m_context->allocateCommandBuffer();
    if (m_currentCommandBuffer == VK_NULL_HANDLE) {
        qCWarning(KWIN_CORE) << "Failed to allocate command buffer";
        return;
    }

    // Begin command buffer recording
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    if (vkBeginCommandBuffer(m_currentCommandBuffer, &beginInfo) != VK_SUCCESS) {
        qCWarning(KWIN_CORE) << "Failed to begin command buffer recording";
        return;
    }

    // Get the framebuffer from render target
    // Extract the VulkanRenderTarget from the RenderTarget
    const auto vulkanRenderTarget = renderTarget.vulkanTarget();
    if (vulkanRenderTarget) {
        m_currentFramebuffer = vulkanRenderTarget->framebuffer();
        // Extract sync info for GPU-GPU synchronization
        if (vulkanRenderTarget->hasSyncInfo()) {
            m_currentSyncInfo = vulkanRenderTarget->syncInfo();
            // Swapchain frame: write into the buffer copy owned by this
            // frame-in-flight slot. doBeginFrame() has already waited the
            // matching fence, so the slot's previous user (this slot's frame
            // from kFramesInFlight ago) is guaranteed done with its buffers.
            m_currentFrameIndex = m_currentSyncInfo.frameIndex % kFramesInFlight;
        } else {
            m_currentSyncInfo = VulkanSyncInfo{}; // Reset to defaults
            // Offscreen / non-swapchain target: use the dedicated slot so it
            // never aliases a swapchain frame's in-flight buffers.
            m_currentFrameIndex = kOffscreenSlot;
        }
    }

    // Per-draw uniform cursor restarts each frame; the buffer it indexes is now
    // private to this frame slot.
    m_uniformDrawIndex = 0;

    // Use the viewport's projection matrix which includes render target transform
    // This is important for multi-monitor setups with different orientations
    m_currentProjection = viewport.projectionMatrix();
    const QSize size = viewport.renderRect().size().toSize();

    // Decide between a full repaint and a damage-driven partial repaint.
    //
    // Partial repaint is only safe on a swapchain image (it carries sync info and
    // is re-acquired in PRESENT_SRC_KHR with its previous contents intact), and is
    // skipped when fullscreen post-passes are active (those re-process the whole
    // screen). The decision uses the actual damage *region*, not its bounding box:
    // a frame is a full clearing repaint only when the damage genuinely covers
    // every pixel. Disjoint damage whose bounding box happens to span the screen
    // must still be a partial LOAD frame — a full clear would blank the gaps
    // between the damage rects (the "background flickers to black" bug). A finite
    // damage region also implies the swapchain image was rendered before
    // (doBeginFrame() only returns a finite buffer-age repaint for an
    // already-rendered image), so the LOAD path's preserved contents are valid.
    const QSize fbSize = m_currentFramebuffer ? m_currentFramebuffer->size() : size;
    const VkRect2D fullArea{{0, 0}, {static_cast<uint32_t>(fbSize.width()), static_cast<uint32_t>(fbSize.height())}};
    const bool swapchainTarget = vulkanRenderTarget && vulkanRenderTarget->hasSyncInfo();

    m_partialFrame = false;
    m_frameRenderArea = fullArea;
    if (swapchainTarget && m_currentFramebuffer && !hasFullscreenPostPasses()
        && damage != infiniteRegion()) {
        const QRegion fbRegion(QRect(QPoint(0, 0), fbSize));
        // Damage mapped to device pixels and clamped to the framebuffer.
        const QRegion devRegion = damage.isEmpty()
            ? QRegion()
            : (viewport.mapToRenderTarget(damage) & fbRegion);
        if (devRegion != fbRegion) {
            // Sub-screen damage (including disjoint or empty) — partial LOAD frame.
            m_partialFrame = true;
            const QRect devBounds = devRegion.boundingRect();
            if (devBounds.isEmpty()) {
                // Minimal valid render area; no draws are expected to land in it.
                m_frameRenderArea = VkRect2D{{0, 0}, {1, 1}};
            } else {
                m_frameRenderArea = VkRect2D{
                    {devBounds.x(), devBounds.y()},
                    {static_cast<uint32_t>(devBounds.width()), static_cast<uint32_t>(devBounds.height())}};
            }
        }
    }

    // Begin render pass if we have a framebuffer
    if (m_currentFramebuffer) {
        if (m_partialFrame) {
            // Lazily (re)create the LOAD-variant render pass for this color format.
            const VkFormat fmt = m_currentFramebuffer->renderPass()->config().colorFormat;
            if (!m_loadRenderPass || m_loadRenderPassFormat != fmt) {
                m_loadRenderPass = VulkanRenderPass::createForSwapchainLoad(m_context, fmt);
                m_loadRenderPassFormat = fmt;
            }
            if (m_loadRenderPass) {
                // No clear: loadOp=LOAD keeps the previously-presented contents;
                // only m_frameRenderArea is touched, so undamaged pixels survive.
                m_loadRenderPass->begin(m_currentCommandBuffer, m_currentFramebuffer->framebuffer(),
                                        m_frameRenderArea, nullptr, 0);
            } else {
                // LOAD pass unavailable — fall back to a full clearing repaint.
                qCWarning(KWIN_CORE) << "Partial repaint: LOAD render pass unavailable, falling back to full repaint";
                m_partialFrame = false;
                m_frameRenderArea = fullArea;
            }
        }
        if (!m_partialFrame) {
            // Full repaint: the presentation render pass clears the framebuffer.
            std::array<VkClearValue, 2> clearValues{};
            clearValues[0].color = {{0.0f, 0.0f, 0.0f, 0.0f}}; // Clear to transparent black
            if (m_currentFramebuffer->renderPass()->config().hasDepth) {
                clearValues[1].depthStencil = {1.0f, 0};
            }

            m_currentFramebuffer->beginRenderPass(m_currentCommandBuffer, clearValues.data(),
                                                  m_currentFramebuffer->renderPass()->config().hasDepth ? 2 : 1);
        }
    }

    // Set viewport with Y-flip to match OpenGL coordinate conventions
    // Vulkan has Y pointing down, but our ortho projection expects Y=0 at top
    // Using negative height (VK_KHR_maintenance1) flips the Y axis
    VkViewport vkViewport{};
    vkViewport.x = 0.0f;
    vkViewport.y = static_cast<float>(size.height()); // Start at bottom
    vkViewport.width = static_cast<float>(size.width());
    vkViewport.height = -static_cast<float>(size.height()); // Negative flips Y
    vkViewport.minDepth = 0.0f;
    vkViewport.maxDepth = 1.0f;
    vkCmdSetViewport(m_currentCommandBuffer, 0, 1, &vkViewport);

    // Scissor everything to the frame's render area: the full framebuffer for a
    // full repaint, or the damage bounding box for a partial one.
    vkCmdSetScissor(m_currentCommandBuffer, 0, 1, &m_frameRenderArea);
}

void ItemRendererVulkan::registerPostPassCopy(VkImage srcImage, VkImageLayout srcLayoutAtPassEnd,
                                              VkFormat srcFormat, const VkOffset3D &offset,
                                              const VkExtent3D &extent, PostPassCopyCallback callback)
{
    if (srcImage == VK_NULL_HANDLE || extent.width == 0 || extent.height == 0) {
        if (callback) {
            callback(QImage());
        }
        return;
    }
    PostPassCopyRequest req;
    req.srcImage = srcImage;
    req.srcLayoutAtPassEnd = srcLayoutAtPassEnd;
    req.srcFormat = srcFormat;
    req.offset = offset;
    req.extent = extent;
    req.callback = std::move(callback);
    m_pendingPostPassCopies.push_back(std::move(req));
}

int ItemRendererVulkan::registerFullscreenPostPass(FullscreenPostPassCallback callback)
{
    if (!callback) {
        return 0;
    }
    const int id = m_nextPostPassId++;
    m_fullscreenPostPasses.push_back({id, std::move(callback)});
    return id;
}

void ItemRendererVulkan::unregisterFullscreenPostPass(int id)
{
    auto it = std::find_if(m_fullscreenPostPasses.begin(), m_fullscreenPostPasses.end(),
                           [id](const FullscreenPostPassRegistration &r) {
        return r.id == id;
    });
    if (it != m_fullscreenPostPasses.end()) {
        m_fullscreenPostPasses.erase(it);
    }
}

void ItemRendererVulkan::runFullscreenPostPasses(const RenderTarget &renderTarget, const RenderViewport &viewport)
{
    if (m_fullscreenPostPasses.empty() || m_currentCommandBuffer == VK_NULL_HANDLE || !m_currentFramebuffer) {
        return;
    }
    // Only operates on the main swapchain target. Recursive/offscreen flows (zoom,
    // screenshot, ...) skip this — their content doesn't need invert-style post-FX,
    // and they own their own command buffer / framebuffer.
    auto *vkTarget = renderTarget.vulkanTarget();
    if (!vkTarget || vkTarget->commandBuffer() != VK_NULL_HANDLE) {
        return;
    }

    VkImage swapchainImage = m_currentFramebuffer->colorImage();
    const VkFormat swapchainFormat = m_currentFramebuffer->renderPass()->config().colorFormat;
    const QSize fbSize = m_currentFramebuffer->size();
    if (swapchainImage == VK_NULL_HANDLE || swapchainFormat == VK_FORMAT_UNDEFINED || fbSize.isEmpty()) {
        return;
    }

    // Lazy: (re)allocate scene-capture texture when the swapchain size/format changes.
    if (!m_sceneCaptureTexture
        || m_sceneCaptureTexture->size() != fbSize
        || m_sceneCaptureTexture->format() != swapchainFormat) {
        m_sceneCaptureTexture = VulkanTexture::allocate(m_context, fbSize, swapchainFormat);
        if (!m_sceneCaptureTexture) {
            qCWarning(KWIN_CORE) << "runFullscreenPostPasses: failed to allocate scene capture texture";
            return;
        }
        m_sceneCaptureTexture->setFilter(VK_FILTER_NEAREST);
        m_sceneCaptureTexture->setWrapMode(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    }
    // Lazy: (re)create post-FX render pass when the swapchain format changes.
    if (!m_postFxRenderPass || m_postFxRenderPassFormat != swapchainFormat) {
        m_postFxRenderPass = VulkanRenderPass::createForSwapchainPostFx(m_context, swapchainFormat);
        m_postFxRenderPassFormat = swapchainFormat;
        if (!m_postFxRenderPass) {
            qCWarning(KWIN_CORE) << "runFullscreenPostPasses: failed to create post-FX render pass";
            return;
        }
    }

    // End the main scene pass; swapchain is now in PRESENT_SRC_KHR (its finalLayout).
    m_currentFramebuffer->endRenderPass(m_currentCommandBuffer);

    const VkRect2D renderArea{{0, 0}, {uint32_t(fbSize.width()), uint32_t(fbSize.height())}};

    for (auto &reg : m_fullscreenPostPasses) {
        // 1) swapchain: PRESENT_SRC_KHR (or COLOR_ATTACHMENT, after a previous post-pass) -> TRANSFER_SRC
        {
            VkImageMemoryBarrier b{};
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = swapchainImage;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(m_currentCommandBuffer,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &b);
        }

        // 2) capture texture: current (UNDEFINED first time, SHADER_READ_ONLY thereafter) -> TRANSFER_DST
        const VkImageLayout captureOld = m_sceneCaptureTexture->currentLayout();
        m_sceneCaptureTexture->transitionLayout(m_currentCommandBuffer,
                                                captureOld,
                                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                captureOld == VK_IMAGE_LAYOUT_UNDEFINED
                                                    ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                                    : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                                VK_PIPELINE_STAGE_TRANSFER_BIT);

        // 3) blit swapchain -> capture (1:1, no scaling)
        {
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[0] = {0, 0, 0};
            blit.srcOffsets[1] = {fbSize.width(), fbSize.height(), 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.dstOffsets[0] = {0, 0, 0};
            blit.dstOffsets[1] = {fbSize.width(), fbSize.height(), 1};
            vkCmdBlitImage(m_currentCommandBuffer,
                           swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           m_sceneCaptureTexture->image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &blit, VK_FILTER_NEAREST);
        }

        // 4) capture: TRANSFER_DST -> SHADER_READ_ONLY (the post-pass samples it)
        m_sceneCaptureTexture->transitionLayout(m_currentCommandBuffer,
                                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                VK_PIPELINE_STAGE_TRANSFER_BIT,
                                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

        // 5) swapchain: TRANSFER_SRC -> COLOR_ATTACHMENT_OPTIMAL. The post-FX render
        //    pass has initialLayout=UNDEFINED + loadOp=DONT_CARE so the layout is
        //    semantically not preserved; the explicit barrier provides the required
        //    transfer→color-attachment-output synchronization.
        {
            VkImageMemoryBarrier b{};
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = swapchainImage;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            vkCmdPipelineBarrier(m_currentCommandBuffer,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &b);
        }

        // 6) begin post-FX pass on the same swapchain framebuffer. loadOp=DONT_CARE
        //    so the callback MUST cover every pixel via a fullscreen quad.
        VkClearValue clear{};
        m_postFxRenderPass->begin(m_currentCommandBuffer,
                                  m_currentFramebuffer->framebuffer(),
                                  renderArea, &clear, 1);

        // Y-flipped (negative-height) viewport, matching beginFrame(). The scene
        // — and therefore the captured texture — was rendered with this
        // convention, and post-pass callbacks build geometry with
        // viewport.projectionMatrix(). A positive-height viewport here would
        // flip the post-processed output upside down.
        VkViewport vkViewport{};
        vkViewport.x = 0.0f;
        vkViewport.y = float(fbSize.height());
        vkViewport.width = float(fbSize.width());
        vkViewport.height = -float(fbSize.height());
        vkViewport.minDepth = 0.0f;
        vkViewport.maxDepth = 1.0f;
        vkCmdSetViewport(m_currentCommandBuffer, 0, 1, &vkViewport);
        vkCmdSetScissor(m_currentCommandBuffer, 0, 1, &renderArea);

        // 7) callback: pipeline bind + descriptor + fullscreen draw.
        reg.callback(m_currentCommandBuffer, m_sceneCaptureTexture.get(), renderTarget, viewport);

        // 8) end pass: swapchain auto-transitions back to PRESENT_SRC_KHR.
        m_postFxRenderPass->end(m_currentCommandBuffer);
    }

    // The main scene render pass is already ended; tell endFrame() not to end it again.
    m_currentFramebuffer = nullptr;
}

void ItemRendererVulkan::runFullscreenPostPassesOffscreen(VkCommandBuffer cmd, VulkanFramebuffer *targetFb,
                                                          const RenderTarget &renderTarget)
{
    if (m_fullscreenPostPasses.empty() || cmd == VK_NULL_HANDLE || !targetFb) {
        return;
    }
    VulkanTexture *colorTex = targetFb->colorTexture();
    VulkanRenderPass *renderPass = targetFb->renderPass();
    if (!colorTex || !renderPass) {
        return;
    }
    const QSize fbSize = targetFb->size();
    const VkFormat fmt = colorTex->format();
    if (fbSize.isEmpty() || fmt == VK_FORMAT_UNDEFINED) {
        return;
    }

    // Capture texture: a fresh allocation — offscreen captures (window screenshots)
    // are rare and one-shot. VulkanTexture destruction is deferred, so the local
    // staying alive only until this function returns is safe: the caller blocks on
    // the GPU (endSingleTimeCommands) before the deferred-destruction queue drains.
    auto capture = VulkanTexture::allocate(m_context, fbSize, fmt);
    if (!capture) {
        qCWarning(KWIN_CORE) << "runFullscreenPostPassesOffscreen: capture allocation failed";
        return;
    }
    capture->setFilter(VK_FILTER_NEAREST);
    capture->setWrapMode(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);

    // The caller's render pass has ended; its color image is in SHADER_READ_ONLY.
    colorTex->transitionLayout(cmd, colorTex->currentLayout(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    capture->transitionLayout(cmd, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[0] = {0, 0, 0};
    blit.srcOffsets[1] = {fbSize.width(), fbSize.height(), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[0] = {0, 0, 0};
    blit.dstOffsets[1] = {fbSize.width(), fbSize.height(), 1};
    vkCmdBlitImage(cmd, colorTex->image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   capture->image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &blit, VK_FILTER_NEAREST);

    capture->transitionLayout(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    // Synchronize the transfer read against the upcoming color-attachment writes.
    colorTex->transitionLayout(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    // The caller's viewport is anchored at the captured window's on-screen
    // position, so its projection maps FB-local pixel (0,0) somewhere off the
    // framebuffer. Build a viewport whose projection maps framebuffer-local
    // device pixels (0,0)..(fbSize) instead, matching how the post-FX callbacks
    // build their fullscreen quad. Without this the quad lands off-framebuffer
    // and the screenshot comes out fully transparent.
    const RenderViewport localViewport(QRectF(0, 0, fbSize.width(), fbSize.height()), 1.0, renderTarget);

    // Reuse the framebuffer's own render pass. Its loadOp is CLEAR; the post-FX
    // callbacks cover every pixel with a fullscreen quad so the clear is harmless.
    // Its finalLayout is SHADER_READ_ONLY, which is what the caller reads back.
    const VkRect2D renderArea{{0, 0}, {uint32_t(fbSize.width()), uint32_t(fbSize.height())}};
    VkClearValue clear{};
    renderPass->begin(cmd, targetFb->framebuffer(), renderArea, &clear, 1);

    // Y-flipped viewport, matching beginFrame() and the screenshot effect's own
    // offscreen render — see runFullscreenPostPasses().
    VkViewport vkViewport{};
    vkViewport.x = 0.0f;
    vkViewport.y = float(fbSize.height());
    vkViewport.width = float(fbSize.width());
    vkViewport.height = -float(fbSize.height());
    vkViewport.minDepth = 0.0f;
    vkViewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vkViewport);
    vkCmdSetScissor(cmd, 0, 1, &renderArea);

    for (auto &reg : m_fullscreenPostPasses) {
        reg.callback(cmd, capture.get(), renderTarget, localViewport);
    }

    renderPass->end(cmd);
    // createForOffscreen's finalLayout is SHADER_READ_ONLY_OPTIMAL.
    colorTex->setCurrentLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

static QImage::Format vulkanFormatToQImageFormat(VkFormat fmt)
{
    switch (fmt) {
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        return QImage::Format_ARGB32_Premultiplied;
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
        return QImage::Format_RGBA8888_Premultiplied;
    default:
        return QImage::Format_Invalid;
    }
}

// Caller must guarantee the GPU has finished executing the post-pass copies
// (typically via vkWaitForFences on the submission's fence) before invoking this.
static void drainPostPassCopies(auto &requests)
{
    for (auto &req : requests) {
        if (!req.callback) {
            continue;
        }
        QImage img;
        if (req.staging) {
            const QImage::Format qfmt = vulkanFormatToQImageFormat(req.srcFormat);
            if (qfmt == QImage::Format_Invalid) {
                qCWarning(KWIN_CORE) << "post-pass copy: unsupported format" << req.srcFormat;
            } else {
                const void *mapped = req.staging->map();
                if (mapped) {
                    QImage tmp(int(req.extent.width), int(req.extent.height), qfmt);
                    std::memcpy(tmp.bits(), mapped, VkDeviceSize(req.extent.width) * req.extent.height * 4);
                    req.staging->unmap();
                    img = tmp;
                }
            }
        }
        req.callback(img);
    }
    requests.clear();
}

void ItemRendererVulkan::endFrame()
{
    if (m_currentCommandBuffer == VK_NULL_HANDLE) {
        qCWarning(KWIN_CORE) << "endFrame called with null command buffer";
        return;
    }

    // End render pass if we were rendering to a framebuffer
    if (m_currentFramebuffer) {
        m_currentFramebuffer->endRenderPass(m_currentCommandBuffer);
    }

    // Record post-pass copies into the main command buffer. Allocates staging buffers,
    // transitions each source image into TRANSFER_SRC_OPTIMAL, copies, transitions back.
    // The actual readback happens after the queue submit completes (drainPostPassCopies).
    if (!m_pendingPostPassCopies.empty()) {
        for (auto &req : m_pendingPostPassCopies) {
            const VkDeviceSize size = VkDeviceSize(req.extent.width) * req.extent.height * 4;
            req.staging = VulkanBuffer::createStagingBuffer(m_context, size);
            if (!req.staging) {
                qCWarning(KWIN_CORE) << "post-pass copy: staging buffer alloc failed";
                continue;
            }

            VkImageMemoryBarrier toTransfer{};
            toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            toTransfer.oldLayout = req.srcLayoutAtPassEnd;
            toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransfer.image = req.srcImage;
            toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            toTransfer.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(m_currentCommandBuffer,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &toTransfer);

            VkBufferImageCopy copy{};
            copy.bufferOffset = 0;
            copy.bufferRowLength = 0;
            copy.bufferImageHeight = 0;
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageOffset = req.offset;
            copy.imageExtent = req.extent;
            vkCmdCopyImageToBuffer(m_currentCommandBuffer, req.srcImage,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   req.staging->buffer(), 1, &copy);

            VkImageMemoryBarrier toRestore = toTransfer;
            toRestore.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            toRestore.newLayout = req.srcLayoutAtPassEnd;
            toRestore.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            // Swapchain images expect to be ready for present; offscreen targets
            // for shader read. Either way COLOR_ATTACHMENT_OUTPUT is a safe stage
            // since we're about to hand off to present or to the next frame.
            toRestore.dstAccessMask = 0;
            vkCmdPipelineBarrier(m_currentCommandBuffer,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &toRestore);
        }
    }

    // End command buffer recording
    VkResult result = vkEndCommandBuffer(m_currentCommandBuffer);
    if (result != VK_SUCCESS) {
        qCWarning(KWIN_CORE) << "Failed to end command buffer recording:" << result;
        return;
    }

    // Submit command buffer to graphics queue
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &m_currentCommandBuffer;

    // Check if we have GPU-GPU synchronization info (swapchain rendering)
    const bool hasGpuSync = m_currentSyncInfo.imageAvailableSemaphore != VK_NULL_HANDLE && m_currentSyncInfo.renderFinishedSemaphore != VK_NULL_HANDLE;

    // Aggregate wait semaphores: the swapchain's imageAvailable (when present) plus any
    // external semaphores attached this frame (e.g. ZoomEffect's offscreen submission).
    std::vector<VkSemaphore> waitSemaphores;
    std::vector<VkPipelineStageFlags> waitStages;
    if (hasGpuSync) {
        waitSemaphores.push_back(m_currentSyncInfo.imageAvailableSemaphore);
        waitStages.push_back(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    }
    waitSemaphores.insert(waitSemaphores.end(), m_externalWaitSemaphores.begin(), m_externalWaitSemaphores.end());
    waitStages.insert(waitStages.end(), m_externalWaitStages.begin(), m_externalWaitStages.end());

    if (hasGpuSync) {
        // GPU-GPU semaphore synchronization (no CPU blocking needed for render-present sync)
        submitInfo.waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.size());
        submitInfo.pWaitSemaphores = waitSemaphores.data();
        submitInfo.pWaitDstStageMask = waitStages.data();

        // Signal renderFinishedSemaphore when rendering is done (for present to wait on)
        VkSemaphore signalSemaphores[] = {m_currentSyncInfo.renderFinishedSemaphore};
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = signalSemaphores;

        // Use the in-flight fence for CPU-GPU sync (command buffer reuse protection)
        VkFence fence = m_currentSyncInfo.inFlightFence;
        if (fence == VK_NULL_HANDLE) {
            fence = m_context->getOrCreateFence();
            vkResetFences(m_backend->device(), 1, &fence);
        }

        VkResult result = vkQueueSubmit(m_backend->graphicsQueue(), 1, &submitInfo, fence);
        if (result != VK_SUCCESS) {
            qCWarning(KWIN_CORE) << "Failed to submit command buffer with GPU-GPU sync:" << result;
            m_context->freeCommandBuffer(m_currentCommandBuffer);
            m_currentCommandBuffer = VK_NULL_HANDLE;
            m_currentFramebuffer = nullptr;
            m_currentSyncInfo = VulkanSyncInfo{};
            m_externalWaitSemaphores.clear();
            m_externalWaitStages.clear();
            return;
        }

        // No blocking wait here! The GPU-GPU synchronization handles the timing:
        // - Render waits on imageAvailableSemaphore (signaled by acquireNextImage)
        // - Present waits on renderFinishedSemaphore (signaled by this submit)
        // - The inFlightFence will be waited on at the START of the NEXT frame

        // …except when post-pass copies are pending: block once so the staging
        // buffers are safe to read on the CPU before firing their callbacks.
        if (!m_pendingPostPassCopies.empty()) {
            vkWaitForFences(m_backend->device(), 1, &fence, VK_TRUE, UINT64_MAX);
            drainPostPassCopies(m_pendingPostPassCopies);
        }

        // Handle release points for external sync (e.g., DMA-BUF clients)
        if (!m_releasePoints.empty() && m_context->supportsExternalFenceFd()) {
            // Export fence to sync fd for release points
            VkFence exportableFence = m_context->createExportableFence();
            if (exportableFence != VK_NULL_HANDLE) {
                // Submit a no-op command to signal the exportable fence after render completes
                VkSubmitInfo syncSubmit{};
                syncSubmit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                syncSubmit.waitSemaphoreCount = 1;
                syncSubmit.pWaitSemaphores = signalSemaphores; // Wait on renderFinished
                VkPipelineStageFlags syncStages[] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
                syncSubmit.pWaitDstStageMask = syncStages;

                if (vkQueueSubmit(m_backend->graphicsQueue(), 1, &syncSubmit, exportableFence) == VK_SUCCESS) {
                    FileDescriptor syncFd = m_context->exportFenceToSyncFd(exportableFence);
                    if (syncFd.isValid()) {
                        for (const auto &releasePoint : m_releasePoints) {
                            if (releasePoint) {
                                releasePoint->addReleaseFence(syncFd);
                            }
                        }
                    }
                }
                vkDestroyFence(m_backend->device(), exportableFence, nullptr);
            }
        }
        m_releasePoints.clear();

        // Descriptor sets are not explicitly freed here - they will be freed when
        // the descriptor pool is reset at the start of the next frame when
        // m_outputsInFlight reaches 0. This avoids CPU stalls from vkDeviceWaitIdle.
        m_frameDescriptorSets.clear();
    } else {
        // Fallback: no swapchain semaphores, use non-blocking synchronization
        // This path is used for:
        // - Rendering to offscreen textures
        // - Backends without proper semaphore support

        // Apply external wait semaphores (e.g. ZoomEffect's offscreen-render signal).
        if (!waitSemaphores.empty()) {
            submitInfo.waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.size());
            submitInfo.pWaitSemaphores = waitSemaphores.data();
            submitInfo.pWaitDstStageMask = waitStages.data();
        }

        // Try external fence fd for non-blocking client sync if supported
        if (m_context->supportsExternalFenceFd() && !m_releasePoints.empty()) {
            VkFence exportableFence = m_context->createExportableFence();
            if (exportableFence != VK_NULL_HANDLE) {
                if (vkQueueSubmit(m_backend->graphicsQueue(), 1, &submitInfo, exportableFence) == VK_SUCCESS) {
                    FileDescriptor syncFd = m_context->exportFenceToSyncFd(exportableFence);
                    if (syncFd.isValid()) {
                        for (const auto &releasePoint : m_releasePoints) {
                            if (releasePoint) {
                                releasePoint->addReleaseFence(syncFd);
                            }
                        }
                    }
                    m_releasePoints.clear();
                    // Wait for the fence so the command buffer is safe to free immediately.
                    vkWaitForFences(m_backend->device(), 1, &exportableFence, VK_TRUE, UINT64_MAX);
                    if (!m_pendingPostPassCopies.empty()) {
                        drainPostPassCopies(m_pendingPostPassCopies);
                    }
                    vkDestroyFence(m_backend->device(), exportableFence, nullptr);
                    m_context->freeCommandBuffer(m_currentCommandBuffer);
                    m_currentCommandBuffer = VK_NULL_HANDLE;
                    m_currentFramebuffer = nullptr;
                    m_currentSyncInfo = VulkanSyncInfo{};
                    return;
                }
                vkDestroyFence(m_backend->device(), exportableFence, nullptr);
            }
        }

        // Non-blocking fallback: submit without waiting
        VkFence fence = m_context->getOrCreateFence();
        vkResetFences(m_backend->device(), 1, &fence);

        VkResult result = vkQueueSubmit(m_backend->graphicsQueue(), 1, &submitInfo, fence);
        if (result != VK_SUCCESS) {
            qCWarning(KWIN_CORE) << "Failed to submit command buffer:" << result;
            m_context->freeCommandBuffer(m_currentCommandBuffer);
            m_currentCommandBuffer = VK_NULL_HANDLE;
            m_currentFramebuffer = nullptr;
            m_currentSyncInfo = VulkanSyncInfo{};
            m_externalWaitSemaphores.clear();
            m_externalWaitStages.clear();
            return;
        }
        m_releasePoints.clear();

        // Wait for the fence so the command buffer can be freed immediately rather than
        // leaking via m_prevCommandBuffer (the fallback path has no guarantee that
        // doBeginFrame will wait for this specific fence next frame).
        vkWaitForFences(m_backend->device(), 1, &fence, VK_TRUE, UINT64_MAX);

        // GPU is idle for this submission; safe to read back any post-pass copies.
        if (!m_pendingPostPassCopies.empty()) {
            drainPostPassCopies(m_pendingPostPassCopies);
        }

        m_context->freeCommandBuffer(m_currentCommandBuffer);
        m_currentCommandBuffer = VK_NULL_HANDLE;

        // Descriptor sets will be freed when the descriptor pool is reset at the start
        // of the next frame (when m_outputsInFlight reaches 0), avoiding CPU stalls
        m_frameDescriptorSets.clear();
    }

    // Defer freeing to the start of the next beginFrame(), where doBeginFrame() has
    // already waited for the inFlightFence, guaranteeing the GPU is done with this buffer.
    m_prevCommandBuffer = m_currentCommandBuffer;
    m_currentCommandBuffer = VK_NULL_HANDLE;
    m_currentFramebuffer = nullptr;
    m_currentSyncInfo = VulkanSyncInfo{};
    m_externalWaitSemaphores.clear();
    m_externalWaitStages.clear();

    // Decrement output counter - when it reaches 0, next beginFrame can reset pool
    if (m_outputsInFlight > 0) {
        m_outputsInFlight--;
    }
}

void ItemRendererVulkan::renderBackground(const RenderTarget &renderTarget, const RenderViewport &viewport, const QRegion &region)
{
    if (m_currentCommandBuffer == VK_NULL_HANDLE) {
        return;
    }

    // The render pass already clears the framebuffer with the clear color
    // If we need to render a specific background color or pattern, we'd do it here
    // For now, the clear in beginFrame handles this
}

QVector4D ItemRendererVulkan::modulate(float opacity, float brightness) const
{
    const float a = opacity;
    const float rgb = opacity * brightness;
    return QVector4D(rgb, rgb, rgb, a);
}

// Clip an item's window quads to the damage region, matching ItemRendererOpenGL::clipQuads().
//
// On a partial-repaint frame context->clip is the damage region. Quads fully outside it are
// dropped and partially-covered quads are split to their intersection, so the geometry a draw
// produces never extends past the actual damage — a lower window cannot paint into the gap
// between disjoint damage rects, where an undamaged higher window is showing. When the clip is
// infinite (full repaint) or hardware clipping is active, quads are emitted whole and the GPU
// scissor does the clipping instead.
static RenderGeometry clipQuads(const Item *item, const ItemRendererVulkan::RenderContext *context)
{
    const WindowQuadList quads = item->quads();
    // Item-to-world translation, so clip rects (world space) can be moved into item space.
    const QPointF worldTranslation = context->transformStack.top().map(QPointF(0., 0.));
    const qreal scale = context->renderTargetScale;

    RenderGeometry geometry;
    geometry.reserve(quads.count() * 6);

    for (const WindowQuad &quad : quads) {
        if (context->clip != infiniteRegion() && !context->hardwareClipping) {
            // Scale to device coordinates, rounding as needed.
            const QRectF deviceBounds = snapToPixelGridF(scaledRect(quad.bounds(), scale));

            for (const QRect &clipRect : context->clip) {
                const QRectF deviceClipRect = snapToPixelGridF(scaledRect(clipRect, scale)).translated(-worldTranslation);
                const QRectF intersected = deviceClipRect.intersected(deviceBounds);
                if (intersected.isValid()) {
                    if (deviceBounds == intersected) {
                        // case 1: quad completely inside this clip rect — keep it whole
                        geometry.appendWindowQuad(quad, scale);
                        break;
                    }
                    // case 2: partial intersection — emit only the covered sub-quad
                    geometry.appendSubQuad(quad, intersected, scale);
                }
            }
        } else {
            geometry.appendWindowQuad(quad, scale);
        }
    }

    return geometry;
}

void ItemRendererVulkan::createRenderNode(Item *item, RenderContext *context)
{
    // Create a stable local sorted copy to avoid cache invalidation issues during rendering
    QList<Item *> sortedChildItems = item->childItems();
    std::stable_sort(sortedChildItems.begin(), sortedChildItems.end(), [](const Item *a, const Item *b) {
        return a->z() < b->z();
    });

    const qreal scale = context->renderTargetScale;

    // Build transform matrix for this item (matching OpenGL approach)
    const auto logicalPosition = QVector2D(item->position().x(), item->position().y());

    QMatrix4x4 matrix;
    matrix.translate(roundVector(logicalPosition * scale).toVector3D());
    if (context->transformStack.size() == 1) {
        matrix *= context->rootTransform;
    }
    if (!item->transform().isIdentity()) {
        matrix.scale(scale, scale);
        matrix *= item->transform();
        matrix.scale(1 / scale, 1 / scale);
    }
    context->transformStack.push(context->transformStack.top() * matrix);
    context->opacityStack.push(context->opacityStack.top() * item->opacity());

    // Process child items with z < 0 first (behind this item)
    for (Item *childItem : sortedChildItems) {
        if (childItem->z() < 0) {
            if (childItem->explicitVisible()) {
                createRenderNode(childItem, context);
            }
        }
    }

    // Handle border radius (matching OpenGL approach)
    if (const BorderRadius radius = item->borderRadius(); !radius.isNull()) {
        const QRectF nativeRect = snapToPixelGridF(scaledRect(item->rect(), scale));
        const BorderRadius nativeRadius = radius.scaled(scale).rounded();
        context->cornerStack.push({
            .box = nativeRect,
            .radius = nativeRadius,
        });
    } else if (!context->cornerStack.isEmpty()) {
        const auto &top = std::as_const(context->cornerStack).top();
        context->cornerStack.push({
            .box = matrix.inverted().mapRect(top.box),
            .radius = top.radius,
        });
    }

    // Preprocess the item to ensure pixmap/texture is created
    item->preprocess();

    // Build geometry from quads, clipped to the damage region on partial-repaint frames.
    RenderGeometry geometry = clipQuads(item, context);

    // Handle different item types (matching OpenGL's approach)
    if (auto shadowItem = qobject_cast<ShadowItem *>(item)) {
        // ShadowItem - use VulkanShadowTextureProvider
        auto *textureProvider = static_cast<VulkanShadowTextureProvider *>(shadowItem->textureProvider());
        if (!geometry.isEmpty()) {
            if (textureProvider && textureProvider->texture()) {
                VulkanTexture *texture = textureProvider->texture();

                // Post-process texture coordinates (convert pixel coords to normalized)
                geometry.postProcessTextureCoordinates(texture->matrix(VulkanCoordinateType::Unnormalized));

                RenderNode node;
                node.traits = VulkanShaderTrait::MapTexture;
                node.textures.append(texture);
                node.opacity = context->opacityStack.top();
                node.hasAlpha = true;
                node.colorDescription = item->colorDescription();
                node.renderingIntent = item->renderingIntent();
                node.bufferReleasePoint = nullptr;
                node.transformMatrix = context->transformStack.top();

                // Copy geometry to node
                for (const auto &vertex : geometry) {
                    node.geometry.append({
                        .position = vertex.position,
                        .texcoord = vertex.texcoord,
                    });
                }
                node.vertexCount = node.geometry.count();

                if (node.opacity < 1.0) {
                    node.traits |= VulkanShaderTrait::Modulate;
                }

                context->renderNodes.append(node);
            }
        }
    } else if (auto decorationItem = qobject_cast<DecorationItem *>(item)) {
        // DecorationItem - use SceneVulkanDecorationRenderer
        auto *renderer = static_cast<const SceneVulkanDecorationRenderer *>(decorationItem->renderer());
        if (!geometry.isEmpty()) {
            if (renderer && renderer->texture()) {
                VulkanTexture *texture = renderer->texture();

                // Post-process texture coordinates (convert pixel coords to normalized)
                geometry.postProcessTextureCoordinates(texture->matrix(VulkanCoordinateType::Unnormalized));

                RenderNode node;
                node.traits = VulkanShaderTrait::MapTexture;
                node.textures.append(texture);
                node.opacity = context->opacityStack.top();
                node.hasAlpha = true;
                node.colorDescription = item->colorDescription();
                node.renderingIntent = item->renderingIntent();
                node.bufferReleasePoint = nullptr;
                node.transformMatrix = context->transformStack.top();

                // Copy geometry to node
                for (const auto &vertex : geometry) {
                    node.geometry.append({
                        .position = vertex.position,
                        .texcoord = vertex.texcoord,
                    });
                }
                node.vertexCount = node.geometry.count();

                if (node.opacity < 1.0) {
                    node.traits |= VulkanShaderTrait::Modulate;
                }

                context->renderNodes.append(node);
            }
        }
    } else if (auto surfaceItem = qobject_cast<SurfaceItem *>(item)) {
        // SurfaceItem - main window content
        SurfacePixmap *pixmap = surfaceItem->pixmap();
        auto vulkanSurfaceTexture = pixmap ? dynamic_cast<VulkanSurfaceTextureX11 *>(pixmap->texture()) : nullptr;
        if (pixmap && !geometry.isEmpty()) {
            if (vulkanSurfaceTexture && vulkanSurfaceTexture->isValid()) {
                VulkanSurfaceContents surfaceContents = vulkanSurfaceTexture->texture();

                // Get the first plane for texture coordinates
                VulkanTexture *texture = surfaceContents.firstPlane();

                if (!texture) {
                    qCWarning(KWIN_VULKAN) << "SurfaceItem skipped: no valid texture planes";
                    return;
                }

                // Post-process texture coordinates (convert pixel coords to normalized)
                QMatrix4x4 texMatrix = texture->matrix(VulkanCoordinateType::Unnormalized);
                geometry.postProcessTextureCoordinates(texMatrix);

                RenderNode node;
                node.traits = VulkanShaderTrait::MapTexture;
                node.textures = surfaceContents.toVarLengthArray();

                // Check if this is a multi-plane YUV texture (3 textures: Y, U, V)
                if (node.textures.size() >= 3) {
                    node.traits |= VulkanShaderTrait::YUV;
                }

                node.opacity = context->opacityStack.top();
                node.hasAlpha = pixmap->hasAlphaChannel();
                node.colorDescription = surfaceItem->colorDescription();
                node.renderingIntent = surfaceItem->renderingIntent();
                node.bufferReleasePoint = surfaceItem->bufferReleasePoint();
                node.transformMatrix = context->transformStack.top();

                if (node.bufferReleasePoint) {
                    m_releasePoints.insert(node.bufferReleasePoint);
                }

                // Copy geometry to node
                for (const auto &vertex : geometry) {
                    node.geometry.append({
                        .position = vertex.position,
                        .texcoord = vertex.texcoord,
                    });
                }
                node.vertexCount = node.geometry.count();

                // Handle rounded corners
                if (!context->cornerStack.isEmpty()) {
                    const auto &top = context->cornerStack.top();
                    if (!top.radius.isNull()) {
                        node.traits |= VulkanShaderTrait::RoundedCorners;
                        node.hasAlpha = true;
                        node.box = QVector4D(top.box.x() + top.box.width() * 0.5,
                                             top.box.y() + top.box.height() * 0.5,
                                             top.box.width() * 0.5,
                                             top.box.height() * 0.5);
                        node.borderRadius = top.radius.toVector();
                    }
                }

                // Handle opacity modulation
                if (node.opacity < 1.0) {
                    node.traits |= VulkanShaderTrait::Modulate;
                }

                context->renderNodes.append(node);
            } else {
                qCWarning(KWIN_VULKAN) << "SurfaceItem skipped: invalid texture";
            }
        }
    } else if (auto imageItem = qobject_cast<ImageItemVulkan *>(item)) {
        // ImageItemVulkan - use the texture from preprocess()
        if (!geometry.isEmpty() && imageItem->texture()) {
            VulkanTexture *texture = imageItem->texture();

            // Post-process texture coordinates (convert pixel coords to normalized)
            geometry.postProcessTextureCoordinates(texture->matrix(VulkanCoordinateType::Unnormalized));

            RenderNode node;
            node.traits = VulkanShaderTrait::MapTexture;
            node.textures.append(texture);
            node.opacity = context->opacityStack.top();
            node.hasAlpha = true;
            node.colorDescription = item->colorDescription();
            node.renderingIntent = item->renderingIntent();
            node.bufferReleasePoint = nullptr;
            node.transformMatrix = context->transformStack.top();

            // Copy geometry to node
            for (const auto &vertex : geometry) {
                node.geometry.append({
                    .position = vertex.position,
                    .texcoord = vertex.texcoord,
                });
            }
            node.vertexCount = node.geometry.count();

            if (node.opacity < 1.0) {
                node.traits |= VulkanShaderTrait::Modulate;
            }

            context->renderNodes.append(node);
        }
    } else if (auto borderItem = qobject_cast<OutlinedBorderItem *>(item)) {
        // OutlinedBorderItem - uses Border trait, no texture needed
        if (!geometry.isEmpty()) {
            const BorderOutline outline = borderItem->outline();
            const int thickness = std::round(outline.thickness() * scale);
            const QRectF outerRect = snapToPixelGridF(scaledRect(borderItem->rect(), scale));
            const QRectF innerRect = outerRect.adjusted(thickness, thickness, -thickness, -thickness);

            RenderNode node;
            node.traits = VulkanShaderTrait::Border;
            node.opacity = context->opacityStack.top();
            node.hasAlpha = true;
            node.transformMatrix = context->transformStack.top();
            node.box = QVector4D(outerRect.x(), outerRect.y(), outerRect.width(), outerRect.height());
            // Inner box stored in borderRadius for the Border shader
            node.borderRadius = QVector4D(innerRect.x(), innerRect.y(), innerRect.width(), innerRect.height());
            node.borderColor = outline.color();

            // Copy geometry to node
            for (const auto &vertex : geometry) {
                node.geometry.append({
                    .position = vertex.position,
                    .texcoord = vertex.texcoord,
                });
            }
            node.vertexCount = node.geometry.count();

            if (node.opacity < 1.0) {
                node.traits |= VulkanShaderTrait::Modulate;
            }

            context->renderNodes.append(node);
        }
    } else if (qobject_cast<WindowItem *>(item) || qobject_cast<RootItem *>(item)) {
        // Container items with no geometry of their own — children are rendered separately
    } else {
        qCWarning(KWIN_VULKAN) << "Unhandled item type:" << item->metaObject()->className();
    }

    // Process children with z >= 0 (in front of this item)
    for (Item *childItem : sortedChildItems) {
        if (childItem->z() >= 0) {
            if (childItem->explicitVisible()) {
                createRenderNode(childItem, context);
            }
        }
    }

    // Pop stacks at the end (matching the push at the start)
    context->transformStack.pop();
    context->opacityStack.pop();
    if (!context->cornerStack.isEmpty()) {
        context->cornerStack.pop();
    }
}

void ItemRendererVulkan::renderNodes(const RenderContext &context, VkCommandBuffer cmd, VkRenderPass renderPass)
{
    if (context.renderNodes.isEmpty()) {
        return;
    }

    auto *pipelineManager = m_context->pipelineManager();
    VulkanBuffer *streamingBuffer = m_frameResources[m_currentFrameIndex].streamingBuffer.get();
    VulkanBuffer *uniformBuffer = m_frameResources[m_currentFrameIndex].uniformBuffer.get();

    // Calculate total vertex count for this batch
    size_t totalVertexCount = 0;
    for (const RenderNode &node : context.renderNodes) {
        totalVertexCount += node.geometry.count();
    }

    // Upload all vertex data to streaming buffer at current offset
    // Each renderItem() call advances the offset to avoid overwriting previous data
    std::vector<GLVertex2D> allVertices;
    allVertices.reserve(totalVertexCount);

    // firstVertex is the GLOBAL vertex index in the buffer (not local to this batch)
    int localVertexOffset = 0;
    for (RenderNode &node : const_cast<QList<RenderNode> &>(context.renderNodes)) {
        node.firstVertex = static_cast<int>(m_vertexBufferOffset / sizeof(GLVertex2D)) + localVertexOffset;
        for (int i = 0; i < node.geometry.count(); ++i) {
            allVertices.push_back(node.geometry.at(i));
        }
        localVertexOffset += node.vertexCount;
    }

    if (!allVertices.empty()) {
        void *mappedData = streamingBuffer->map();
        if (mappedData) {
            // Copy to the current offset position in the buffer
            uint8_t *dst = static_cast<uint8_t *>(mappedData) + m_vertexBufferOffset;
            memcpy(dst, allVertices.data(), allVertices.size() * sizeof(GLVertex2D));
            streamingBuffer->unmap();
            // Flush the region we wrote to
            streamingBuffer->flush(m_vertexBufferOffset, allVertices.size() * sizeof(GLVertex2D));

            // Advance the offset for the next renderItem() call
            m_vertexBufferOffset += allVertices.size() * sizeof(GLVertex2D);
        }

        // Bind vertex buffer at offset 0 - firstVertex handles the actual offset
        VkBuffer vertexBuffers[] = {streamingBuffer->buffer()};
        VkDeviceSize offsets[] = {0};

        // Validate vertex buffer before binding
        if (vertexBuffers[0] == VK_NULL_HANDLE) {
            qCWarning(KWIN_CORE) << "Vertex buffer is null, skipping vertex buffer binding";
        } else {
            vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);
        }
    }

    // Non-hardware-clipped draws inherit the scissor set in beginFrame
    // (m_frameRenderArea — the full framebuffer, or the damage bounding box on a
    // partial-repaint frame). The hardware-clipping branch below overrides it
    // per node and restores it afterwards.

    // Render each node
    VulkanPipeline *currentPipeline = nullptr;

    for (const RenderNode &node : context.renderNodes) {
        if (node.vertexCount == 0) {
            continue;
        }

        // Fold window-global brightness/saturation modulation into the per-node
        // traits so the correct pipeline (specialization constants) is selected.
        VulkanShaderTraits effectiveTraits = node.traits;
        if (context.brightness != 1.0) {
            effectiveTraits |= VulkanShaderTrait::Modulate;
        }
        if (context.saturation != 1.0) {
            effectiveTraits |= VulkanShaderTrait::AdjustSaturation;
        }

        // Get or create pipeline for this node's traits, scoped to the
        // currently-active render pass. The format-aware lookup keeps the
        // swapchain's BGRA pipelines and an offscreen RGBA caller's pipelines
        // in separate cache buckets, so a single render thread can interleave
        // both without violating Vulkan §8.2 render-pass compatibility.
        VulkanPipeline *pipeline = pipelineManager->pipeline(effectiveTraits, renderPass);
        if (!pipeline) {
            qCWarning(KWIN_CORE) << "Failed to get pipeline for traits:" << static_cast<int>(effectiveTraits);
            continue;
        }

        // Validate pipeline before using it
        if (!pipeline->isValid()) {
            qCWarning(KWIN_CORE) << "Pipeline is invalid for traits:" << static_cast<int>(effectiveTraits);
            continue;
        }

        // Bind pipeline if changed
        if (pipeline != currentPipeline) {
            if (pipeline->pipeline() != VK_NULL_HANDLE) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline());
                currentPipeline = pipeline;
            } else {
                qCWarning(KWIN_CORE) << "Skipping null pipeline binding for traits:" << static_cast<int>(effectiveTraits);
                continue;
            }
        }

        // Set push constants (MVP matrix and texture matrix)
        VulkanPushConstants pc{};
        const QMatrix4x4 mvp = context.projectionMatrix * node.transformMatrix;

        memcpy(pc.mvp, mvp.data(), sizeof(pc.mvp));
        // Texture matrix is identity - we already applied it via postProcessTextureCoordinates()
        // on the CPU side before uploading the geometry, so the shader doesn't need to transform again
        memset(pc.textureMatrix, 0, sizeof(pc.textureMatrix));
        pc.textureMatrix[0] = 1.0f;
        pc.textureMatrix[5] = 1.0f;
        pc.textureMatrix[10] = 1.0f;
        pc.textureMatrix[15] = 1.0f;

        vkCmdPushConstants(cmd, pipeline->layout(),
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(VulkanPushConstants), &pc);

        // Allocate descriptor set first (needed for both normal and default texture cases)
        VkDescriptorSet descriptorSet = m_context->allocateDescriptorSet(pipeline->descriptorSetLayout());
        if (descriptorSet == VK_NULL_HANDLE) {
            qCWarning(KWIN_CORE) << "Failed to allocate descriptor set, skipping node";
            continue;
        }

        // Track descriptor set for cleanup after frame submission
        m_frameDescriptorSets.push_back(descriptorSet);

        // Upload uniforms to buffer
        VulkanUniforms uniforms{};
        const QVector4D modulation = modulate(node.opacity, 1.0f);
        memcpy(uniforms.uniformColor, &modulation, sizeof(uniforms.uniformColor));
        uniforms.opacity = node.opacity;
        uniforms.brightness = context.brightness;
        uniforms.saturation = context.saturation;
        uniforms.primaryBrightness[0] = context.primaryBrightness.x();
        uniforms.primaryBrightness[1] = context.primaryBrightness.y();
        uniforms.primaryBrightness[2] = context.primaryBrightness.z();
        memcpy(uniforms.geometryBox, &node.box, sizeof(uniforms.geometryBox));
        memcpy(uniforms.borderRadius, &node.borderRadius, sizeof(uniforms.borderRadius));
        uniforms.borderThickness = node.borderThickness;
        const QVector4D borderColor(node.borderColor.redF(), node.borderColor.greenF(),
                                    node.borderColor.blueF(), node.borderColor.alphaF());
        memcpy(uniforms.borderColor, &borderColor, sizeof(uniforms.borderColor));

        // Uniform slot cursor is per-frame (reset in beginFrame) and indexes this
        // frame slot's own uniform buffer, so there is no cross-frame aliasing.
        const VkDeviceSize uniformOffset = (m_uniformDrawIndex % 1024) * sizeof(VulkanUniforms);
        uniformBuffer->upload(&uniforms, sizeof(VulkanUniforms), uniformOffset);
        m_uniformDrawIndex++;

        // Update and bind descriptor sets for textures and UBO

        // Get the default texture to use for unused slots
        VulkanTexture *defaultTexture = m_defaultWhiteTexture.get();
        if (!defaultTexture || !defaultTexture->isValid()) {
            qCWarning(KWIN_CORE) << "No default texture available, skipping node";
            continue;
        }

        // Always create exactly 4 image infos for the sampler array
        std::array<VkDescriptorImageInfo, 4> imageInfos{};

        // Fill slots with node textures first, then default texture for remaining slots
        int slot = 0;
        for (VulkanTexture *tex : node.textures) {
            if (tex && tex->isValid() && tex->imageView() != VK_NULL_HANDLE && slot < 4) {
                imageInfos[slot].sampler = tex->sampler();
                imageInfos[slot].imageView = tex->imageView();
                imageInfos[slot].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                slot++;
            }
        }

        // Fill remaining slots with default texture
        for (; slot < 4; slot++) {
            imageInfos[slot].sampler = defaultTexture->sampler();
            imageInfos[slot].imageView = defaultTexture->imageView();
            imageInfos[slot].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }

        // Set up UBO
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = uniformBuffer->buffer();
        bufferInfo.offset = uniformOffset;
        bufferInfo.range = sizeof(VulkanUniforms);

        // Update descriptor set with all 4 samplers and the UBO
        std::array<VkWriteDescriptorSet, 2> descriptorWrites{};
        descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrites[0].dstSet = descriptorSet;
        descriptorWrites[0].dstBinding = 0;
        descriptorWrites[0].dstArrayElement = 0;
        descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptorWrites[0].descriptorCount = 4; // Always update all 4 samplers
        descriptorWrites[0].pImageInfo = imageInfos.data();

        descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrites[1].dstSet = descriptorSet;
        descriptorWrites[1].dstBinding = 1;
        descriptorWrites[1].dstArrayElement = 0;
        descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        descriptorWrites[1].descriptorCount = 1;
        descriptorWrites[1].pBufferInfo = &bufferInfo;

        vkUpdateDescriptorSets(m_backend->device(), static_cast<uint32_t>(descriptorWrites.size()),
                               descriptorWrites.data(), 0, nullptr);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipeline->layout(), 0, 1, &descriptorSet, 0, nullptr);

        // Draw (descriptor set is guaranteed bound since we continue above if not)
        if (node.vertexCount > 0) {
            // Update scissor for hardware clipping if needed
            if (context.hardwareClipping) {
                // Convert clip region to device coordinates, using its bounding
                // rect, and clamp to the frame's render area so a clipped draw
                // can never write outside the damaged region on a partial frame.
                const QRect frameArea(m_frameRenderArea.offset.x, m_frameRenderArea.offset.y,
                                      m_frameRenderArea.extent.width, m_frameRenderArea.extent.height);
                const QRect scissorRect = context.viewport->mapToRenderTarget(context.clip).boundingRect() & frameArea;
                VkRect2D scissor{};
                scissor.offset.x = scissorRect.x();
                scissor.offset.y = scissorRect.y();
                scissor.extent.width = scissorRect.width();
                scissor.extent.height = scissorRect.height();
                vkCmdSetScissor(cmd, 0, 1, &scissor);
            }

            vkCmdDraw(cmd, node.vertexCount, 1, node.firstVertex, 0);
        }
    }

    // Restore the frame-wide scissor if a hardware-clipped node overrode it.
    if (context.hardwareClipping) {
        vkCmdSetScissor(cmd, 0, 1, &m_frameRenderArea);
    }
}

void ItemRendererVulkan::renderItem(const RenderTarget &renderTarget, const RenderViewport &viewport, Item *item, int mask, const QRegion &region, const WindowPaintData &data)
{
    // Route to the renderTarget's command buffer when set (recursive offscreen
    // flow), otherwise the swapchain one — see activeCommandBuffer() doc.
    VkCommandBuffer cmd = activeCommandBuffer(renderTarget);
    if (cmd == VK_NULL_HANDLE) {
        qCWarning(KWIN_VULKAN) << "No active command buffer for rendering";
        return;
    }

    // Hardware clipping is needed when the region is clipped AND the window is transformed
    bool hardwareClipping = region != infiniteRegion() && (static_cast<bool>(mask & Effect::PAINT_WINDOW_TRANSFORMED) || static_cast<bool>(mask & Effect::PAINT_SCREEN_TRANSFORMED));

    // Build render context using the viewport's projection matrix
    RenderContext context(viewport.projectionMatrix(), data.toMatrix(viewport.scale()), region, hardwareClipping, viewport.scale(), &viewport);

    // Window-global brightness/saturation modulation (e.g. diminactive).
    context.brightness = data.brightness();
    context.saturation = data.saturation();
    const auto toXYZ = renderTarget.colorDescription().containerColorimetry().toXYZ();
    context.primaryBrightness = QVector3D(toXYZ(1, 0), toXYZ(1, 1), toXYZ(1, 2));

    // Initialize stacks (matching OpenGL approach)
    // Push identity - rootTransform is applied in createRenderNode when stack size is 1
    context.transformStack.push(QMatrix4x4());
    context.opacityStack.push(data.opacity());

    // Build render nodes from item tree
    createRenderNode(item, &context);

    // Render all nodes using the appropriate command buffer and render pass.
    // activeRenderPass() routes via the renderTarget's framebuffer when set,
    // so offscreen consumers (window thumbnails, screenshot, ...) get
    // pipelines compiled against their own render pass.
    const VkRenderPass renderPass = activeRenderPass(renderTarget);
    renderNodes(context, cmd, renderPass);
}

} // namespace KWin
