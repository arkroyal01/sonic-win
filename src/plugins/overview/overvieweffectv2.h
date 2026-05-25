/*
    SPDX-FileCopyrightText: 2026 Ark Royal <awright42mk1@protonmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "effect/effect.h"

#include "config-kwin.h"

#include <QVariantAnimation>

#include <memory>
#include <unordered_map>
#include <vector>

#if HAVE_VULKAN
#include "platformsupport/scenes/vulkan/vulkancontext.h" // for VulkanSubmitHandle
#include "platformsupport/scenes/vulkan/vulkanthumbnailatlas.h"
#include <vulkan/vulkan.h>
#endif

class QAction;

namespace KWin
{

class Window;

#if HAVE_VULKAN
class VulkanContext;
class VulkanFramebuffer;
class VulkanRenderPass;
class VulkanTexture;
#endif

/**
 * @brief Rewrite of OverviewEffect that doesn't use Qt Quick.
 *
 * The existing OverviewEffect inherits from QuickSceneEffect; its UI
 * lives in `qml/main.qml` etc. and Qt's QML/Quick stack holds VRAM for
 * per-thumbnail QSGTextures, layer FBOs, glyph atlases, and scene-graph
 * batches in a per-QQuickWindow pool kwin cannot directly free. Across
 * a session, those allocations accumulate to ~750 MB per
 * [[project_kwin_frame_drop_drift_backlog]].
 *
 * OverviewEffectV2 paints everything via kwin's compositor renderer
 * (`ItemRendererVulkan`) and allocates all GPU resources through VMA.
 * Trade: re-implement layout, animation, input handling, and (later
 * phases) text rendering and drag-drop manually.
 *
 * Phase 1 (this file): subclass + minimal lifecycle. Activates a
 * translucent overlay with a slide-in alpha animation; deactivates on
 * Esc. No window thumbnails yet (phase 2) and no plugin registration
 * yet (phase 1b — caller currently can only construct this class
 * directly, not load it as a kwin effect).
 *
 * See the design plan for the
 * multi-phase plan.
 */
class OverviewEffectV2 : public Effect
{
    Q_OBJECT

public:
    OverviewEffectV2();
    ~OverviewEffectV2() override;

    /// Plugin gate: this effect only loads when `KWIN_OVERVIEW_V2=1`.
    /// The existing OverviewEffect mirrors this check so the two never
    /// run at the same time and don't fight over the same global
    /// shortcut. Phase 1b wiring; later phases may also require
    /// `effects->isVulkanCompositing()` once the renderer path is
    /// committed to Vulkan-only.
    static bool supported();

    /// Activate the effect: starts the slide-in animation.
    void activate();
    /// Deactivate: animates slide-out then releases per-activation state.
    void deactivate();

    // Effect API
    bool isActive() const override;
    int requestedEffectChainPosition() const override;
    void prePaintScreen(ScreenPrePaintData &data,
                        std::chrono::milliseconds presentTime) override;
    void paintScreen(const RenderTarget &renderTarget,
                     const RenderViewport &viewport, int mask,
                     const QRegion &region, Output *screen) override;
    void postPaintScreen() override;
    void grabbedKeyboardEvent(QKeyEvent *event) override;
    void windowInputMouseEvent(QEvent *event) override;

private:
    /// Hit-test a global mouse position against the current tile grid.
    /// Returns the Window* whose tile contains @p globalPos, or nullptr
    /// if the click is outside any tile. Recomputes the same grid the
    /// post-pass draws — keeps the two paths trivially consistent.
    Window *hitTestTile(const QPoint &globalPos) const;

    /// State machine for the slide-in/out animation. Drives
    /// `m_activationFactor` (0 = hidden, 1 = fully shown) over
    /// `m_animationDuration` ms. We use QVariantAnimation rather than
    /// QML's `NumberAnimation` because the entire point of V2 is to
    /// not depend on the Qt Quick infrastructure.
    QVariantAnimation m_animation;
    qreal m_activationFactor = 0.0;
    int m_animationDuration = 400;

    /// True while the effect is in the active phase (animating in,
    /// fully shown, or animating out). Cleared at the end of the
    /// slide-out animation; until then, paintScreen draws.
    bool m_visible = false;

    /// Global toggle shortcut. Same object name as the existing
    /// OverviewEffect's `Overview` action so the user's saved binding
    /// (default `Meta+W`) carries over without reconfiguration.
    QAction *m_toggleAction = nullptr;

#if HAVE_VULKAN
    /// Build the Vulkan pipeline used by `paintWindowTile()` to draw a
    /// textured quad sampling from `VulkanThumbnailAtlas`. Compatible
    /// with the renderer's post-FX render pass (LOAD_OP_DONT_CARE,
    /// finalLayout PRESENT_SRC_KHR). Idempotent; safe to call
    /// repeatedly. Returns false if the underlying device rejects any
    /// of the calls.
    bool ensureVulkanPipeline(VulkanContext *ctx, VkFormat colorFormat);
    void destroyVulkanPipeline();

