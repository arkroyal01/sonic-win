/*
    SPDX-FileCopyrightText: 2026 Ark Royal <awright42mk1@protonmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "effect/effect.h"
#include "effect/effectwindow.h" // EffectWindowVisibleRef
#include "effect/globals.h" // ElectricBorder, SwipeDirection

#include "config-kwin.h"

#include <QColor>
#include <QPoint>
#include <QQuaternion>
#include <QUrl>
#include <QVariantAnimation>
#include <QVector3D>

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
 * @brief C++ rewrite of the QML cube effect, modelled on OverviewEffectV2.
 *
 * The existing CubeEffect is a scripted/QML effect rendered through
 * QtQuick3D's SceneGraph. As with the overview, the Qt Quick stack
 * holds per-session VRAM that kwin can't directly free: QSGTextures
 * for each face, the Quick3D render pipeline, layer FBOs. Per the V2
 * memory rule ([[feedback_v2_active_memory_release]]) the rewrite
 * releases everything on deactivate.
 *
 * Lessons-applied checklist from the OverviewV2 work:
 *   - Subclass Effect directly (not QuickSceneEffect).
 *   - All GPU resources allocated through VulkanThumbnailAtlas /
 *     VMA so dedicated-allocation + suspend-hook teardown apply
 *     automatically.
 *   - Per-window EffectWindowVisibleRef held during active phase
 *     so off-current-desktop windows get rendered into their
 *     atlas slot; refs dropped on deactivate.
 *   - All settings read straight from KConfigGroup (no generated
 *     kcfg wrapper needed); env-var startup log line so silence !=
 *     "is it set?".
 *   - Activation: keyboard shortcut + electric border + touchpad /
 *     touchscreen swipe — wire each via the Effect API in the
 *     constructor and refresh on reconfigure().
 *
 * Cube-specific design notes:
 *
 *  1. **Geometry.** N virtual desktops → N face quads arranged
 *     radially around the Y axis. Face dimensions match the
 *     compositor framebuffer aspect; their distance from the
 *     centre is `(faceW/2)/tan(angleTick/2) + faceDisplacement`,
 *     matching V1's formula.
 *
 *  2. **Camera.** Stored as a yaw / pitch / radius triple in
 *     m_cameraTarget; mouse drag and wheel update it; the visual
 *     m_cameraCurrent lerps toward m_cameraTarget every frame
 *     (no QVariantAnimation per channel — saves the timer plumbing
 *     and lets multi-input gestures coalesce). Aspect-correct
 *     perspective projection assembled in updateViewProj() once
 *     per frame.
 *
 *  3. **Rendering order — painter's algorithm.** Sort face quads
 *     by camera-space Z descending and draw back-to-front. No
 *     depth attachment in the post-FX pass → don't need one. The
 *     V1 path uses Quick3D's depth buffer; ours skips that whole
 *     attachment.
 *
 *  4. **Background.** Two modes (V1 parity):
 *       - **Color** — clear/clear-region the post-FX target with
 *         the configured color before drawing faces. Implementation
 *         option: render a full-screen tinted quad with the same
 *         pipeline, factor=1, atlas binding ignored (tintRgba.a=1
 *         path that overview V2 already uses).
 *       - **SkyBox** — sample a 2D equirectangular texture (V1
 *         loads via QtQuick.Texture so the SkyBox URL is a flat
 *         image). For V2 we'll load via QImage + VulkanTexture
 *         and sample with view-direction → equirect UV in the
 *         shader. Cubemap-formatted skyboxes are a follow-up.
 *
 *  5. **Picking.** On click we ray-cast against the N face planes
 *     in camera space and pick the closest hit. The angle of the
 *     hit point relative to the cube centre maps back to a desktop
 *     index. No GPU readback needed.
 *
 *  6. **Input parity with V1.**
 *       - Drag: rotate (yaw delta from X, pitch delta from Y;
 *         MouseInvertedX/Y from config).
 *       - Wheel: zoom (radius * (1 ± 0.1 * delta)).
 *       - Click without drag → switch to face under cursor; click
 *         on background → switch to currently centred face.
 *       - Left/Right → snap rotation to neighbouring face.
 *       - Enter/Space → activate currently centred face.
 *       - Esc → deactivate.
 *
 *  7. **Memory hygiene.**
 *       - One full-size atlas slot per desktop, sized to the
 *         compositor framebuffer.
 *       - Each window on a non-current desktop gets an
 *         EffectWindowVisibleRef during active phase so its
 *         WindowItem renders into its desktop's atlas slot.
 *       - On deactivate: release every slot, drop refs, free the
 *         SkyBox texture, drop the persistent atlas singleton (V2
 *         atlas hand-off pattern). VRAM should return to baseline
 *         like Overview V2 does.
 *
 * Multi-phase rollout — same pattern as Overview V2:
 *
 *   - **Phase 1 (this draft).** Class scaffolding + plugin
 *     registration + lifecycle (activate/deactivate/reconfigure)
 *     + input wiring + camera math. paintScreen is a stub that
 *     does nothing visible — the effect activates and deactivates
 *     cleanly without rendering yet.
 *   - **Phase 2.** Atlas-slot reservation per desktop + per-window
 *     visibility refs + the offscreen rendering of each desktop
 *     into its slot.
 *   - **Phase 3.** Vulkan pipeline + per-face quad rendering with
 *     painter's algorithm. No background yet (transparent over
 *     captured scene).
 *   - **Phase 4.** Background color + SkyBox sampling.
 *   - **Phase 5.** Animation polish (slide-in factor), gesture
 *     parity (touchpad/touchscreen swipe to activate), settings
 *     panel reconciliation with V1's kcm.
 *
 * @see /home/user/devel/claude/sonic-win/src/plugins/overview/overvieweffectv2.h
 *      for the parallel V2 design that proved this approach.
 */
