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
#include <vector>
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

    void beginFrame(const RenderTarget &renderTarget, const RenderViewport &viewport, const QRegion &damage) override;
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
     * Returns the render pass currently being drawn into for @p renderTarget.
     * Prefers the render pass of @c renderTarget.vulkanTarget()->framebuffer()
     * (recursive offscreen flow), falling back to the swapchain framebuffer's
     * render pass otherwise.
     *
     * Used to look up pipelines via the format-aware
     * @c VulkanPipelineManager::pipeline(traits, renderPass) overload, so an
     * offscreen consumer rendering into a non-swapchain format (e.g. RGBA for
     * zero-copy QtQuick import) gets pipelines compiled against the matching
     * render pass — never the swapchain's, which would violate Vulkan spec
     * §8.2 render-pass compatibility.
     */
    VkRenderPass activeRenderPass(const RenderTarget &renderTarget) const;

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
     * Effects that flow through renderNodes() outside the main beginFrame /
     * endFrame cycle (e.g. screenshot, recursive paints) advance this offset
     * into the shared streaming buffer. With Phase 0 in place, single-time
     * submissions wait on a per-submission fence rather than draining the
     * queue, so save/restore around such a call still works — the wait
     * ensures the GPU is done with the offset region before the main frame
     * reuses it. The newer pushOffscreenSlot() / popOffscreenSlot() pair
     * avoids the wait entirely by writing into the dedicated offscreen slot.
     */
    size_t vertexBufferOffset() const { return m_vertexBufferOffset; }
    void setVertexBufferOffset(size_t offset) { m_vertexBufferOffset = offset; }

    /**
     * Switch the renderer to the dedicated offscreen streaming-buffer slot
     * (@c kOffscreenSlot) and save the prior swapchain-slot state.
     *
     * Pair with @c popOffscreenSlot() to bracket an offscreen render that
     * flows through @c renderItem() / @c drawWindow(). The offscreen slot's
     * vertex/uniform writes never alias a swapchain frame's buffers, so the
     * outer frame's GPU work can proceed concurrently — no need to wait on
     * the offscreen submission's fence before letting the main frame reuse
     * its own buffer region. Within a frame the offscreen slot accumulates
     * across calls; it resets at the next @c beginFrame.
     */
    void pushOffscreenSlot();
    void popOffscreenSlot();

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
    void renderNodes(const RenderContext &context, VkCommandBuffer cmd, VkRenderPass renderPass);

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
    // Slots in each frame's uniform buffer; m_uniformDrawIndex is taken
    // modulo this to address the slot. Sized for a busy multi-window
    // session — exceeding it would silently alias an earlier draw's
    // uniforms, so renderNodes() warns once when m_uniformDrawIndex would
    // wrap so the size can be raised before the silent corruption.
    static constexpr uint32_t kMaxUniformDrawsPerFrame = 1024;

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

    // Accumulated cursors for the dedicated offscreen slot (kOffscreenSlot).
    // Persist across pushOffscreenSlot() calls within a single frame so
    // multiple async offscreen renders can pack into the same slot's
    // streaming and uniform buffers without overwriting each other. Both
    // reset at beginFrame().
    size_t m_offscreenVertexOffset = 0;
    uint32_t m_offscreenUniformDrawIndex = 0;

    // Save/restore stack for pushOffscreenSlot()/popOffscreenSlot().
    struct OffscreenSlotSave
    {
        uint32_t frameIndex;
        size_t vertexOffset;
        uint32_t uniformDrawIndex;
    };
    std::vector<OffscreenSlotSave> m_offscreenSlotStack;

    // --- Partial repaint state (per frame) ---
    // True when this frame only repaints a damage sub-region of a swapchain
    // image, preserving the rest via a LOAD render pass. False => full repaint.
    bool m_partialFrame = false;
    // Device-pixel render area / scissor for this frame: the full framebuffer for
    // a full repaint, the damage bounding box for a partial one. All draws are
    // scissored to it so undamaged pixels keep their preserved contents.
    VkRect2D m_frameRenderArea{};
    // LOAD-variant swapchain render pass (loadOp=LOAD, initialLayout=PRESENT_SRC),
    // used for partial-repaint frames. Lazily (re)created when the color format
    // changes; compatible with the swapchain's presentation framebuffers.
    std::unique_ptr<VulkanRenderPass> m_loadRenderPass;
    VkFormat m_loadRenderPassFormat = VK_FORMAT_UNDEFINED;

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
