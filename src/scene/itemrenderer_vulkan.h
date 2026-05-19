/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2025 Joseph <author@example.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "core/colorspace.h"
#include "core/renderviewport.h"
#include "core/syncobjtimeline.h"
#include "platformsupport/scenes/vulkan/vulkanpipeline.h"
#include "platformsupport/scenes/vulkan/vulkanrendertarget.h"
#include "scene/borderradius.h"
#include "scene/itemgeometry.h"
#include "scene/itemrenderer.h"
#include "scene/surfaceitem.h"
#include "scene/windowitem.h"

#include <QImage>
#include <QMatrix4x4>
#include <QStack>
#include <functional>
#include <unordered_set>
#include <vulkan/vulkan.h>

namespace KWin
{

class VulkanBackend;
class VulkanContext;
class VulkanBuffer;
class VulkanTexture;
class VulkanFramebuffer;
class VulkanSwapchain;
class VulkanRenderTarget;

class KWIN_EXPORT ItemRendererVulkan : public ItemRenderer
{
public:
    struct RenderNode
    {
        VulkanShaderTraits traits;
        QVarLengthArray<VulkanTexture *, 4> textures;
        RenderGeometry geometry;
        QMatrix4x4 transformMatrix;
        int firstVertex = 0;
        int vertexCount = 0;
        qreal opacity = 1;
        bool hasAlpha = false;
        ColorDescription colorDescription = ColorDescription::sRGB;
        RenderingIntent renderingIntent = RenderingIntent::Perceptual;
        std::shared_ptr<SyncReleasePoint> bufferReleasePoint;
        QVector4D box;
        QVector4D borderRadius;
        int borderThickness = 0;
        QColor borderColor;
    };

    struct RenderCorner
    {
        QRectF box;
        BorderRadius radius;
    };

    struct RenderContext
    {
        QList<RenderNode> renderNodes;
        QStack<QMatrix4x4> transformStack;
        QStack<qreal> opacityStack;
        QStack<RenderCorner> cornerStack;
        const QMatrix4x4 projectionMatrix;
        const QMatrix4x4 rootTransform;
        const QRegion clip;
        const bool hardwareClipping;
        const qreal renderTargetScale;
        const RenderViewport *viewport;

        RenderContext(const QMatrix4x4 &proj, const QMatrix4x4 &root, const QRegion &c, bool hc, qreal scale, const RenderViewport *vp)
            : projectionMatrix(proj)
            , rootTransform(root)
            , clip(c)
            , hardwareClipping(hc)
            , renderTargetScale(scale)
            , viewport(vp)
        {
        }
    };

    explicit ItemRendererVulkan(VulkanBackend *backend);
    ~ItemRendererVulkan();

    std::unique_ptr<ImageItem> createImageItem(Item *parent = nullptr) override;

    void beginFrame(const RenderTarget &renderTarget, const RenderViewport &viewport) override;
    void endFrame() override;

    void renderBackground(const RenderTarget &renderTarget, const RenderViewport &viewport, const QRegion &region) override;
    void renderItem(const RenderTarget &renderTarget, const RenderViewport &viewport, Item *item, int mask, const QRegion &region, const WindowPaintData &data) override;

    VulkanBackend *backend() const
    {
        return m_backend;
    }
    VulkanContext *context() const
    {
        return m_context;
    }

    /**
     * Returns the current Vulkan command buffer being used for rendering.
     * This is valid only during a frame (between beginFrame and endFrame).
     */
    VkCommandBuffer currentCommandBuffer() const
    {
        return m_currentCommandBuffer;
    }

    /**
     * Returns the current Vulkan framebuffer being rendered to.
     * This is valid only during a frame (between beginFrame and endFrame).
     * For swapchain framebuffers, colorImage() returns the raw VkImage.
     */
    VulkanFramebuffer *currentFramebuffer() const
    {
        return m_currentFramebuffer;
    }

    /**
     * Snapshot / restore the streaming-buffer write position.
     *
     * Offscreen effects call effects->drawWindow() which flows through renderNodes()
     * and advances m_vertexBufferOffset into the shared streaming buffer. Because
     * endSingleTimeCommands() blocks (vkQueueWaitIdle) before returning, the GPU has
     * finished using that buffer region, so the offset can be safely restored and the
     * main frame can reuse it without a hazard.
     */
    size_t vertexBufferOffset() const { return m_vertexBufferOffset; }
    void setVertexBufferOffset(size_t offset) { m_vertexBufferOffset = offset; }