class CubeEffectV2 : public Effect
{
    Q_OBJECT

public:
    CubeEffectV2();
    ~CubeEffectV2() override;

    /// Plugin gate: this effect only loads when `KWIN_CUBE_V2=1`. The
    /// existing scripted CubeEffect mirrors this check (Phase 1b — to
    /// be added in the cube package) so both never load at once and
    /// don't fight over Meta+C.
    static bool supported();

    /// Activate the effect: starts the slide-in animation. Reserves
    /// per-desktop atlas slots + per-window visibility refs (Phase 2).
    void activate();
    /// Deactivate: animates slide-out then releases per-activation
    /// state. After the animation completes, releaseAllResources()
    /// drops every GPU allocation per the V2 active-memory rule.
    void deactivate();

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
    /// Synchronous teardown — releases grabs, drops atlas slots,
    /// snaps activationFactor to 0, clears m_visible. Used when an
    /// animated slide-out would race a following state change (face
    /// click → setCurrentDesktop, same problem Overview V2 hit).
    void teardownImmediate();

    /// Read kwinrc[Effect-cube] into the m_* config members. Called
    /// from reconfigure(). Also re-reserves electric borders so a
    /// live kcm change picks up without a kwin restart.
    void loadConfig();

    /// Recompute view+projection matrices from m_cameraCurrent and
    /// the current viewport size. Called once per paintScreen before
    /// the per-face draws.
    void updateViewProjection(const QSize &fbSize);

    /// Lerp m_cameraCurrent toward m_cameraTarget every frame. Returns
    /// true if anything changed (caller schedules another repaint).
    bool stepCameraInterpolation(std::chrono::milliseconds presentTime);

    /// Distance from cube centre to each face's centre, given the
    /// number of desktops and the current m_faceDisplacement. Matches
    /// V1's QML formula in Cube.qml.
    qreal faceDistance(int desktopCount) const;

    /// Hit-test against all face planes given a viewport-relative
    /// (x, y) cursor position. Returns -1 if nothing hit, otherwise
    /// the desktop index of the picked face. Used by mouse-click
    /// release for the desktop-switch path.
    int hitTestFace(const QPoint &cursorViewport, const QSize &fbSize) const;

    /// Snap m_cameraTarget yaw to the desktop one step to the
    /// {left,right} of currently-centred. Used by Left/Right keys.
    void snapToAdjacentDesktop(int delta);

