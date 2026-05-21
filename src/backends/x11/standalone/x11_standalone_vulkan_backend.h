/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2025 Joseph <author@example.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "core/outputlayer.h"
#include "platformsupport/scenes/vulkan/vulkanbackend.h"
#include "platformsupport/scenes/vulkan/vulkancontext.h"
#include "platformsupport/scenes/vulkan/vulkansurfacetexture_x11.h"
#include "platformsupport/scenes/vulkan/vulkanswapchain.h"
#include "utils/damagejournal.h"
#include "x11eventfilter.h"

#include <vulkan/vulkan.h>
#include <xcb/xcb.h>

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

    std::unique_ptr<OverlayWindow> m_overlayWindow;
    xcb_window_t m_window = XCB_WINDOW_NONE;
    xcb_colormap_t m_colormap = XCB_COLORMAP_NONE;

    std::unique_ptr<KWin::VulkanContext> m_context;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    std::unique_ptr<KWin::VulkanSwapchain> m_swapchain;

    X11StandaloneBackend *m_backend;
    std::unique_ptr<VulkanLayer> m_layer;
    std::shared_ptr<OutputFrame> m_frame;

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
