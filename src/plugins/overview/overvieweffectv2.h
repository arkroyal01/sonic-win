/*
    SPDX-FileCopyrightText: 2026 Ark Royal <awright42mk1@protonmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "effect/effect.h"
#include "effect/effectwindow.h" // EffectWindowVisibleRef

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

class VirtualDesktop;
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

private:
    /// Synchronous teardown — releases grabs, unregisters the post-
    /// pass, releases atlas slots, snaps activationFactor to 0 and
    /// clears m_visible. Used when an animated slide-out would race
    /// with an immediately-following state change (e.g. bar-click
    /// desktop switch, where setCurrentDesktop's OSD + effect chain
    /// fight V2's still-rendering post-pass and leave KGlobalAccel in
    /// a dead state).
    void teardownImmediate();

public:
    // Effect API
    bool isActive() const override;
    int requestedEffectChainPosition() const override;
    void reconfigure(ReconfigureFlags flags) override;
    bool borderActivated(ElectricBorder border) override;
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

#if HAVE_VULKAN
    /// Hit-test a global mouse position against the desktop bar.
    /// Returns the VirtualDesktop of the bar tile under @p globalPos,
    /// or nullptr if the cursor isn't on any bar tile. Shared by the
    /// click path (release on a bar tile switches desktop) and the
    /// drag-drop path (release on a bar tile moves the dragged
    /// window to that desktop).
    VirtualDesktop *hitTestBar(const QPoint &globalPos) const;

    /// Hit-test a global mouse position against the Add-VD tile at
    /// the trailing end of the bar. Returns true when the cursor is
    /// inside the "+" tile's NDC rect. Used by both the hover-state
    /// tracker (MouseMove) and the release path (creates a new
    /// virtual desktop and switches to it).
    bool hitTestAddTile(const QPoint &globalPos) const;

    /// Hit-test against the per-bar-tile "×" delete affordance.
    /// Returns the index into m_barTiles of the tile whose delete
    /// button is under the cursor, or -1 if none. Returns -1 when
    /// there's only one desktop (can't delete the last one).
    int hitTestDeleteAffordance(const QPoint &globalPos) const;

    /// NDC rect of the delete affordance for a bar tile whose
    /// own NDC rect is (ndcX, ndcY, ndcW, ndcH). Shared by the
    /// hit-test and the render path so the visual "×" and the
    /// click target stay in lock-step.
    QRectF deleteAffordanceNdc(float ndcX, float ndcY, float ndcW, float ndcH) const;
#endif

    /// Screen rect of the dragged tile for a given cursor position.
    /// Used by the damage-tracking path in the mouse handler so we
    /// can invalidate just the strip the tile sweeps across rather
    /// than the whole screen. Returns an empty rect if there's no
    /// drag candidate or its TileLayout isn't in m_tileLayout.
    /// @p cursor is the global mouse position to project to; the
    /// settled grid rect is computed from m_tileLayout[…].gridNdc.
    QRect draggedTileScreenRect(const QPoint &cursor) const;

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

    /// Whether activate() actually grabbed the keyboard. Skipped on
    /// empty desktops (no tiles to navigate, and the ungrab call
    /// otherwise leaves KGlobalAccel's Super+W routing dead — the
    /// keyboard focus chain has nowhere to return to without a
    /// focused window).
    bool m_grabbedKeyboard = false;

    /// Where the keyboard focus currently lives — the window grid,
    /// the desktop bar, or nowhere yet (first activation, or first
    /// nav key not yet pressed). Arrow keys move focus across zones
    /// (Up from top grid row → bar; Down from bar → top grid row),
    /// mirroring the V1 overview's "leak" behaviour.
    enum class FocusZone {
        None,
        Grid,
        Bar,
    };
    FocusZone m_focusZone = FocusZone::None;

    /// Index into the active zone's container. When FocusZone::Grid,
    /// indexes m_tileLayout. When FocusZone::Bar, indexes m_barTiles.
    /// -1 when m_focusZone == None.
    int m_focusedIndex = -1;

    /// Drag-and-drop state for "drop a grid tile onto a bar tile to
    /// move the window to that desktop". On MouseButtonPress over a
    /// grid tile we record the candidate (m_dragCandidate) and the
    /// press position but don't enter drag mode yet — a release
    /// inside a small threshold falls through to the normal click
    /// path so single-clicks keep working. Once the cursor moves
    /// past the threshold we flip m_dragActive: the source tile
    /// follows the cursor instead of sitting in its grid cell, and
    /// a bar-tile hit at MouseButtonRelease commits the move via
    /// effects->windowToDesktops.
    /// Search-filter text, accumulated from grabbedKeyboardEvent's
    /// printable-key path. While non-empty, rebuildTileLayout drops
    /// any window whose caption doesn't contain it (case-insensitive
    /// substring). Cleared on activate/deactivate, on Escape with a
    /// non-empty value (Escape with an empty value still dismisses
    /// the overview), and reset to empty when V2 is torn down.
    QString m_searchText;

    /// Last known global mouse position. Updated on every MouseMove
    /// (not just drag), so hover state on bar tiles (used to reveal
    /// the per-tile delete affordance) has a fresh cursor reading
    /// even when no button is pressed.
    QPoint m_mouseGlobal;

#if HAVE_VULKAN
    /// Pixel size of the bar tile's per-tile "×" delete affordance.
    /// Kept small so it doesn't dominate the bar tile; sized to a
    /// fixed proportion of the bar tile's height.
    static constexpr float kDeleteAffordanceFrac = 0.28f;

    /// Cached texture for the on-screen search bar. Re-rendered via
    /// QPainter → QImage → VulkanTexture::upload only when
    /// m_searchText changes (compared against m_searchRenderedText).
    /// Lifetime: created on first non-empty search, destroyed in
    /// releaseAllSlots so each overview activation starts with a
    /// fresh allocation. ~96 KB for the 800×40 cache image; the
    /// per-keystroke QPainter pass is the cheap part. The plan called
    /// out text rendering as the biggest risk (Qt's font cache might
    /// leak across activations); confine that risk to this one
    /// texture rather than letting QPainter run inside the hot loop.
    std::unique_ptr<VulkanTexture> m_searchTexture;
    QString m_searchRenderedText;
    QSize m_searchTextureSize;

    /// (Re)build m_searchTexture from m_searchText. Idempotent: skips
    /// the QPainter+upload if m_searchRenderedText already matches.
    void updateSearchTexture();

    /// Persistent icon textures for the bar's Add ("+") and Delete
    /// ("×") affordances. QPainter-rendered into transparent
    /// QImages, uploaded once per V2 lifetime — the icons don't
    /// change while the effect runs. Released in releaseAllSlots
    /// alongside m_searchTexture so VRAM goes back to zero on
    /// deactivate.
    std::unique_ptr<VulkanTexture> m_addIconTexture;
    std::unique_ptr<VulkanTexture> m_deleteIconTexture;
    void ensureBarIconTextures();
#endif

    /// Whether the cursor is currently inside the rect of the Add-VD
    /// tile and whether it's over a specific bar tile's delete
    /// affordance. Used for hover affordance reveal + drop-target
    /// styling; cleared in releaseAllSlots so a deactivate doesn't
    /// leak state into the next activation.
    bool m_addTileHover = false;
    int m_deleteAffordanceHover = -1; // index into m_barTiles, or -1
    /// Index into m_tileLayout for the grid tile currently under the
    /// pointer, or -1 when no tile is hovered. Drives the V1-parity
    /// highlight outline (Kirigami highlightColor border around the
    /// tile when hovered or keyboard-focused).
    int m_hoveredTileIndex = -1;
    /// Index into m_barTiles for the bar tile currently under the
    /// pointer. Same V1-parity outline logic as m_hoveredTileIndex
    /// but for the desktop-bar strip.
    int m_hoveredBarIndex = -1;
    /// Horizontal scroll offset for the desktop bar, in NDC X units.
    /// Matches V1's Flickable horizontal flick: when the strip is
    /// wider than the viewport (many VDs), wheel input over the bar
    /// shifts tiles left/right so off-screen ones become reachable.
    /// Clamped to keep the strip's leading/trailing edge from
    /// overshooting the viewport. Applied during rebuildBarLayout
    /// so hit-tests and rendering both see the scrolled positions.
    /// Reset on deactivate.
    float m_barScrollX = 0.0f;
    /// Strip width (sum of tile widths + gutters) in NDC X units.
    /// Computed in rebuildBarLayout; used to clamp m_barScrollX and
    /// to gate wheel input (don't react if the strip fits).
    float m_barStripWidthNdc = 0.0f;

    Window *m_dragCandidate = nullptr;
    QPoint m_dragPressGlobal;
    QPoint m_dragCurrentGlobal;
    bool m_dragActive = false;
    /// Last frame's dragged-tile screen rect. Damage tracking uses
    /// (last ∪ new) so a mouse move only invalidates the strip the
    /// tile sweeps across, not the whole screen. Empty when no
    /// previous frame applied a drag offset.
    QRect m_dragLastDamage;
    /// Pixels the cursor must move from m_dragPressGlobal before a
    /// press becomes a drag. Below this a release reverts to a
    /// regular click (activate window + deactivate overview).
    static constexpr int kDragThresholdPx = 6;
    /// Margin around the dragged tile's screen rect that gets added
    /// to the damage region — covers mip filter footprint + the
    /// focus-wash overlay so the trailing edge doesn't leave smear.
    static constexpr int kDragDamagePadPx = 8;

    /// Global toggle shortcut. Same object name as the existing
    /// OverviewEffect's `Overview` action so the user's saved binding
    /// (default `Meta+W`) carries over without reconfiguration.
    QAction *m_toggleAction = nullptr;

    /// Touchpad / touchscreen swipe-to-activate handlers. KWin's
    /// gesture API takes QActions for "gesture completed in this
    /// direction", so we keep one action per direction-half: swipe-
    /// up commits activation, swipe-down commits deactivation. The
    /// progress callback isn't wired here — that's the smooth
    /// "follow the swipe" UX V1 has via EffectTogglableState, and
    /// would need V2's animation state machine to accept an
    /// externally-driven m_activationFactor mid-flight. Tracked as a
    /// follow-up; binary trigger covers the basic gesture for now.
    QAction *m_swipeActivateAction = nullptr;
    QAction *m_swipeDeactivateAction = nullptr;

    /// Electric borders reserved for activation, read from the
    /// Effect-overview / BorderActivate config key. Matches V1's
    /// behaviour and config schema so users get the same hot-corner
    /// they configured (default: top-left). Refreshed in
    /// reconfigure() — kwin's settings panel triggers that when the
    /// user changes the border binding live.
    QList<ElectricBorder> m_borderActivate;

    /// V1 parity knob: when true, hide minimised windows from the
    /// grid and skip them in atlas reservation. Read from the
    /// Effect-overview / IgnoreMinimized config key (default false).
    bool m_ignoreMinimized = false;

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
    /// Allocate small static snapshot slots for windows on *other*
    /// desktops so the desktop-bar mini-thumbnails have something to
    /// sample. Each slot is a fixed small size (kBarThumbSize) so
    /// the atlas pressure is bounded even on heavy multi-desktop
    /// setups. EffectWindowVisibleRefs are acquired in
    /// m_barThumbVisRefs to force the WindowItem visible long
    /// enough for renderItem to produce content; they're dropped
    /// immediately after the first frame's snapshot render commits,
    /// so non-current-desktop windows don't stay force-visible (the
    /// "force visible per frame" version of phase 4b held them
    /// visible across activations and bloated VRAM to ~900 MB).
    void reserveBarThumbs();
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
    /// Sample the desktop wallpaper from m_wallpaperSlot and draw it as
    /// a fullscreen backdrop, then a dark wash on top. Bottom of V2's
    /// layer stack — windows in the live scene aren't visible because
    /// only the wallpaper is rendered. Returns false (caller should
    /// fall back to drawSceneCaptureBackground) when the slot isn't
    /// populated yet (first frame after activate, or no Desktop-type
    /// window found).
    bool drawWallpaperBackground(VkCommandBuffer cmd, const QSize &fbSize, VkFormat colorFormat);
    /// Find the Plasma wallpaper window for the current desktop /
    /// activity / output and reserve a single atlas slot for it.
    /// Called from activate() and from the live activity-change hook.
    void reserveWallpaperSlot();

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

    /// Bar mini-thumbnail slots for non-current-desktop windows.
    /// Small fixed size (kBarThumbSize), re-rendered per frame
    /// alongside the current-desktop full-size slots so bar tiles
    /// show live window content, not stale snapshots. Released on
    /// deactivate like m_windowSlots. Memory cost of holding all
    /// off-desktop WindowItems visible is recovered by the X11
    /// suspend hook (a50a7e6d1c + 1d51bf4e61) and the dedicated-
    /// allocation work (39cf11b882 + b8d35e7a0b) once the refs are
    /// dropped at deactivate.
    std::unordered_map<Window *, VulkanThumbnailAtlas::Slot> m_barThumbSlots;

    /// Atlas slots holding the desktop wallpaper. Two-level indirection
    /// so we dedup slots when Plasma uses a single sticky desktop window
    /// for all virtual desktops (the common case): one slot per unique
    /// Window* handle, plus a VD→handle map.
    ///
    /// - m_wallpaperHandleByVd: which Desktop-class Window* serves a
    ///   given VD. Multiple VDs may map to the same handle when the
    ///   wallpaper is shared (isOnDesktop is true for every VD).
    /// - m_wallpaperSlotByHandle: one atlas slot per unique handle.
    ///   We refOffscreenRendering once per slot, not per VD entry.
    ///
    /// The current desktop's slot drives the fullscreen blur backdrop;
    /// the bar pass uses the VD→handle→slot lookup per tile so per-VD
    /// wallpapers (when configured) end up in the right tile.
    std::unordered_map<VirtualDesktop *, Window *> m_wallpaperHandleByVd;
    std::unordered_map<Window *, VulkanThumbnailAtlas::Slot> m_wallpaperSlotByHandle;

    /// Per-off-desktop-window visibility refs, held for the entire
    /// overview lifetime. Without these, WindowItem::computeVisibility
    /// returns false for off-desktop windows and renderItem writes
    /// nothing into their atlas slot. Released in releaseAllSlots /
    /// the per-window destroy callback.
    std::vector<EffectWindowVisibleRef> m_barThumbVisRefs;

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

    /// Top-of-screen desktop bar: one entry per virtual desktop, in
    /// the same order as `effects->desktops()`. Click hit-test and
    /// the post-pass both consume this; `isCurrent` toggles the
    /// active-desktop highlight tint. Refreshed each frame inside the
    /// post-pass so a desktop add/remove between activations is
    /// picked up automatically.
    struct BarTile
    {
        VirtualDesktop *desktop;
        float ndcX;
        float ndcY;
        float ndcW;
        float ndcH;
        bool isCurrent;
    };
    std::vector<BarTile> m_barTiles;
    /// NDC rect of the "+" Add-Virtual-Desktop affordance at the
    /// trailing end of the bar. Always positioned, regardless of
    /// desktop count (so a user with one desktop can still create
    /// more from inside V2). Empty rect when the bar isn't shown
    /// at all (e.g. effect not initialised).
    QRectF m_addTileNdc;
    void rebuildBarLayout(const QSize &fbSize);
    void renderDesktopBar(VkCommandBuffer cmd, const QSize &fbSize);

    /// Render pass + framebuffer used by `renderWindowsToAtlas()`. The
    /// framebuffer wraps the atlas's mip-0 view; viewport + scissor at
    /// draw time restrict each window's render to its slot's sub-rect.
    /// Both lazily built on first use and torn down with the rest of
    /// the Vulkan resources on deactivation.
    std::unique_ptr<VulkanRenderPass> m_atlasRenderPass;
    std::unique_ptr<VulkanFramebuffer> m_atlasFramebuffer;

    /// One framebuffer per fallback slot — windows that didn't fit in
    /// the shared atlas get a dedicated VkImage from the atlas
    /// allocator and we wrap its mip-0 view here. Reuses
    /// m_atlasRenderPass (same format + GENERAL layouts); only the
    /// framebuffer differs. Lazily built per-slot inside
    /// renderWindowsToAtlas and torn down in releaseAllSlots / the
    /// per-window destroy callback.
    std::unordered_map<Window *, std::unique_ptr<VulkanFramebuffer>> m_fallbackFramebuffers;

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
    /// Connection to effects->currentActivityChanged, live while V2 is
    /// visible. On signal we release the activate-time slot reservation
    /// and re-reserve for the new activity so the grid stays correct.
    QMetaObject::Connection m_activityConnection;
#endif
};

} // namespace KWin