    /// Desktop currently closest to the camera's yaw — the one Enter
    /// / Space / background-click would switch to.
    VirtualDesktop *centredDesktop() const;

    /// Animation state. Drives `m_activationFactor` (0 = hidden, 1 =
    /// fully shown) over `m_animationDuration` ms. Only the alpha
    /// fade rides this — camera rotation has its own per-frame
    /// interpolation (m_cameraCurrent → m_cameraTarget) so multi-
    /// finger gestures coalesce cleanly.
    QVariantAnimation m_animation;
    qreal m_activationFactor = 0.0;
    int m_animationDuration = 400;

    /// True while the effect is in the active phase (animating in,
    /// fully shown, or animating out). Cleared at the end of the
    /// slide-out animation.
    bool m_visible = false;

    /// Whether activate() actually grabbed the keyboard. Mirrors
    /// Overview V2's grab safety — on a single-desktop session,
    /// there's nothing to switch between and we render the
    /// "placeholder" V1 used; no grab to avoid the KGlobalAccel
    /// stall.
    bool m_grabbedKeyboard = false;

    /// Global toggle shortcut. Same object name as the existing
    /// CubeEffect's `Cube` action so the user's saved binding
    /// (default `Meta+C`) carries over without reconfiguration.
    QAction *m_toggleAction = nullptr;

    /// Touchpad / touchscreen swipe activation handlers — same
    /// binary-trigger model Overview V2 uses. 4-finger touchpad
    /// swipe-up activates; swipe-down deactivates. Touchscreen
    /// uses 3 fingers. Progress-driven UX is deferred (would need
    /// the animation state machine to accept an externally driven
    /// activation factor mid-flight).
    QAction *m_swipeActivateAction = nullptr;
    QAction *m_swipeDeactivateAction = nullptr;

    /// Camera state. Yaw azimuth around Y (0 = facing first
    /// desktop's face); pitch elevation around X, clamped to
    /// ±kMaxPitchDeg; radius is the orbital distance from the
    /// cube centre. Drag/wheel update m_cameraTarget; per-frame
    /// stepCameraInterpolation() drifts m_cameraCurrent toward it
    /// so input + animation share a single eased path.
    struct CameraState
    {
        qreal yawDeg = 0.0;
        qreal pitchDeg = 0.0;
        qreal radius = 0.0;
    };
    CameraState m_cameraTarget;
    CameraState m_cameraCurrent;
    /// Saved on press, used so drag-motion deltas are deterministic
    /// against the press anchor rather than chasing the last frame
    /// (the V1 path used FrameAnimation polling — we use direct
    /// deltas).
    CameraState m_cameraPressStart;
    QPoint m_mousePressGlobal;
    QPoint m_mouseCurrentGlobal;
    bool m_mouseDragging = false;

    /// Picked face hit-test state, refreshed on every MouseMove so
    /// the post-pass can highlight the currently-hovered face when
    /// Phase 3 rendering arrives.
    int m_hoverFaceIndex = -1;

    /// Last presentTime fed to prePaintScreen — used by
    /// stepCameraInterpolation() to compute per-frame deltas for
    /// the lerp.
    std::chrono::milliseconds m_lastPresentTime{0};

    /// Stored MVP for paintScreen + hitTestFace consistency. Both
    /// read this; updateViewProjection writes it. View + projection
    /// only; per-face model lives in the face draw push-constants.
    struct ViewProj
    {
        QMatrix4x4 view;
        QMatrix4x4 projection;
        qreal aspect = 1.0;
    };
    ViewProj m_viewProj;

    // -- Configuration, read in loadConfig() from kwinrc[Effect-cube] --
    // (match V1's main.xml schema so kwinrc files carry over verbatim)

    QList<ElectricBorder> m_borderActivate;
    QList<ElectricBorder> m_touchBorderActivate;
    qreal m_faceDisplacement = 100.0;
    qreal m_distanceFactor = 1.75;
    bool m_mouseInvertedX = false;
    bool m_mouseInvertedY = false;
    enum class Background {
        Color,
        SkyBox,
    };
    Background m_backgroundMode = Background::Color;
    QColor m_backgroundColor{0x21, 0x24, 0x27};
    QUrl m_skyboxPath;

