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
    /// Off (1) means no MSAA — the cube is drawn straight into the
    /// renderer's single-sample swapchain post-FX pass. 2/4/8 enable
    /// the offscreen AA path: render into an N-sample VkImage, resolve
    /// to single-sample, composite onto the swapchain.
    int m_msaaSamples = 1;

    /// AA dispatch enum. Off is the direct render path; the rest go
    /// through the offscreen AA helpers (m_offscreenAa). MSAA is the
    /// first mode implemented; FXAA / TAA / SSAA slot into the same
    /// offscreen path with different composite shaders or jitter
    /// patterns. Enum kept distinct from m_msaaSamples so a future
    /// "FXAA on top of MSAA 4x" is a one-line extension.
    enum class AaMode {
        Off,
        MSAA2x,
        MSAA4x,
        MSAA8x,
    };
    AaMode m_aaMode = AaMode::Off;

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

    /// Shared atlas render-pass + framebuffer wrapper, lazily built
    /// on first renderDesktopsToAtlas. The atlas image stays in
    /// GENERAL layout (see VulkanRenderPass::createForAtlasWrite),
    /// so all desktop slots share one framebuffer; per-slot
    /// viewport+scissor restricts each renderItem call to its
    /// slot's atlas rect. Mirrors OverviewEffectV2's pattern.
    std::unique_ptr<VulkanFramebuffer> m_atlasFramebuffer;
    std::unique_ptr<VulkanRenderPass> m_atlasRenderPass;

    /// Per-desktop dedicated framebuffer when the atlas reserve
    /// returns a fallback slot (slot.isFallback). Fallback slots
    /// own their image; each needs its own framebuffer because
    /// the render pass is parameterised on image view. Keyed by
    /// the desktop pointer so we can drop them alongside the
    /// matching DesktopSlot in releaseAllResources.
    std::unordered_map<VirtualDesktop *, std::unique_ptr<VulkanFramebuffer>> m_fallbackFramebuffers;

    /// preFrameRender connection so each frame triggers a fresh
    /// desktop composition pass before the swapchain frame is
    /// recorded. Disconnected in releaseAllResources.
    QMetaObject::Connection m_preFrameConnection;

    /// Last frame's atlas-write submit handle. Used to fence the
    /// next frame's overwrite against the previous frame's read
    /// (same race-condition guard Overview V2 uses).
    VulkanSubmitHandle m_lastAtlasSubmit;

    /// Reserve one atlas slot per virtual desktop (sized to the
    /// compositor framebuffer), and acquire EffectWindowVisibleRefs
    /// for every window on a non-current desktop so its WindowItem
    /// renders content during renderDesktopsToAtlas. Idempotent —
    /// safe to call once per activate(). Bails if the renderer
    /// isn't Vulkan (the cube V2 has no GL path yet).
    void reserveDesktopSlots();

    /// Composite each desktop's window stack into its atlas slot.
    /// Connected to WorkspaceScene::preFrameRender from activate()
    /// so every paint sees fresh per-desktop captures.
    void renderDesktopsToAtlas();

    /// Build the cube-face graphics pipeline. Combined-image-sampler
    /// at set=0,binding=0; push constants carry per-face MVP matrix,
    /// atlas UV rect, and slide-in opacity. Compatible with the
    /// renderer's post-FX render pass (LOAD_OP_LOAD, finalLayout
    /// PRESENT_SRC_KHR). Idempotent; safe to call repeatedly.
    bool ensureVulkanPipeline(VulkanContext *ctx, VkFormat colorFormat);
    void destroyVulkanPipeline();

    /// Per-face draw callback registered with
    /// ItemRendererVulkan::registerFullscreenPostPass. Walks every
    /// desktop slot, sorts by camera-space Z, then draws each as
    /// a textured quad with per-face MVP push constants. Painter's
    /// algorithm — no depth attachment.
    void onPostPass(VkCommandBuffer cmd, VulkanTexture *sceneCapture,
                    const RenderTarget &renderTarget,
                    const RenderViewport &viewport);

    /// Per-face model matrix for desktop @p i out of @p n. Combines
    /// the unit-quad scale to fb dimensions, translation along +Z by
    /// faceDistance(n), and Y-axis rotation by `angleTick * i`.
    QMatrix4x4 faceModelMatrix(int i, int n) const;

    /// Per-face descriptor handles + caches. Mirrors the per-slot
    /// descriptor-set pattern overview V2's ensureVulkanPipeline
    /// builds: one VkPipeline, one VkPipelineLayout, one
    /// VkDescriptorSetLayout shared across all faces; each face
    /// pushes its own descriptor write before drawing.
    VkShaderModule m_vertModule = VK_NULL_HANDLE;
    VkShaderModule m_fragModule = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_vkDescriptorSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_vkPipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_vkPipeline = VK_NULL_HANDLE;
    std::unique_ptr<VulkanRenderPass> m_postPassCompatRenderPass;
    VkFormat m_pipelineColorFormat = VK_FORMAT_UNDEFINED;

    /// SkyBox-mode background pipeline. Reuses the cube-face
    /// descriptor set layout (one combined-image-sampler) but binds
    /// the loaded skybox texture instead of an atlas slot. Push
    /// constants here are inverse(P*V) + opacity, not MVP.
    VkShaderModule m_skyboxVertModule = VK_NULL_HANDLE;
    VkShaderModule m_skyboxFragModule = VK_NULL_HANDLE;
    VkPipelineLayout m_skyboxPipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_skyboxPipeline = VK_NULL_HANDLE;

    /// Build the skybox pipeline lazily on first activate with a
    /// valid m_skyboxPath. Same compat-render-pass shape as the
    /// face pipeline.
    bool ensureSkyboxPipeline(VulkanContext *ctx, VkFormat colorFormat);
    void destroySkyboxPipeline();
    /// Lazy load m_skyboxPath into m_skyboxTexture. Returns true if
    /// the texture is valid and ready to bind.
    bool ensureSkyboxTexture();

    /// Offscreen anti-aliasing resources. Built when m_aaMode != Off
    /// and torn down on deactivate. Generalises beyond MSAA: the
    /// resolveColor image is the universal "what we composite onto
    /// the swapchain" target, so future AA modes (FXAA in the
    /// composite shader, TAA with a history buffer, SSAA via larger
    /// fbSize) plug into the same scaffold.
    ///
    /// Render flow when active:
    ///   1. recordOffscreenAaPass(cmd) — render skybox + cube faces
    ///      into msaaColor (multisample) via msaaRenderPass.
    ///   2. vkCmdResolveImage from msaaColor to resolveColor (skipped
    ///      when samples == 1; SSAA path uses a blit + downscale here).
    ///   3. Transition resolveColor → SHADER_READ_ONLY_OPTIMAL.
    ///   4. composeOffscreenAaToSwapchain(cmd, …) inside the renderer
    ///      post-FX pass samples resolveColor as a fullscreen quad
    ///      via the existing cube-face pipeline (identity-ish MVP +
    ///      full UV) — this is where a future FXAA shader would
    ///      replace the pipeline.
    struct OffscreenAa
    {
        VkImage msaaColorImage = VK_NULL_HANDLE;
        // Hand-rolled VkDeviceMemory for the multisample image —
        // VulkanTexture's VMA path doesn't expose sample count, and
        // a one-off allocation per activation is cheap enough that
        // we don't need pool sharing. Direct vkAllocateMemory keeps
        // the VMA include out of the plugin.
        VkDeviceMemory msaaColorMemory = VK_NULL_HANDLE;
        VkImageView msaaColorView = VK_NULL_HANDLE;
        VkImageLayout msaaColorLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        // Single-sample resolved target, sampled by the composite
        // pass. Wraps a VulkanTexture so VRAM lifetime cleanly
        // follows the same hand-back-on-deactivate rule as the
        // atlas singleton.
        std::unique_ptr<VulkanTexture> resolveColor;

        std::unique_ptr<VulkanRenderPass> renderPass;
        std::unique_ptr<VulkanFramebuffer> framebuffer;

        VkPipeline facePipeline = VK_NULL_HANDLE;
        VkPipeline skyboxPipeline = VK_NULL_HANDLE;

        QSize size;
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        VkFormat colorFormat = VK_FORMAT_UNDEFINED;
    };
    OffscreenAa m_offscreenAa;

    /// Map m_msaaSamples → AaMode. Single source of truth for the
    /// "what mode are we in" decision; loadConfig calls it after
    /// reading the kwinrc value.
    static AaMode aaModeForSamples(int samples);

    /// Build everything in m_offscreenAa for the requested mode. No-op
    /// when m_aaMode == Off. Idempotent — safe to call from activate
    /// multiple times; tears down + rebuilds on format/size change.
    bool ensureOffscreenAa(VulkanContext *ctx, VkFormat colorFormat, const QSize &fbSize);
    void destroyOffscreenAa();

    /// Record the skybox + face draws into m_offscreenAa.framebuffer.
    /// Called once per frame from the preFrameRender callback after
    /// renderDesktopsToAtlas. After the pass ends, explicit barrier +
    /// vkCmdResolveImage hand the result over to resolveColor in
    /// SHADER_READ_ONLY_OPTIMAL layout for the composite below.
    void recordOffscreenAaPass(VkCommandBuffer cmd);

    /// Composite m_offscreenAa.resolveColor onto the swapchain target.
    /// Runs inside the renderer's post-FX pass via onPostPass when
    /// m_aaMode != Off; replaces the direct face draws. Reuses the
    /// single-sample cube-face pipeline (set to draw a screen-filling
    /// quad with identity MVP + UV(0,0,1,1)) so we don't need a
    /// dedicated blit pipeline yet — slot in an FXAA shader by
    /// branching here when that mode lands.
    void composeOffscreenAaToSwapchain(VkCommandBuffer cmd,
                                       const QSize &fbSize);

    /// Drop every per-activation GPU resource: atlas slots,
    /// visibility refs, skybox texture, atlas singleton. Pipelines
    /// + descriptor layouts stay (they're cheap to keep, expensive
    /// to recompile — same trade as overview V2).
    void releaseAllResources();
#endif // HAVE_VULKAN
};

} // namespace KWin