    VulkanTexture *defaultWhiteTexture() const
    {
        return m_defaultWhiteTexture.get();
    }

    /**
     * Queue an external semaphore to wait on before the main command buffer
     * executes. Used by effects that submit their own auxiliary command buffers
     * (e.g. ZoomEffect rendering the scene into an offscreen image) and need the
     * main submission to wait without a CPU-side vkQueueWaitIdle.
     *
     * Cleared automatically each frame after endFrame() submits.
     */
    void addExternalWaitSemaphore(VkSemaphore sem, VkPipelineStageFlags stage)
    {
        m_externalWaitSemaphores.push_back(sem);
        m_externalWaitStages.push_back(stage);
    }

    /**
     * Capture a region of an image after the current frame's render pass ends.
     *
     * The renderer records vkCmdCopyImageToBuffer into the main command buffer
     * (between endRenderPass and queue submit), waits for the submit to complete,
     * then maps the staging buffer and invokes @p callback with the resulting QImage.
     *
     * Intended for effects that need a snapshot of the rendered output (the screenshot
     * effect, eventually screen recording / color picker / etc.). Reading from the
     * color attachment is illegal mid-render-pass, so requests are deferred to here.
     *
     * @p srcLayoutAtPassEnd is what layout the image is in immediately after the
     * render pass's final-layout transition (typically PRESENT_SRC_KHR for swapchain
     * images). The renderer transitions to TRANSFER_SRC_OPTIMAL, copies, then back.
     */
    using PostPassCopyCallback = std::function<void(const QImage &)>;
    void registerPostPassCopy(VkImage srcImage, VkImageLayout srcLayoutAtPassEnd,
                              VkFormat srcFormat, const VkOffset3D &offset,
                              const VkExtent3D &extent, PostPassCopyCallback callback);

private:
    QVector4D modulate(float opacity, float brightness) const;
    void createRenderNode(Item *item, RenderContext *context);
    void renderNodes(const RenderContext &context, VkCommandBuffer cmd);

    VulkanBackend *m_backend;
    VulkanContext *m_context;

    VkCommandBuffer m_currentCommandBuffer = VK_NULL_HANDLE;
    VkCommandBuffer m_prevCommandBuffer = VK_NULL_HANDLE; // freed at the start of the next beginFrame()
    VulkanFramebuffer *m_currentFramebuffer = nullptr;
    QMatrix4x4 m_currentProjection;

    std::unique_ptr<VulkanBuffer> m_uniformBuffer;
    VkDescriptorSet m_currentDescriptorSet = VK_NULL_HANDLE;

    // Default 1x1 white texture for non-textured draws
    std::unique_ptr<VulkanTexture> m_defaultWhiteTexture;

    std::unordered_set<std::shared_ptr<SyncReleasePoint>> m_releasePoints;

    // GPU-GPU synchronization info for the current frame
    VulkanSyncInfo m_currentSyncInfo;

    // Track frame rendering to coordinate descriptor pool reset
    int m_outputsInFlight = 0;
    uint64_t m_frameNumber = 0;

    // Track vertex buffer offset for streaming (reset each frame)
    size_t m_vertexBufferOffset = 0;

    // Track descriptor sets allocated during frame for cleanup
    std::vector<VkDescriptorSet> m_frameDescriptorSets;

    // External wait semaphores attached by effects this frame (cleared in endFrame).
    std::vector<VkSemaphore> m_externalWaitSemaphores;
    std::vector<VkPipelineStageFlags> m_externalWaitStages;

    // Post-render-pass image captures requested this frame (drained in endFrame).
    struct PostPassCopyRequest
    {
        VkImage srcImage = VK_NULL_HANDLE;
        VkImageLayout srcLayoutAtPassEnd = VK_IMAGE_LAYOUT_UNDEFINED;
        VkFormat srcFormat = VK_FORMAT_UNDEFINED;
        VkOffset3D offset{};
        VkExtent3D extent{};
        std::unique_ptr<VulkanBuffer> staging;
        PostPassCopyCallback callback;
    };
    std::vector<PostPassCopyRequest> m_pendingPostPassCopies;
};

} // namespace KWin