    /// V2-only knob: MSAA sample count for the cube post-FX pass.
    /// Off (1) means no MSAA. V1 has no equivalent setting; the
    /// scripted cube renders through QtQuick3D and inherits whatever
    /// the SceneGraph default is. Stored as VkSampleCountFlagBits-
    /// compatible int (1, 2, 4, or 8) so Phase 4's renderpass code
    /// can feed it straight to vkCreateRenderPass without remapping.
    int m_msaaSamples = 1;

    static constexpr qreal kMaxPitchDeg = 30.0;
    static constexpr qreal kMinPitchDeg = -30.0;
    /// Pixels the cursor must travel before a press becomes a drag
    /// (below this, release is a click — face pick / switch).
    static constexpr int kDragThresholdPx = 6;
    /// Interpolation half-life in ms — m_cameraCurrent reaches
    /// 50% of the gap each kCameraEaseHalfLifeMs. Independent of
    /// frame rate (uses presentTime delta).
    static constexpr int kCameraEaseHalfLifeMs = 90;

#if HAVE_VULKAN
    /// Per-desktop atlas slot for the desktop's full-resolution
    /// rendered content. Reserved at activate(), released at
    /// deactivate(). Mirrors OverviewEffectV2's m_windowSlots in
    /// spirit but one entry *per desktop*, not per window — cube
    /// composites whole desktops, not individual windows.
    struct DesktopSlot
    {
        VulkanThumbnailAtlas::Slot slot;
        bool hasContent = false;
    };
    std::unordered_map<VirtualDesktop *, DesktopSlot> m_desktopSlots;

    /// EffectWindowVisibleRefs for every window on a non-current
    /// desktop while V2 is active, so its WindowItem produces
    /// content during the per-desktop atlas render. Cleared on
    /// deactivate.
    std::vector<EffectWindowVisibleRef> m_visibilityRefs;

    /// Persistent SkyBox texture (V1 parity). Loaded on demand
    /// when m_backgroundMode == SkyBox and m_skyboxPath is valid;
    /// destroyed in releaseAllResources() so deactivate hands
    /// VRAM back. Re-uploaded on the next activate if still
    /// configured.
    std::unique_ptr<VulkanTexture> m_skyboxTexture;

    /// Atlas pointer + context handle — same lazy-init pattern
    /// Overview V2 uses. Both nulled out during teardown.
    VulkanThumbnailAtlas *m_atlas = nullptr;
    VulkanContext *m_vulkanCtx = nullptr;

    /// Renderer post-pass registration id, returned by
    /// ItemRendererVulkan::registerFullscreenPostPass. -1 when no
    /// post-pass is active. Unregistered before releaseAllResources()
    /// so the renderer doesn't try to sample dropped slots.
    int m_postPassId = -1;

    /// Build the cube-face graphics pipeline. Same shape as the
    /// overview pipeline: combined-image-sampler binding 0, push
    /// constants carrying per-face model matrix + atlas UV rect.
    /// The fragment shader samples the atlas slot through the
    /// SRGB view (same as overview); blends premultiplied. Phase
    /// 3 lands the actual SPIR-V.
    bool ensureVulkanPipeline(VulkanContext *ctx, VkFormat colorFormat);
    void destroyVulkanPipeline();

    /// Render every desktop into its atlas slot. Called from the
    /// scene preFrameRender so the slots are populated before the
    /// post-pass tries to sample them. Mirrors
    /// OverviewEffectV2::renderWindowsToAtlas but at the desktop
    /// granularity. Phase 2.
    void renderDesktopsToAtlas();

    /// Drop every per-activation GPU resource: atlas slots,
    /// visibility refs, skybox texture, atlas singleton. Pipelines
    /// + descriptor layouts stay (they're cheap to keep, expensive
    /// to recompile — same trade as overview V2).
    void releaseAllResources();
#endif // HAVE_VULKAN
};

} // namespace KWin
