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
#include <array>
#include <functional>
#include <memory>
#include <unordered_set>
#include <vulkan/vulkan.h>

namespace KWin
{

class VulkanBackend;
class VulkanContext;
class VulkanBuffer;
class VulkanTexture;
class VulkanFramebuffer;
class VulkanRenderPass;
class VulkanSwapchain;
class VulkanRenderTarget;
class RenderTarget;

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

        // Window-global modulation from WindowPaintData (e.g. diminactive).
        qreal brightness = 1.0;
        qreal saturation = 1.0;
        QVector3D primaryBrightness;

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
     *
     * This is the swapchain (outer) frame's command buffer, valid only
     * between beginFrame() and endFrame(). Effects participating in
     * @c paintScreen / @c drawWindow should NOT use it directly: a parent
     * effect may have invoked them recursively with an offscreen
     * RenderTarget that carries its own command buffer (see ZoomEffect's
     * fullscreen capture, ScreenShotEffect::scheduleVulkanScreenshot, etc.),
     * in which case recording on the swapchain buffer puts draws into the
     * wrong render pass and leaves the recursive target empty.
     *
     * Use @c activeCommandBuffer(renderTarget) inside such hooks instead.
     * In debug builds this method asserts when called while a
     * @c RecursivePaintScope is active.
     */
    VkCommandBuffer currentCommandBuffer() const;

    /**
     * Returns the command buffer that should be used to record draws for
     * @p renderTarget. Prefers @c renderTarget.vulkanTarget()->commandBuffer()
     * when set (recursive offscreen flow), falling back to the swapchain
     * command buffer otherwise.
     *
     * This is the canonical API for effects that participate in the paint
     * chain: it routes draws to the correct render pass even when invoked
     * recursively from another effect's offscreen capture.
     */
    VkCommandBuffer activeCommandBuffer(const RenderTarget &renderTarget) const;

    /**
     * RAII scope that marks the enclosing block as "recording into a
     * recursive offscreen RenderTarget". Effects that invoke
     * @c effects->paintScreen() or @c effects->drawWindow() with their own
     * offscreen target (zoom, screenshot, ...) must wrap the call so the
     * renderer can flag misuse of @c currentCommandBuffer() in debug builds.
     *
     * The guard is reentrant and thread-local. Nested guards stack.
     */
    class KWIN_EXPORT RecursivePaintScope
    {
    public:
        RecursivePaintScope();
        ~RecursivePaintScope();
        RecursivePaintScope(const RecursivePaintScope &) = delete;
        RecursivePaintScope &operator=(const RecursivePaintScope &) = delete;
    };

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

    /**
     * Fullscreen post-pass: runs after the scene's effect chain has finished and the
     * main render pass has been ended. The renderer takes the just-rendered swapchain
     * content, blits it into a sampled "scene capture" texture, and invokes each
     * registered callback inside a fresh render pass on the same swapchain framebuffer.
     * The callback is expected to draw a fullscreen quad (or other geometry) that
     * samples @p sceneCapture and writes the post-processed result to the swapchain.
     *
     * Registrations are stable across frames — register once when the effect becomes
     * active and unregister when it deactivates. Use this for effects that must wrap
     * the entire screen including QuickSceneEffect-derived overlays (overview, etc.),
     * which terminate the effect chain and would otherwise hide downstream effects.
     */
    using FullscreenPostPassCallback = std::function<void(
        VkCommandBuffer cmd,
        VulkanTexture *sceneCapture,
        const RenderTarget &renderTarget,
        const RenderViewport &viewport)>;
    int registerFullscreenPostPass(FullscreenPostPassCallback callback);
    void unregisterFullscreenPostPass(int id);
    bool hasFullscreenPostPasses() const
    {
        return !m_fullscreenPostPasses.empty();
    }

    /**
     * Drains all registered fullscreen post-passes against the current frame's
     * swapchain target. Must be called after the effect chain's paintScreen() has
     * returned but before endFrame(). Safe no-op if no callbacks are registered or
     * the current target is not a Vulkan swapchain framebuffer.
     *
     * After this returns, the main scene render pass has been ended and the renderer
     * is "between passes" — endFrame() detects this via m_currentFramebuffer == nullptr
     * and skips its own endRenderPass call.
     */
    void runFullscreenPostPasses(const RenderTarget &renderTarget, const RenderViewport &viewport) override;

    /**
     * Applies the registered fullscreen post-passes to an offscreen framebuffer
     * that was rendered outside the main frame (e.g. the screenshot effect's
     * per-window capture, which uses a recursive drawWindow() and so never goes
     * through runFullscreenPostPasses()).
     *
     * @p cmd is the caller's (single-time) command buffer; @p targetFb must
     * already contain the rendered content with its color image in
     * SHADER_READ_ONLY_OPTIMAL (i.e. the caller's render pass has ended). The
     * method blits the content into a capture texture, re-renders the post-FX
     * over @p targetFb reusing its own render pass, and leaves the color image
     * back in SHADER_READ_ONLY_OPTIMAL. Safe no-op if no post-passes registered.
     */
    void runFullscreenPostPassesOffscreen(VkCommandBuffer cmd, VulkanFramebuffer *targetFb,
                                          const RenderTarget &renderTarget);

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

    VkDescriptorSet m_currentDescriptorSet = VK_NULL_HANDLE;

    // Transient per-frame GPU buffers. The streaming buffer holds this frame's
    // vertex data; the uniform buffer holds per-draw shader uniforms. Both are
    // host-visible and rewritten every frame, so each frame-in-flight needs its
    // own copy — otherwise frame N would overwrite regions frame N-1's GPU work
    // is still reading. kBufferSlots = MAX_FRAMES_IN_FLIGHT swapchain slots plus
    // one dedicated slot for offscreen / non-swapchain targets (which carry no
    // sync info and are serialized, but must not alias a swapchain frame's copy).
    static constexpr uint32_t kFramesInFlight = 2;
    static constexpr uint32_t kOffscreenSlot = kFramesInFlight;
    static constexpr uint32_t kBufferSlots = kFramesInFlight + 1;

    struct FrameResources
    {
        std::unique_ptr<VulkanBuffer> streamingBuffer;
        std::unique_ptr<VulkanBuffer> uniformBuffer;
    };
    std::array<FrameResources, kBufferSlots> m_frameResources;
    // Buffer slot the current beginFrame()/endFrame() pair writes into.
    uint32_t m_currentFrameIndex = 0;
    // Per-draw uniform slot cursor, reset to 0 at the start of every frame.
    uint32_t m_uniformDrawIndex = 0;
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

    // Fullscreen post-pass registrations: each is invoked after the main scene pass.
    // Drained by runFullscreenPostPasses(); registrations are NOT cleared per-frame.
    struct FullscreenPostPassRegistration
    {
        int id;
        FullscreenPostPassCallback callback;
    };
    std::vector<FullscreenPostPassRegistration> m_fullscreenPostPasses;
    int m_nextPostPassId = 1;
    // Lazy resources for runFullscreenPostPasses. Reallocated when the swapchain
    // format/size changes.
    std::unique_ptr<VulkanTexture> m_sceneCaptureTexture;
    std::unique_ptr<VulkanRenderPass> m_postFxRenderPass;
    VkFormat m_postFxRenderPassFormat = VK_FORMAT_UNDEFINED;

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
