/*
    SPDX-FileCopyrightText: 2026 Ark Royal <awright42mk1@protonmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "cubeeffectv2.h"

#include "effect/effecthandler.h"
#include "effect/effectwindow.h"

#include "virtualdesktops.h"

#if HAVE_VULKAN
#include "platformsupport/scenes/vulkan/vulkantexture.h"
#endif

#include <KConfigGroup>
#include <KGlobalAccel>
#include <KLocalizedString>
#include <KSharedConfig>

#include <QAction>
#include <QEasingCurve>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLoggingCategory>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QtMath>

namespace KWin
{

Q_LOGGING_CATEGORY(KWIN_CUBE_V2_LOG, "kwin_cube_v2", QtWarningMsg)

namespace
{
constexpr int kRequiredEffectChainPosition = 50;

/// Pixels-per-degree drag sensitivity. Tuned to match V1's
/// QtQuick CubeCameraController feel (xSpeed/ySpeed=10 at 60Hz
/// dt=16ms, integrated over a 200ms press): ~1.25 px/deg.
constexpr qreal kYawPixelsPerDegree = 4.0;
constexpr qreal kPitchPixelsPerDegree = 5.0;

/// Wheel scaling. V1 uses radius *= (1 + 0.1 * delta) with delta
/// = angleDelta.y * 0.01. One wheel notch = 120 angle units, so
/// each notch adjusts radius by ~12%. Match that here so kwinrc
/// values map identically.
constexpr qreal kWheelRadiusFactor = 0.12;
} // namespace

bool CubeEffectV2::supported()
{
    // Vulkan-only: the V2 renderer (atlas, Vulkan pipeline, post-pass —
    // Phase 3+) bails on a non-Vulkan compositor, so loading under OpenGL
    // would grab input and draw nothing. Off by default
    // (EnabledByDefault=false in v2/metadata.json) and with a distinct name +
    // shortcut (see the ctor), so it coexists with the scripted V1 cube
    // without colliding; enable "Desktop Cube (V2)" in Desktop Effects to use
    // it. No env-var gate — that made supported() answer differently in the
    // KCM process (no env) than in kwin, hiding V2 from Desktop Effects while
    // the hot-corner KCM (which keys off enabled-state) still showed it.
    return effects && effects->isVulkanCompositing();
}

CubeEffectV2::CubeEffectV2()
{
    // One-shot startup log so it's obvious which cube is active.
    qCWarning(KWIN_CUBE_V2_LOG)
        << "CubeEffectV2: loaded (Vulkan, opt-in via Desktop Effects). C++ "
           "rewrite of the scripted cube effect — Phase 1 (lifecycle only).";

    m_animation.setDuration(m_animationDuration);
    m_animation.setEasingCurve(QEasingCurve::OutCubic);
    m_animation.setStartValue(qreal(0.0));
    m_animation.setEndValue(qreal(1.0));

    connect(&m_animation, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
        m_activationFactor = v.toReal();
        if (effects) {
            effects->addRepaintFull();
        }
    });
    connect(&m_animation, &QVariantAnimation::finished, this, [this]() {
        if (qFuzzyCompare(m_activationFactor, 0.0)) {
            m_visible = false;
#if HAVE_VULKAN
            releaseAllResources();
#endif
            if (effects) {
                effects->addRepaintFull();
            }
        }
    });

    // Global toggle. Distinct object name + default key from the scripted V1
    // cube ("Cube" / Meta+C) so the two never collide while both are present
    // during the staged rewrite — V1 keeps Meta+C, V2 takes Meta+Shift+C.
    // (At final cutover, when V1 is removed, V2 can reclaim the "Cube" name
    // and Meta+C so saved bindings carry over.)
    m_toggleAction = new QAction(this);
    m_toggleAction->setObjectName(QStringLiteral("CubeV2"));
    m_toggleAction->setText(i18nc("@action", "Toggle Desktop Cube (V2)"));
    m_toggleAction->setAutoRepeat(false);
    const QKeySequence defaultShortcut = Qt::META | Qt::SHIFT | Qt::Key_C;
    KGlobalAccel::self()->setDefaultShortcut(m_toggleAction, {defaultShortcut});
    KGlobalAccel::self()->setShortcut(m_toggleAction, {defaultShortcut});
    connect(m_toggleAction, &QAction::triggered, this, [this]() {
        if (m_visible) {
            deactivate();
        } else {
            activate();
        }
    });

    // Touchpad / touchscreen swipe activation. Binary trigger model
    // — matches Overview V2's pragma (progress-driven UX deferred).
    m_swipeActivateAction = new QAction(this);
    connect(m_swipeActivateAction, &QAction::triggered, this, [this]() {
        if (!m_visible) {
            activate();
        }
    });
    m_swipeDeactivateAction = new QAction(this);
    connect(m_swipeDeactivateAction, &QAction::triggered, this, [this]() {
        if (m_visible) {
            deactivate();
        }
    });
    if (effects) {
        effects->registerTouchpadSwipeShortcut(SwipeDirection::Up, 4, m_swipeActivateAction, {});
        effects->registerTouchpadSwipeShortcut(SwipeDirection::Down, 4, m_swipeDeactivateAction, {});
        effects->registerTouchscreenSwipeShortcut(SwipeDirection::Up, 3, m_swipeActivateAction, {});
        effects->registerTouchscreenSwipeShortcut(SwipeDirection::Down, 3, m_swipeDeactivateAction, {});
    }

    reconfigure(ReconfigureAll);
}

CubeEffectV2::~CubeEffectV2()
{
    if (m_toggleAction) {
        KGlobalAccel::self()->removeAllShortcuts(m_toggleAction);
    }
    if (effects) {
        for (const ElectricBorder border : std::as_const(m_borderActivate)) {
            effects->unreserveElectricBorder(border, this);
        }
        // Touch borders are bound to QActions (registerTouchBorder),
        // not directly to the Effect. Phase 5 will wire that path so
        // V1's TouchBorderActivate config carries over; for the
        // draft only pointer hot-corners and gesture swipes activate.
    }
#if HAVE_VULKAN
    releaseAllResources();
    destroyVulkanPipeline();
#endif
}

void CubeEffectV2::reconfigure(ReconfigureFlags flags)
{
    Q_UNUSED(flags);
    loadConfig();
}

void CubeEffectV2::loadConfig()
{
    const auto cfg = KSharedConfig::openConfig();
    const KConfigGroup group = cfg->group(QStringLiteral("Effect-cube"));

    // Re-reserve electric borders so a live kcm save picks up
    // without a kwin restart.
    if (effects) {
        for (const ElectricBorder border : std::as_const(m_borderActivate)) {
            effects->unreserveElectricBorder(border, this);
        }
    }
    m_borderActivate.clear();
    m_touchBorderActivate.clear();

    const QList<int> borders = group.readEntry(QStringLiteral("BorderActivate"), QList<int>{});
    const QList<int> touchBorders = group.readEntry(QStringLiteral("TouchBorderActivate"), QList<int>{});
    if (effects) {
        for (const int border : borders) {
            const ElectricBorder eb = ElectricBorder(border);
            m_borderActivate.append(eb);
            effects->reserveElectricBorder(eb, this);
        }
        // TouchBorderActivate: deferred to Phase 5 (registerTouchBorder
        // takes a QAction, not the effect; needs per-border bookkeeping
        // since each border binding wants its own QAction). Store the
        // configured list so the eventual wiring path has it ready.
        for (const int border : touchBorders) {
            m_touchBorderActivate.append(ElectricBorder(border));
        }
    }

    m_faceDisplacement = group.readEntry(QStringLiteral("CubeFaceDisplacement"), 100.0);
    m_distanceFactor = group.readEntry(QStringLiteral("DistanceFactor"), 1.75);
    m_mouseInvertedX = group.readEntry(QStringLiteral("MouseInvertedX"), false);
    m_mouseInvertedY = group.readEntry(QStringLiteral("MouseInvertedY"), false);
    m_skyboxPath = group.readEntry(QStringLiteral("SkyBox"), QUrl());
    const QString bg = group.readEntry(QStringLiteral("Background"), QStringLiteral("Color"));
    m_backgroundMode = (bg == QLatin1String("SkyBox")) ? Background::SkyBox : Background::Color;
    m_backgroundColor = group.readEntry(QStringLiteral("BackgroundColor"), QColor(0x21, 0x24, 0x27));

    // MSAA enum: Off / X2 / X4 / X8 → 1 / 2 / 4 / 8 samples. Default
    // Off so existing kwinrc files (which don't have this key) get
    // the same render path as V1 had. Phase 4 wires this into the
    // render-pass creation and adds the resolve attachment.
    const QString msaa = group.readEntry(QStringLiteral("MSAA"), QStringLiteral("Off"));
    if (msaa == QLatin1String("X2")) {
        m_msaaSamples = 2;
    } else if (msaa == QLatin1String("X4")) {
        m_msaaSamples = 4;
    } else if (msaa == QLatin1String("X8")) {
        m_msaaSamples = 8;
    } else {
        m_msaaSamples = 1;
    }
}

bool CubeEffectV2::borderActivated(ElectricBorder border)
{
    if (!m_borderActivate.contains(border)) {
        return false;
    }
    if (m_visible) {
        deactivate();
    } else {
        activate();
    }
    return true;
}

bool CubeEffectV2::isActive() const
{
    return m_visible;
}

int CubeEffectV2::requestedEffectChainPosition() const
{
    return kRequiredEffectChainPosition;
}

qreal CubeEffectV2::faceDistance(int desktopCount) const
{
    if (desktopCount <= 0 || !effects) {
        return 0.0;
    }
    // V1: (faceW/2) / tan(angleTick/2) + faceDisplacement.
    // faceW = framebuffer width (treated as the world-space face side).
    const QSize fb = effects->virtualScreenSize();
    const qreal faceW = qreal(fb.width());
    const qreal angleTickDeg = 360.0 / qreal(desktopCount);
    const qreal halfAngleRad = qDegreesToRadians(angleTickDeg * 0.5);
    if (std::tan(halfAngleRad) < 1e-6) {
        return m_faceDisplacement;
    }
    return (faceW * 0.5) / std::tan(halfAngleRad) + m_faceDisplacement;
}

void CubeEffectV2::activate()
{
    if (!effects) {
        return;
    }
    if (m_visible && m_animation.direction() == QVariantAnimation::Forward) {
        return;
    }
    m_visible = true;

    // Camera "close" preset matches V1's State name="close": orbit
    // radius is far enough that the centred face fills the view at
    // the camera's vertical FOV. We default to 45° FOV for the
    // initial perspective. Yaw points at the current desktop.
    const auto desktops = effects->desktops();
    const int n = int(desktops.size());
    const qreal angleTickDeg = (n > 0) ? (360.0 / n) : 0.0;
    VirtualDesktop *current = effects->currentDesktop();
    int currentIndex = 0;
    if (current) {
        for (int i = 0; i < n; ++i) {
            if (desktops[i] == current) {
                currentIndex = i;
                break;
            }
        }
    }
    const qreal startYaw = angleTickDeg * currentIndex;
    const qreal faceDist = faceDistance(n);
    const qreal fovHalfRad = qDegreesToRadians(45.0 * 0.5);
    const qreal closeRadius = faceDist
        + (effects->virtualScreenSize().height() * 0.5) / std::tan(fovHalfRad);
    const qreal distantRadius = faceDist * m_distanceFactor
        + (effects->virtualScreenSize().height() * 0.5) / std::tan(fovHalfRad);

    // Start the camera at the close preset, animate toward the
    // distant preset (matching V1's state transition).
    m_cameraCurrent = {startYaw, 0.0, closeRadius};
    m_cameraTarget = {startYaw, -20.0, distantRadius};

    // Keyboard + mouse grab so Esc/arrows/clicks hit us, not the
    // windows behind the cube.
    effects->startMouseInterception(this, Qt::ArrowCursor);
    m_grabbedKeyboard = effects->grabKeyboard(this);

    if (m_animation.state() == QVariantAnimation::Running
        && m_animation.direction() == QVariantAnimation::Backward) {
        m_animation.stop();
    }
    m_animation.setDirection(QVariantAnimation::Forward);
    m_animation.setDuration(m_animationDuration);
    m_animation.start();
    effects->addRepaintFull();
}

void CubeEffectV2::deactivate()
{
    if (!m_visible) {
        return;
    }
    if (m_animation.state() == QVariantAnimation::Running
        && m_animation.direction() == QVariantAnimation::Backward) {
        return;
    }
    if (effects && m_grabbedKeyboard) {
        effects->ungrabKeyboard();
        m_grabbedKeyboard = false;
    }
    if (effects) {
        effects->stopMouseInterception(this);
    }
    m_animation.setDirection(QVariantAnimation::Backward);
    m_animation.setDuration(m_animationDuration);
    m_animation.start();
}

void CubeEffectV2::teardownImmediate()
{
    if (effects && m_grabbedKeyboard) {
        effects->ungrabKeyboard();
        m_grabbedKeyboard = false;
    }
    if (effects) {
        effects->stopMouseInterception(this);
    }
    if (m_animation.state() == QVariantAnimation::Running) {
        m_animation.stop();
    }
    m_activationFactor = 0.0;
    m_visible = false;
#if HAVE_VULKAN
    releaseAllResources();
#endif
    if (effects) {
        effects->addRepaintFull();
    }
}

void CubeEffectV2::updateViewProjection(const QSize &fbSize)
{
    if (fbSize.isEmpty()) {
        return;
    }
    const qreal aspect = qreal(fbSize.width()) / qreal(fbSize.height());

    // Orbital camera position: spherical coordinates.
    // theta = elevation from XZ-plane; phi = azimuth about Y.
    const qreal theta = qDegreesToRadians(m_cameraCurrent.pitchDeg + 90.0);
    const qreal phi = qDegreesToRadians(m_cameraCurrent.yawDeg);
    const qreal r = m_cameraCurrent.radius;

    const QVector3D eye(float(r * std::sin(phi) * std::sin(theta)),
                        float(r * std::cos(theta)),
                        float(r * std::cos(phi) * std::sin(theta)));
    const QVector3D centre(0, 0, 0);
    const QVector3D up(0, 1, 0);

    m_viewProj.view.setToIdentity();
    m_viewProj.view.lookAt(eye, centre, up);

    m_viewProj.projection.setToIdentity();
    // 45° vertical FOV, near/far chosen to comfortably bracket the
    // face distance range (matches V1's PerspectiveCamera clipNear=10,
    // clipFar=100000).
    m_viewProj.projection.perspective(45.0f, float(aspect), 10.0f, 100000.0f);

    // Vulkan-style Y-flip handled in the shader via gl_Position.y
    // negation, same as the Overview V2 pipeline ([[project_vulkan_y_flip_viewport]]).

    m_viewProj.aspect = aspect;
}

bool CubeEffectV2::stepCameraInterpolation(std::chrono::milliseconds presentTime)
{
    if (m_lastPresentTime.count() == 0) {
        m_lastPresentTime = presentTime;
        m_cameraCurrent = m_cameraTarget;
        return false;
    }
    const auto deltaMs = presentTime - m_lastPresentTime;
    m_lastPresentTime = presentTime;
    if (deltaMs.count() <= 0) {
        return false;
    }
    // Exponential ease: factor = 1 - 0.5^(dt / halfLife). Frame-rate
    // independent and matches the perceptual feel of QML's Behavior
    // on rotation with OutCubic.
    const qreal factor = 1.0 - std::pow(0.5, qreal(deltaMs.count()) / kCameraEaseHalfLifeMs);
    auto lerp = [factor](qreal &cur, qreal target) {
        const qreal next = cur + (target - cur) * factor;
        const bool changed = std::abs(next - cur) > 1e-3;
        cur = next;
        return changed;
    };
    bool changed = false;
    changed |= lerp(m_cameraCurrent.yawDeg, m_cameraTarget.yawDeg);
    changed |= lerp(m_cameraCurrent.pitchDeg, m_cameraTarget.pitchDeg);
    changed |= lerp(m_cameraCurrent.radius, m_cameraTarget.radius);
    return changed;
}

void CubeEffectV2::prePaintScreen(ScreenPrePaintData &data,
                                  std::chrono::milliseconds presentTime)
{
    if (m_visible) {
        if (stepCameraInterpolation(presentTime)) {
            if (effects) {
                effects->addRepaintFull();
            }
        }
        data.mask |= Effect::PAINT_SCREEN_TRANSFORMED | Effect::PAINT_SCREEN_BACKGROUND_FIRST;
    }
    effects->prePaintScreen(data, presentTime);
}

void CubeEffectV2::paintScreen(const RenderTarget &renderTarget,
                               const RenderViewport &viewport, int mask,
                               const QRegion &region, Output *screen)
{
    // Phase 1: no rendering yet. The wrapped chain still paints the
    // windows behind us; we'll draw the cube on top once the Vulkan
    // pipeline lands in Phase 3. Until then, an active CubeEffectV2
    // is visually a no-op — but mouse/keyboard grab, gestures, and
    // animation lifecycle still work end-to-end. Phase 1 acceptance
    // = activate, drag, release, deactivate cycle without crashes.
    effects->paintScreen(renderTarget, viewport, mask, region, screen);
}

void CubeEffectV2::postPaintScreen()
{
    effects->postPaintScreen();
}

void CubeEffectV2::grabbedKeyboardEvent(QKeyEvent *event)
{
    if (event->type() != QEvent::KeyPress) {
        return;
    }
    switch (event->key()) {
    case Qt::Key_Escape:
        deactivate();
        return;
    case Qt::Key_Left:
        snapToAdjacentDesktop(-1);
        return;
    case Qt::Key_Right:
        snapToAdjacentDesktop(+1);
        return;
    case Qt::Key_Enter:
    case Qt::Key_Return:
    case Qt::Key_Space:
        if (VirtualDesktop *target = centredDesktop()) {
            if (effects && target != effects->currentDesktop()) {
                effects->setCurrentDesktop(target);
            }
            deactivate();
        }
        return;
    default:
        break;
    }
}

void CubeEffectV2::snapToAdjacentDesktop(int delta)
{
    if (!effects) {
        return;
    }
    const int n = int(effects->desktops().size());
    if (n <= 1) {
        return;
    }
    const qreal angleTickDeg = 360.0 / qreal(n);
    // V1 picks the next/previous tick from the current yaw, with a
    // small bias so a near-aligned camera doesn't no-op (the 0.05*
    // tick threshold from CubeCameraController.qml). Mirror that
    // here so the snap-and-release feel matches.
    const qreal current = m_cameraTarget.yawDeg;
    qreal next;
    if (delta < 0) {
        next = std::floor(current / angleTickDeg) * angleTickDeg;
        if (std::abs(next - current) < 0.05 * angleTickDeg) {
            next -= angleTickDeg;
        }
    } else {
        next = std::ceil(current / angleTickDeg) * angleTickDeg;
        if (std::abs(next - current) < 0.05 * angleTickDeg) {
            next += angleTickDeg;
        }
    }
    m_cameraTarget.yawDeg = next;
    if (effects) {
        effects->addRepaintFull();
    }
}

VirtualDesktop *CubeEffectV2::centredDesktop() const
{
    if (!effects) {
        return nullptr;
    }
    const auto desktops = effects->desktops();
    const int n = int(desktops.size());
    if (n == 0) {
        return nullptr;
    }
    const qreal angleTickDeg = 360.0 / qreal(n);
    qreal yaw = std::fmod(m_cameraCurrent.yawDeg, 360.0);
    if (yaw < 0) {
        yaw += 360.0;
    }
    int index = int(std::lround(yaw / angleTickDeg)) % n;
    if (index < 0) {
        index += n;
    }
    return desktops[index];
}

int CubeEffectV2::hitTestFace(const QPoint &cursorViewport, const QSize &fbSize) const
{
    Q_UNUSED(cursorViewport);
    Q_UNUSED(fbSize);
    // Phase 3 will implement true ray/plane intersection in camera
    // space. For Phase 1 we fall back to the centred-desktop
    // approximation: any click counts as "switch to currently
    // centred face", which matches V1's MouseArea-onClicked when
    // the picker misses.
    return -1;
}

void CubeEffectV2::windowInputMouseEvent(QEvent *event)
{
    if (!m_visible) {
        return;
    }
    auto *mouseEvent = dynamic_cast<QMouseEvent *>(event);
    if (!mouseEvent) {
        // Wheel scrolling is a separate QEvent::Wheel — handle it
        // here so the user can zoom without leaving the grab.
        if (event->type() == QEvent::Wheel) {
            auto *wheelEvent = dynamic_cast<QWheelEvent *>(event);
            if (wheelEvent) {
                const qreal stepNotches = wheelEvent->angleDelta().y() / 120.0;
                const qreal scale = std::pow(1.0 + kWheelRadiusFactor,
                                             wheelEvent->inverted() ? -stepNotches : stepNotches);
                m_cameraTarget.radius = std::max(qreal(10.0), m_cameraTarget.radius * scale);
                if (effects) {
                    effects->addRepaintFull();
                }
            }
        }
        return;
    }
    const QPoint pos = mouseEvent->globalPosition().toPoint();

    if (event->type() == QEvent::MouseMove) {
        m_mouseCurrentGlobal = pos;
        if (!m_mouseDragging) {
            const QPoint delta = pos - m_mousePressGlobal;
            if (m_mousePressGlobal.x() != 0 && delta.manhattanLength() >= kDragThresholdPx) {
                m_mouseDragging = true;
            }
        }
        if (m_mouseDragging) {
            const QPoint delta = pos - m_mousePressGlobal;
            qreal yawDelta = qreal(delta.x()) / kYawPixelsPerDegree;
            qreal pitchDelta = qreal(delta.y()) / kPitchPixelsPerDegree;
            if (m_mouseInvertedX) {
                yawDelta = -yawDelta;
            }
            if (m_mouseInvertedY) {
                pitchDelta = -pitchDelta;
            }
            m_cameraTarget.yawDeg = m_cameraPressStart.yawDeg - yawDelta;
            m_cameraTarget.pitchDeg = std::clamp(
                m_cameraPressStart.pitchDeg - pitchDelta,
                kMinPitchDeg, kMaxPitchDeg);
            if (effects) {
                effects->addRepaintFull();
            }
        }
        return;
    }

    if (event->type() == QEvent::MouseButtonPress) {
        if (mouseEvent->button() != Qt::LeftButton) {
            return;
        }
        m_mousePressGlobal = pos;
        m_mouseCurrentGlobal = pos;
        m_cameraPressStart = m_cameraTarget;
        m_mouseDragging = false;
        return;
    }

    if (event->type() == QEvent::MouseButtonRelease) {
        if (mouseEvent->button() != Qt::LeftButton) {
            return;
        }
        const bool wasDrag = m_mouseDragging;
        m_mouseDragging = false;
        m_mousePressGlobal = QPoint();
        if (!wasDrag && effects) {
            // Click without drag → switch to the picked face, or
            // to the currently centred desktop if the picker misses.
            int faceIdx = hitTestFace(pos, effects->virtualScreenSize());
            VirtualDesktop *target = nullptr;
            const auto desktops = effects->desktops();
            if (faceIdx >= 0 && faceIdx < int(desktops.size())) {
                target = desktops[faceIdx];
            } else {
                target = centredDesktop();
            }
            if (target && target != effects->currentDesktop()) {
                effects->setCurrentDesktop(target);
            }
            deactivate();
        }
    }
}

#if HAVE_VULKAN
bool CubeEffectV2::ensureVulkanPipeline(VulkanContext *ctx, VkFormat colorFormat)
{
    Q_UNUSED(ctx);
    Q_UNUSED(colorFormat);
    // Phase 3. Mirror OverviewEffectV2::ensureVulkanPipeline:
    //   - VkShaderModule from SPIR-V (per-face quad with model MVP
    //     in push constants, fragment samples binding=0).
    //   - VkDescriptorSetLayout: combined-image-sampler.
    //   - VkPipelineLayout: layout + push constants for model
    //     matrix + atlas UV rect + per-face index.
    //   - VkRenderPass compat with kwin's post-FX pass (LOAD_OP_LOAD
    //     so the rendered scene shows through transparent /
    //     skybox-masked areas).
    //   - VkGraphicsPipeline: triangle list (4 verts → 2 tris,
    //     indexed or strip; overview uses strip), depth test off
    //     (painter's algorithm), blend over premultiplied alpha.
    return false;
}

void CubeEffectV2::destroyVulkanPipeline()
{
    // Phase 3.
}

void CubeEffectV2::renderDesktopsToAtlas()
{
    // Phase 2. Per-desktop full-framebuffer-size atlas slot, rendered
    // via ItemRendererVulkan into the slot's framebuffer view. Walk
    // every desktop, render each desktop's window stack into its own
    // slot. Hold EffectWindowVisibleRefs across the active phase so
    // off-current-desktop windows actually have content; the suspend
    // hook + dedicated-memory work already in place ensure the VRAM
    // is returned on deactivate.
}

void CubeEffectV2::releaseAllResources()
{
    if (m_vulkanCtx) {
        // Wait on any in-flight per-desktop atlas submit. Same
        // pattern OverviewEffectV2::releaseAllSlots uses — without
        // this the freed slot rects could get handed back to a
        // fresh consumer mid-flight.
    }
    if (m_atlas) {
        for (auto &[desktop, ds] : m_desktopSlots) {
            m_atlas->release(ds.slot);
        }
    }
    m_desktopSlots.clear();
    m_visibilityRefs.clear();
    m_skyboxTexture.reset();
    // Atlas singleton drop — same hand-off as Overview V2. If a
    // future consumer ends up sharing this, switch to ref counting.
    if (m_atlas) {
        // VulkanThumbnailAtlas::dropForContext(m_vulkanCtx); // Phase 2.
        m_atlas = nullptr;
    }
    m_postPassId = -1;
    m_lastPresentTime = std::chrono::milliseconds{0};
}
#endif // HAVE_VULKAN

} // namespace KWin

#include "moc_cubeeffectv2.cpp"