    /// On activate(): reserve a `VulkanThumbnailAtlas` slot per window on
    /// the current virtual desktop, sized to the window's visible
    /// geometry. Slots are released on deactivation completion. The
    /// atlas singleton is shared with future consumers (switchers,
    /// window-view) so memory stays bounded.
    void reserveSlotsForCurrentDesktop();
    void releaseAllSlots();

    /// Hook for `WorkspaceScene::preFrameRender`: re-render each
    /// reserved slot's source window into its atlas region, then run
    /// the mip cascade and the publishing barrier. No-op when V2 is
    /// dormant. Phase 2b just sets the framework up; the renderItem
    /// call lands in phase 2c.
    void renderWindowsToAtlas();

    /// Body of the fullscreen-post-pass callback registered on
    /// activate. Records into Qt-free Vulkan: bind the V2 pipeline,
    /// push the atlas as descriptor, loop over slots and draw one
    /// textured quad per window. Position is a simple grid for now;
    /// proper layout (window-from-its-real-position interpolated into
    /// grid by `m_activationFactor`) is a later phase.
    void renderTilesPostPass(VkCommandBuffer cmd, const QSize &fbSize, VkFormat colorFormat);

    /// Background pass: paint the pre-effect scene contents into the
    /// swapchain as a fullscreen quad. Needed because the post-FX
    /// render pass uses LOAD_OP_DONT_CARE — without this, every pixel
    /// outside a tile would be undefined garbage. Reuses the V2 quad
    /// pipeline; only the bound texture (scene capture vs atlas) and
    /// push constants differ.
    void drawSceneCaptureBackground(VkCommandBuffer cmd, VulkanTexture *sceneCapture,
                                    const QSize &fbSize, VkFormat colorFormat);

    VulkanContext *m_vulkanCtx = nullptr;
    VkFormat m_pipelineColorFormat = VK_FORMAT_UNDEFINED;
    VkDescriptorSetLayout m_vkDescriptorSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_vkPipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_vkPipeline = VK_NULL_HANDLE;

    /// Atlas-backed slot per active source window. Created on activate,
    /// released on deactivate completion. The atlas itself is a
    /// process-wide singleton owned by `VulkanThumbnailAtlas::get(ctx)`.
    VulkanThumbnailAtlas *m_atlas = nullptr;
    std::unordered_map<Window *, VulkanThumbnailAtlas::Slot> m_windowSlots;

    /// Cached tile layout: one entry per drawable tile, in stacking
    /// order at activate-time (oldest below, freshest on top). The
    /// post-pass and the hit-test both iterate this vector so they
    /// agree on grid position. `realNdcRect` is the window's true
    /// on-screen rect in NDC (slide-in start); `gridNdcRect` is its
    /// grid cell (slide-in end). Cleared in releaseAllSlots().
    struct TileLayout
    {
        Window *handle;
        VulkanThumbnailAtlas::Slot slot;
        float realNdcX;
        float realNdcY;
        float realNdcW;
        float realNdcH;
        float gridNdcX;
        float gridNdcY;
        float gridNdcW;
        float gridNdcH;
    };
    std::vector<TileLayout> m_tileLayout;
    void rebuildTileLayout(const QSize &fbSize);

    /// Render pass + framebuffer used by `renderWindowsToAtlas()`. The
    /// framebuffer wraps the atlas's mip-0 view; viewport + scissor at
    /// draw time restrict each window's render to its slot's sub-rect.
    /// Both lazily built on first use and torn down with the rest of
    /// the Vulkan resources on deactivation.
    std::unique_ptr<VulkanRenderPass> m_atlasRenderPass;
    std::unique_ptr<VulkanFramebuffer> m_atlasFramebuffer;

    /// Tracks the previous per-frame atlas submit. Waited on before
    /// recording the next frame so the shared streaming vertex buffer's
    /// reused region is GPU-finished, matching the WindowThumbnailSource
    /// pattern. Invalidated after the wait.
    VulkanSubmitHandle m_lastAtlasSubmit;

    /// ID returned by `ItemRendererVulkan::registerFullscreenPostPass`.
    /// `-1` means no callback registered. Registered at activate time,
    /// unregistered when the slide-out animation reaches zero (so the
    /// slide-out still draws the fading tiles).
    int m_postPassId = -1;

    /// Connection to `WorkspaceScene::preFrameRender`. Active only
    /// while the effect is animating in or fully shown; disconnected at
    /// the start of slide-out so we stop scheduling atlas writes once
    /// the user dismisses.
    QMetaObject::Connection m_preFrameConnection;
#endif
};

} // namespace KWin
