/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2025 Joseph <author@example.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "core/outputlayer.h"
#include "core/renderbackend.h"
#include "platformsupport/scenes/vulkan/vulkanbackend.h"
#include "platformsupport/scenes/vulkan/vulkancontext.h"
#include "platformsupport/scenes/vulkan/vulkansurfacetexture_x11.h"
#include "platformsupport/scenes/vulkan/vulkanswapchain.h"
#include "utils/damagejournal.h"
#include "x11eventfilter.h"

#include <vulkan/vulkan.h>
#include <xcb/xcb.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace KWin
{

class X11StandaloneBackend;
class X11StandaloneVulkanSurfaceTexture;
class Swapchain;
class SurfacePixmapX11;
class Output;
class VsyncMonitor;
class VulkanPresentTimingMonitor;

class VulkanLayer : public OutputLayer
{
public:
    VulkanLayer(KWin::VulkanBackend *backend);

protected:
    std::optional<OutputLayerBeginFrameInfo> doBeginFrame() override;
    bool doEndFrame(const QRegion &renderedRegion, const QRegion &damagedRegion, OutputFrame *frame) override;
    DrmDevice *scanoutDevice() const override;
    QHash<uint32_t, QList<uint64_t>> supportedDrmFormats() const override;

private:
    KWin::VulkanBackend *const m_backend;
};

/**
 * @brief Vulkan Backend using Vulkan over an X overlay window.
 */
class X11StandaloneVulkanBackend : public KWin::VulkanBackend
{
    Q_OBJECT

public:
    X11StandaloneVulkanBackend(X11StandaloneBackend *backend);
    ~X11StandaloneVulkanBackend() override;

    std::unique_ptr<SurfaceTexture> createSurfaceTextureX11(SurfacePixmapX11 *pixmap) override;
    bool present(Output *output, const std::shared_ptr<OutputFrame> &frame) override;
    bool makeCurrent() override;
    void doneCurrent() override;
    KWin::VulkanContext *vulkanContext() const override;
    OverlayWindow *overlayWindow() const override;
    void init() override;
    OutputLayer *primaryLayer(Output *output) override;

    xcb_connection_t *connection() const;
    xcb_window_t window() const
    {
        return m_window;
    }

    /**
     * @brief Get the swapchain
     */
    KWin::VulkanSwapchain *swapchain() const
    {
        return m_swapchain.get();
    }

    /**
     * @brief Whether damage-driven partial repaint is enabled (KWIN_VULKAN_PARTIAL_REPAINT=1).
     */
    bool partialRepaintEnabled() const
    {
        return m_partialRepaint;
    }

    /**
     * @brief Compute the region that must be repainted into swapchain image @p imageIndex.
     *
     * Vulkan has no buffer-age query, so age is tracked manually: the result is the
     * union of the damage of every frame since this image was last rendered into.
     * Returns infiniteRegion() (full repaint) when partial repaint is disabled or
     * the image has not been rendered before.
     */
    QRegion bufferDamage(uint32_t imageIndex) const;

    /**
     * @brief Record this frame's damage and mark the current swapchain image fresh.
     *
     * Called from VulkanLayer::doEndFrame() with the per-frame damage region.
     */
    void recordFrameDamage(const QRegion &damage);

    /**
     * @brief Start a CPU-clock render-time measurement for the current frame.
     *
     * Called from VulkanLayer::doBeginFrame(). Mirrors the GLX/EGL pattern of
     * holding the in-flight RenderTimeQuery on the backend and attaching it to
     * the OutputFrame at end-of-frame. Without this, OutputFrame::queryRenderTime()
     * returns an empty span, RenderJournal sees zero, and RenderLoop schedules
     * paint ~1ms before each vblank with no real budget.
     */
    void beginRenderTimeQuery();

    /**
     * @brief Close the in-flight render-time measurement and hand it to @p frame.
     *
     * Called from VulkanLayer::doEndFrame(). No-op if beginRenderTimeQuery()
     * was not called for this frame (defensive — should not normally happen).
     */
    void endAndAttachRenderTimeQuery(OutputFrame *frame);

    /**
     * @brief If the swapchain is flagged as needing recreation (typically
     *        VK_SUBOPTIMAL_KHR / VK_ERROR_OUT_OF_DATE_KHR from acquire or
     *        present after a display reconfiguration), recreate it and
     *        reset the dependent state (buffer-age tracking, present-timing
     *        anchor, present-timing queue).
     *
     * Idempotent — does nothing when the flag is clear. Returns true if no
     * recreate was needed *or* recreate succeeded; false if recreate
     * failed. The caller is expected to skip the frame when this returns
     * false and try again next time.
     */
    bool recreateSwapchainIfNeeded();

private:
    bool initInstance();
    bool initPhysicalDevice();
    bool initDevice();
    bool initSurface();
    bool initSwapchain();
    bool initOverlayWindow();

    void screenGeometryChanged();

    /**
     * @brief Reset partial-repaint age tracking. Called whenever the swapchain is
     * (re)created, since the new images carry no usable previous contents.
     */
    void resetDamageTracking();

    /**
     * @brief Create the vsync monitor used to report real presentation timing.
     *
     * Tries the hardware monitors (SGI_video_sync, OML_sync_control) first,
     * falling back to a software (timer-based) monitor — the same cascade the
     * GLX backend uses.
     */
    void initVsyncMonitor();

    /**
     * @brief Vsync monitor callback: reports the real present timestamp of the
     * in-flight frame to its OutputFrame, driving RenderLoop's vblank prediction.
     */
    void vblank(std::chrono::nanoseconds timestamp);

    /**
     * @brief Determine whether VK_EXT_present_timing can be used: device support
     * (set in createDevice) for both present_timing and present_wait2, the env
     * override, and — queried here — the surface supporting present timing,
     * VK_KHR_present_id2, and VK_KHR_present_wait2. Sets presentTimingEnabled()
     * and the present-stage mask. Must run after the surface exists and before
     * the swapchain is created.
     */
    void detectPresentTimingSupport();

    /**
     * @brief (Re)size the swapchain's VK_EXT_present_timing result queue and
     * (re)create the async present-timing monitor against the current
     * swapchain handle. Called after the swapchain is (re)created when
     * present timing is active.
     */
    void setupPresentTimingQueue();

    /**
     * @brief Handler for VulkanPresentTimingMonitor::errorOccurred — destroys
     * the monitor on the next event-loop tick and falls back to the
     * VsyncMonitor cascade. Posted via a queued connection so the helper
     * thread is not joined from within its own signal emission.
     */
    void handlePresentTimingMonitorError();

    /**
     * @brief Handler for VulkanPresentTimingMonitor::presentTimingsReady.
     *
     * Stashes the per-stage timestamps on the in-flight OutputFrame so the
     * subsequent vblank() — driven by the immediately-following
     * vblankOccurred() emission, Qt-queued-FIFO-ordered after this one — can
     * pass them through OutputFrame::presented() into RenderLoop's CSV (and
     * later phases' adaptive logic).
     */
    void handlePresentTimingsReady(uint64_t presentId,
                                   std::chrono::nanoseconds queueOperationsEnd,
                                   std::chrono::nanoseconds firstPixelOut,
                                   std::chrono::nanoseconds firstPixelVisible);

    std::unique_ptr<OverlayWindow> m_overlayWindow;
    xcb_window_t m_window = XCB_WINDOW_NONE;
    xcb_colormap_t m_colormap = XCB_COLORMAP_NONE;

    std::unique_ptr<KWin::VulkanContext> m_context;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    std::unique_ptr<KWin::VulkanSwapchain> m_swapchain;

    X11StandaloneBackend *m_backend;
    std::unique_ptr<VulkanLayer> m_layer;
    std::shared_ptr<OutputFrame> m_frame;

    // In-flight render-time measurement. Constructed at doBeginFrame(), closed
    // and moved to the OutputFrame at doEndFrame() so RenderJournal sees a real
    // duration. CPU-clock today; QUEUE_OPERATIONS_END variant will swap in later
    // behind an env var.
    std::unique_ptr<CpuRenderTimeQuery> m_renderTimeQuery;

    // Region changed this frame, stashed by doEndFrame() for the next present()'s
    // VK_KHR_incremental_present hint. Independent of partial repaint.
    QRegion m_presentDamage;

    // Reports real vblank timestamps; present() arms it and vblank() forwards the
    // timestamp to the in-flight OutputFrame. Hardware monitor where available,
    // software (timer) fallback otherwise. Used when the present-timing monitor
    // is not active.
    std::unique_ptr<VsyncMonitor> m_vsyncMonitor;

    // VK_EXT_present_timing + VK_KHR_present_wait2 path. When active it replaces
    // the VsyncMonitor: present() tags each frame with a presentId and arms the
    // monitor; the monitor's worker thread blocks on vkWaitForPresent2KHR and
    // emits vblankOccurred() with the real on-screen timestamp drained from
    // vkGetPastPresentationTimingEXT. Same async shape as the VsyncMonitor —
    // RenderLoop sees past timestamps, not future extrapolations. Disable with
    // KWIN_VULKAN_PRESENT_TIMING=0.
    bool m_presentTimingActive = false;
    uint64_t m_nextPresentId = 1;
    std::unique_ptr<VulkanPresentTimingMonitor> m_presentTimingMonitor;

    // --- Partial repaint / manual buffer-age tracking ---
    // Enabled via KWIN_VULKAN_PARTIAL_REPAINT=1.
    bool m_partialRepaint = false;
    // Per-frame damage history, accumulated by age to form the repaint region.
    DamageJournal m_damageJournal;
    // For each swapchain image, the value of m_frameCounter when it was last
    // rendered into; 0 means "never rendered" (its contents are unusable).
    std::vector<uint64_t> m_imageLastRenderFrame;
    // Monotonic presented-frame counter; starts at 1 so 0 reads as "never".
    uint64_t m_frameCounter = 1;

    friend class X11StandaloneVulkanSurfaceTexture;
};

class X11StandaloneVulkanSurfaceTexture final : public KWin::VulkanSurfaceTextureX11
{
public:
    X11StandaloneVulkanSurfaceTexture(X11StandaloneVulkanBackend *backend, SurfacePixmapX11 *pixmap);

    bool create() override;
    void update(const QRegion &region) override;

private:
    X11StandaloneVulkanBackend *m_backend;
};

} // namespace KWin
