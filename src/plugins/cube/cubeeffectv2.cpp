/*
    SPDX-FileCopyrightText: 2026 Ark Royal <awright42mk1@protonmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "cubeeffectv2.h"

#include "effect/effecthandler.h"
#include "effect/effectwindow.h"

#include "virtualdesktops.h"
#include "window.h"

#if HAVE_VULKAN
#include "compositor.h"
#include "core/rendertarget.h"
#include "core/renderviewport.h"
#include "platformsupport/scenes/vulkan/vulkanbackend.h"
#include "platformsupport/scenes/vulkan/vulkancontext.h"
#include "platformsupport/scenes/vulkan/vulkanframebuffer.h"
#include "platformsupport/scenes/vulkan/vulkanrenderpass.h"
#include "platformsupport/scenes/vulkan/vulkanrendertarget.h"
#include "platformsupport/scenes/vulkan/vulkantexture.h"
#include "platformsupport/scenes/vulkan/vulkanthumbnailatlas.h"
#include "scene/itemrenderer_vulkan.h"
#include "scene/workspacescene.h"

#include "shaders/background_skybox_spv.inc"
#include "shaders/cube_face_spv.inc"

using namespace KWin::CubeEffectV2Shaders;
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
#include <QSet>
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
    // Same env-var gate as Overview V2. The existing scripted
    // CubeEffect needs a matching `supported()` predicate that
    // refuses to load when this var is set; that wiring is in the
    // cube package main.qml (see the Phase 1b TODO).
    if (qEnvironmentVariableIntValue("KWIN_CUBE_V2") == 0) {
        return false;
    }
    return effects && (effects->isOpenGLCompositing() || effects->isVulkanCompositing());
}

CubeEffectV2::CubeEffectV2()
{
    // One-shot startup log so silence != "is the env var set?".
    // Same pattern Overview V2 uses ([[feedback_env_var_logging]]).
    qCWarning(KWIN_CUBE_V2_LOG)
        << "CubeEffectV2: enabled (KWIN_CUBE_V2=1). C++ rewrite of "
           "the scripted cube effect — Phase 1 (lifecycle only).";

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

    // Global toggle. Same `Cube` object name as the scripted effect's
    // ShortcutHandler so the user's saved binding (default Meta+C)
    // carries over. With supported() gating, the scripted CubeEffect
    // is the one that won't load, so there's no double-registration.
    m_toggleAction = new QAction(this);
    m_toggleAction->setObjectName(QStringLiteral("Cube"));
    m_toggleAction->setText(i18nc("@action Cube is the name of a Kwin effect", "Toggle Cube"));
    m_toggleAction->setAutoRepeat(false);
    const QKeySequence defaultShortcut = Qt::META | Qt::Key_C;
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
    // the same render path as V1 had.
    //
    // Implementation status: the cube post-pass currently piggybacks
    // on the renderer's swapchain post-FX render pass, which is fixed
    // at 1 sample. Honest MSAA requires an offscreen MSAA color
    // attachment + a resolve attachment, drawn before the renderer's
    // main pass and composited back. Until that path lands we accept
    // the config value but log when the user picked > 1x so they
    // know the request was seen.
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
    if (m_msaaSamples > 1) {
        qCWarning(KWIN_CUBE_V2_LOG)
            << "CubeEffectV2: MSAA=" << m_msaaSamples
            << "configured, but the offscreen MSAA render path is "
               "not yet implemented; rendering at 1x for now.";
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

#if HAVE_VULKAN
    // Phase 2: reserve per-desktop atlas slots + hold off-current-
    // desktop visibility refs, then start composing each desktop
    // into its slot every frame via preFrameRender. Phase 3: also
    // register the on-screen face draw as a fullscreen post-pass.
    reserveDesktopSlots();
    if (auto *scene = Compositor::self()->scene()) {
        m_preFrameConnection = connect(scene, &WorkspaceScene::preFrameRender,
                                       this, &CubeEffectV2::renderDesktopsToAtlas);
        if (auto *vkRenderer = dynamic_cast<ItemRendererVulkan *>(scene->renderer())) {
            m_postPassId = vkRenderer->registerFullscreenPostPass(
                [this](VkCommandBuffer cmd, VulkanTexture *sceneCapture,
                       const RenderTarget &target,
                       const RenderViewport &viewport) {
                onPostPass(cmd, sceneCapture, target, viewport);
            });
        }
    }
#endif

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
    if (!effects || fbSize.isEmpty()) {
        return -1;
    }
    const auto desktops = effects->desktops();
    const int n = int(desktops.size());
    if (n == 0) {
        return -1;
    }
    // Cursor → NDC. The post-pass viewport is Y-flipped (we set vp.y
    // = fbH, vp.height = -fbH in onPostPass), so the NDC mapping for
    // hit tests has to match: NDC.y = -1 at screen top, +1 at screen
    // bottom in flipped Y space; with our flip the picker computes
    // NDC as if Y goes up on screen.
    const float ndcX = (float(cursorViewport.x()) / float(fbSize.width())) * 2.0f - 1.0f;
    const float ndcY = 1.0f - (float(cursorViewport.y()) / float(fbSize.height())) * 2.0f;

    // Inverse projection-view → ray in world space. Use two NDC z
    // depths (-1 = near, +1 = far) and inverse-transform to world.
    const QMatrix4x4 invVp = (m_viewProj.projection * m_viewProj.view).inverted();
    QVector4D nearH = invVp * QVector4D(ndcX, ndcY, -1.0f, 1.0f);
    QVector4D farH = invVp * QVector4D(ndcX, ndcY, +1.0f, 1.0f);
    if (nearH.w() == 0.0f || farH.w() == 0.0f) {
        return -1;
    }
    const QVector3D nearW(nearH.x() / nearH.w(), nearH.y() / nearH.w(), nearH.z() / nearH.w());
    const QVector3D farW(farH.x() / farH.w(), farH.y() / farH.w(), farH.z() / farH.w());
    const QVector3D rayOrigin = nearW;
    const QVector3D rayDir = (farW - nearW).normalized();

    // Intersect against each face's plane (a unit-quad transformed by
    // faceModelMatrix). Then check whether the hit point lies inside
    // the quad's local extent [-0.5, +0.5]² before scaling.
    int bestIndex = -1;
    float bestT = std::numeric_limits<float>::max();
    for (int i = 0; i < n; ++i) {
        const QMatrix4x4 model = faceModelMatrix(i, n);
        bool invertible = false;
        const QMatrix4x4 invModel = model.inverted(&invertible);
        if (!invertible) {
            continue;
        }
        // Ray in face-local space (where the quad is the unit square
        // at z=0). Intersect with z=0 plane.
        const QVector4D localOriginH = invModel * QVector4D(rayOrigin, 1.0f);
        const QVector4D localDirH = invModel * QVector4D(rayDir, 0.0f);
        const QVector3D localOrigin(localOriginH.x(), localOriginH.y(), localOriginH.z());
        const QVector3D localDir(localDirH.x(), localDirH.y(), localDirH.z());
        if (std::abs(localDir.z()) < 1e-6f) {
            continue;
        }
        const float t = -localOrigin.z() / localDir.z();
        if (t <= 0.0f) {
            continue; // behind the camera
        }
        const float hx = localOrigin.x() + t * localDir.x();
        const float hy = localOrigin.y() + t * localDir.y();
        if (hx < -0.5f || hx > 0.5f || hy < -0.5f || hy > 0.5f) {
            continue; // outside the quad
        }
        if (t < bestT) {
            bestT = t;
            bestIndex = i;
        }
    }
    return bestIndex;
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
            // hitTestFace expects cursor in compositor-fb pixel space;
            // global mouse position needs the virtual-screen offset
            // subtracted (multi-monitor setups have non-zero origin).
            const QRect screen = effects->virtualScreenGeometry();
            const QPoint cursorFb = pos - screen.topLeft();
            int faceIdx = hitTestFace(cursorFb, effects->virtualScreenSize());
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

// Push-constant layout: must match cube_face.{vert,frag}'s layout
// block exactly (mat4 + vec4 + float = 84 bytes, packed to 96 with
// std140 vec4 alignment for the trailing scalar). 128 bytes is the
// guaranteed Vulkan minimum so this fits comfortably.
struct CubePushConstants
{
    float mvp[16];
    float atlasSlotUv[4];
    float opacity;
    float _pad[3];
};

bool CubeEffectV2::ensureVulkanPipeline(VulkanContext *ctx, VkFormat colorFormat)
{
    if (m_vkPipeline != VK_NULL_HANDLE && m_pipelineColorFormat == colorFormat) {
        return true;
    }
    if (m_vkPipeline != VK_NULL_HANDLE) {
        // Color format flipped (e.g. switched outputs at different
        // depths). Tear down and rebuild against the new format.
        destroyVulkanPipeline();
    }
    if (!ctx) {
        return false;
    }

    VkDevice device = ctx->backend()->device();

    auto makeModule = [&](const uint32_t *code, size_t byteSize) -> VkShaderModule {
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = byteSize;
        info.pCode = code;
        VkShaderModule mod = VK_NULL_HANDLE;
        vkCreateShaderModule(device, &info, nullptr, &mod);
        return mod;
    };
    m_vertModule = makeModule(kVertSpv, sizeof(kVertSpv));
    m_fragModule = makeModule(kFragSpv, sizeof(kFragSpv));
    if (!m_vertModule || !m_fragModule) {
        qCWarning(KWIN_CUBE_V2_LOG) << "CubeEffectV2: shader-module creation failed";
        destroyVulkanPipeline();
        return false;
    }

    // set=0,binding=0 — combined image sampler (atlas slot's SRGB
    // view + atlas's linear-mipmap sampler). Push-descriptor flag so
    // each face draw can push its own binding without a per-frame
    // descriptor pool churn.
    VkDescriptorSetLayoutBinding dsBinding{};
    dsBinding.binding = 0;
    dsBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    dsBinding.descriptorCount = 1;
    dsBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dsLayoutInfo{};
    dsLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsLayoutInfo.bindingCount = 1;
    dsLayoutInfo.pBindings = &dsBinding;
    if (ctx->backend() && ctx->backend()->supportsPushDescriptor()) {
        dsLayoutInfo.flags |= VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    }
    if (vkCreateDescriptorSetLayout(device, &dsLayoutInfo, nullptr, &m_vkDescriptorSetLayout) != VK_SUCCESS) {
        qCWarning(KWIN_CUBE_V2_LOG) << "CubeEffectV2: vkCreateDescriptorSetLayout failed";
        destroyVulkanPipeline();
        return false;
    }

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset = 0;
    pcRange.size = sizeof(CubePushConstants);

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &m_vkDescriptorSetLayout;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges = &pcRange;
    if (vkCreatePipelineLayout(device, &plInfo, nullptr, &m_vkPipelineLayout) != VK_SUCCESS) {
        qCWarning(KWIN_CUBE_V2_LOG) << "CubeEffectV2: vkCreatePipelineLayout failed";
        destroyVulkanPipeline();
        return false;
    }

    // Compat render pass (matches the renderer's post-FX pass on
    // attachment format + sample count; pipeline doesn't need the
    // exact VkRenderPass object).
    m_postPassCompatRenderPass = VulkanRenderPass::createForSwapchainPostFx(ctx, colorFormat);
    if (!m_postPassCompatRenderPass) {
        qCWarning(KWIN_CUBE_V2_LOG) << "CubeEffectV2: compat render pass build failed";
        destroyVulkanPipeline();
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = m_vertModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = m_fragModule;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

    VkPipelineViewportStateCreateInfo vpState{};
    vpState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vpState.viewportCount = 1;
    vpState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    // No back-face cull: V1's Material.NoCulling matches; lets us see
    // the back of the cube while rotating instead of black holes.
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    // Phase 4 will toggle this to VK_SAMPLE_COUNT_{2,4,8}_BIT based on
    // m_msaaSamples; for Phase 3 we stay at 1x. Render-pass sample
    // count + pipeline sample count must match.
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
        | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    const VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo gpInfo{};
    gpInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpInfo.stageCount = 2;
    gpInfo.pStages = stages;
    gpInfo.pVertexInputState = &vi;
    gpInfo.pInputAssemblyState = &ia;
    gpInfo.pViewportState = &vpState;
    gpInfo.pRasterizationState = &rs;
    gpInfo.pMultisampleState = &ms;
    gpInfo.pColorBlendState = &cb;
    gpInfo.pDynamicState = &dyn;
    gpInfo.layout = m_vkPipelineLayout;
    gpInfo.renderPass = m_postPassCompatRenderPass->renderPass();
    gpInfo.subpass = 0;

    const bool ok = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpInfo,
                                              nullptr, &m_vkPipeline)
        == VK_SUCCESS;
    if (!ok) {
        qCWarning(KWIN_CUBE_V2_LOG) << "CubeEffectV2: vkCreateGraphicsPipelines failed";
        destroyVulkanPipeline();
        return false;
    }
    m_pipelineColorFormat = colorFormat;
    return true;
}

void CubeEffectV2::destroyVulkanPipeline()
{
    destroySkyboxPipeline();
    if (!m_vulkanCtx) {
        // Modules / layouts can't be destroyed without a device, but
        // nothing was created either if m_vulkanCtx is null.
        m_vertModule = VK_NULL_HANDLE;
        m_fragModule = VK_NULL_HANDLE;
        m_vkDescriptorSetLayout = VK_NULL_HANDLE;
        m_vkPipelineLayout = VK_NULL_HANDLE;
        m_vkPipeline = VK_NULL_HANDLE;
        m_postPassCompatRenderPass.reset();
        m_pipelineColorFormat = VK_FORMAT_UNDEFINED;
        return;
    }
    VkDevice device = m_vulkanCtx->backend()->device();
    if (m_vkPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_vkPipeline, nullptr);
        m_vkPipeline = VK_NULL_HANDLE;
    }
    if (m_vkPipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, m_vkPipelineLayout, nullptr);
        m_vkPipelineLayout = VK_NULL_HANDLE;
    }
    if (m_vkDescriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, m_vkDescriptorSetLayout, nullptr);
        m_vkDescriptorSetLayout = VK_NULL_HANDLE;
    }
    if (m_vertModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, m_vertModule, nullptr);
        m_vertModule = VK_NULL_HANDLE;
    }
    if (m_fragModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, m_fragModule, nullptr);
        m_fragModule = VK_NULL_HANDLE;
    }
    m_postPassCompatRenderPass.reset();
    m_pipelineColorFormat = VK_FORMAT_UNDEFINED;
}

// SkyBox push constants — kept layout-compatible with
// background_skybox.{vert,frag}'s push-constant block. cameraPosW
// is the camera eye in world coordinates; the fragment subtracts
// it from the interpolated far-plane point to get the true view
// direction (otherwise the panorama only lines up when the camera
// sits at the world origin, which our orbital camera never does).
struct CubeSkyboxPushConstants
{
    float invViewProj[16];
    float cameraPosW[4];
    float opacity;
    float _pad[3];
};

bool CubeEffectV2::ensureSkyboxTexture()
{
    if (m_skyboxTexture && m_skyboxTexture->isValid()) {
        return true;
    }
    if (!m_vulkanCtx) {
        return false;
    }
    if (!m_skyboxPath.isValid() || m_skyboxPath.isEmpty()) {
        return false;
    }
    QImage img;
    const QString local = m_skyboxPath.isLocalFile() ? m_skyboxPath.toLocalFile()
                                                     : m_skyboxPath.toString();
    if (!img.load(local)) {
        qCWarning(KWIN_CUBE_V2_LOG) << "CubeEffectV2: skybox load failed:" << local;
        return false;
    }
    // Convert to premultiplied RGBA — VulkanTexture::upload expects
    // that for the cube renderer's blend setup
    // ([[project_vulkan_premultiplied_alpha]]).
    img = img.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    m_skyboxTexture = VulkanTexture::upload(m_vulkanCtx, img);
    if (!m_skyboxTexture) {
        qCWarning(KWIN_CUBE_V2_LOG) << "CubeEffectV2: skybox upload failed:" << local;
        return false;
    }
    return true;
}

bool CubeEffectV2::ensureSkyboxPipeline(VulkanContext *ctx, VkFormat colorFormat)
{
    if (m_skyboxPipeline != VK_NULL_HANDLE && m_pipelineColorFormat == colorFormat) {
        return true;
    }
    if (m_skyboxPipeline != VK_NULL_HANDLE) {
        destroySkyboxPipeline();
    }
    if (!ctx || m_vkDescriptorSetLayout == VK_NULL_HANDLE) {
        // Cube-face pipeline must have built first — we share its
        // descriptor set layout.
        return false;
    }
    VkDevice device = ctx->backend()->device();

    auto makeModule = [&](const uint32_t *code, size_t byteSize) -> VkShaderModule {
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = byteSize;
        info.pCode = code;
        VkShaderModule mod = VK_NULL_HANDLE;
        vkCreateShaderModule(device, &info, nullptr, &mod);
        return mod;
    };
    m_skyboxVertModule = makeModule(kSkyboxVertSpv, sizeof(kSkyboxVertSpv));
    m_skyboxFragModule = makeModule(kSkyboxFragSpv, sizeof(kSkyboxFragSpv));
    if (!m_skyboxVertModule || !m_skyboxFragModule) {
        destroySkyboxPipeline();
        return false;
    }

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset = 0;
    pcRange.size = sizeof(CubeSkyboxPushConstants);

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &m_vkDescriptorSetLayout;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges = &pcRange;
    if (vkCreatePipelineLayout(device, &plInfo, nullptr, &m_skyboxPipelineLayout) != VK_SUCCESS) {
        destroySkyboxPipeline();
        return false;
    }

    if (!m_postPassCompatRenderPass) {
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = m_skyboxVertModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = m_skyboxFragModule;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vpState{};
    vpState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vpState.viewportCount = 1;
    vpState.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
        | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    const VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo gpInfo{};
    gpInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpInfo.stageCount = 2;
    gpInfo.pStages = stages;
    gpInfo.pVertexInputState = &vi;
    gpInfo.pInputAssemblyState = &ia;
    gpInfo.pViewportState = &vpState;
    gpInfo.pRasterizationState = &rs;
    gpInfo.pMultisampleState = &ms;
    gpInfo.pColorBlendState = &cb;
    gpInfo.pDynamicState = &dyn;
    gpInfo.layout = m_skyboxPipelineLayout;
    gpInfo.renderPass = m_postPassCompatRenderPass->renderPass();
    gpInfo.subpass = 0;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpInfo, nullptr, &m_skyboxPipeline) != VK_SUCCESS) {
        destroySkyboxPipeline();
        return false;
    }
    return true;
}

void CubeEffectV2::destroySkyboxPipeline()
{
    if (!m_vulkanCtx) {
        m_skyboxVertModule = VK_NULL_HANDLE;
        m_skyboxFragModule = VK_NULL_HANDLE;
        m_skyboxPipelineLayout = VK_NULL_HANDLE;
        m_skyboxPipeline = VK_NULL_HANDLE;
        return;
    }
    VkDevice device = m_vulkanCtx->backend()->device();
    if (m_skyboxPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_skyboxPipeline, nullptr);
        m_skyboxPipeline = VK_NULL_HANDLE;
    }
    if (m_skyboxPipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, m_skyboxPipelineLayout, nullptr);
        m_skyboxPipelineLayout = VK_NULL_HANDLE;
    }
    if (m_skyboxVertModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, m_skyboxVertModule, nullptr);
        m_skyboxVertModule = VK_NULL_HANDLE;
    }
    if (m_skyboxFragModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, m_skyboxFragModule, nullptr);
        m_skyboxFragModule = VK_NULL_HANDLE;
    }
}

QMatrix4x4 CubeEffectV2::faceModelMatrix(int i, int n) const
{
    if (n <= 0 || !effects) {
        return QMatrix4x4();
    }
    const qreal angleTickDeg = 360.0 / qreal(n);
    const QSize fb = effects->virtualScreenSize();
    const qreal faceW = qreal(fb.width());
    const qreal faceH = qreal(fb.height());
    const qreal dist = faceDistance(n);

    // M = Ry(angleTick * i) * T(0, 0, faceDistance) * S(faceW, faceH, 1)
    // (right-to-left composition; rightmost transform applies first
    // to the unit quad).
    QMatrix4x4 m;
    m.setToIdentity();
    m.rotate(float(angleTickDeg * i), 0.0f, 1.0f, 0.0f);
    m.translate(0.0f, 0.0f, float(dist));
    m.scale(float(faceW), float(faceH), 1.0f);
    return m;
}

void CubeEffectV2::onPostPass(VkCommandBuffer cmd, VulkanTexture *sceneCapture,
                              const RenderTarget &renderTarget,
                              const RenderViewport &viewport)
{
    Q_UNUSED(sceneCapture);
    Q_UNUSED(viewport);
    if (!effects || !m_atlas || !m_vulkanCtx || m_desktopSlots.empty()) {
        return;
    }
    auto *vkTarget = renderTarget.vulkanTarget();
    if (!vkTarget) {
        return;
    }
    const QSize fbSize = renderTarget.size();
    if (fbSize.isEmpty()) {
        return;
    }
    // Pull swapchain colour format from the backend (overview V2 uses
    // the same accessor); pipeline format must match the active
    // render pass.
    const VkFormat colorFormat = m_vulkanCtx->backend()
        ? m_vulkanCtx->backend()->colorFormat()
        : VK_FORMAT_B8G8R8A8_UNORM;
    if (!ensureVulkanPipeline(m_vulkanCtx, colorFormat)) {
        return;
    }

    auto *backend = m_vulkanCtx->backend();
    auto pushDescriptor = backend ? backend->cmdPushDescriptorSetKHR() : nullptr;
    if (!pushDescriptor) {
        return;
    }

    updateViewProjection(fbSize);

    // Background pass. The renderer's post-FX pass uses LOAD_OP_DONT_CARE
    // so without this every pixel outside a cube face would be undefined
    // garbage. Color mode = clear to the configured BackgroundColor;
    // SkyBox mode draws an equirect sample on top of that clear.
    const float factor = float(m_activationFactor);
    {
        VkClearAttachment clear{};
        clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        clear.colorAttachment = 0;
        // Premultiplied alpha — the post-FX pass blends with the
        // scene capture beneath via the renderer's compositing path,
        // so a partial-opacity activation ramps the background in
        // alongside the cube faces.
        clear.clearValue.color.float32[0] =
            float(m_backgroundColor.redF()) * factor;
        clear.clearValue.color.float32[1] =
            float(m_backgroundColor.greenF()) * factor;
        clear.clearValue.color.float32[2] =
            float(m_backgroundColor.blueF()) * factor;
        clear.clearValue.color.float32[3] = factor;
        VkClearRect clearRect{};
        clearRect.rect = VkRect2D{
            {0, 0},
            {uint32_t(fbSize.width()), uint32_t(fbSize.height())},
        };
        clearRect.baseArrayLayer = 0;
        clearRect.layerCount = 1;
        vkCmdClearAttachments(cmd, 1, &clear, 1, &clearRect);
    }

    // SkyBox pass: equirect sample using the view ray. Falls through
    // to plain Color when the texture isn't loaded (invalid path,
    // upload failure, etc.) — UX is then "Color background" without
    // a crash.
    if (m_backgroundMode == Background::SkyBox
        && ensureSkyboxPipeline(m_vulkanCtx, colorFormat)
        && ensureSkyboxTexture()
        && m_skyboxTexture && m_skyboxTexture->isValid()) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skyboxPipeline);
        // The skybox covers the full screen, same viewport+scissor
        // as the cube faces. Y-flip applied identically.
        VkViewport sbVp{0.0f, float(fbSize.height()),
                        float(fbSize.width()), -float(fbSize.height()),
                        0.0f, 1.0f};
        vkCmdSetViewport(cmd, 0, 1, &sbVp);
        VkRect2D sbScissor{{0, 0}, {uint32_t(fbSize.width()), uint32_t(fbSize.height())}};
        vkCmdSetScissor(cmd, 0, 1, &sbScissor);

        VkDescriptorImageInfo imgInfo{};
        imgInfo.sampler = m_atlas->sampler();
        imgInfo.imageView = m_skyboxTexture->imageView();
        imgInfo.imageLayout = m_skyboxTexture->currentLayout();
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imgInfo;
        pushDescriptor(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skyboxPipelineLayout, 0, 1, &write);

        CubeSkyboxPushConstants spc{};
        const QMatrix4x4 invVp = (m_viewProj.projection * m_viewProj.view).inverted();
        const float *invData = invVp.constData();
        for (int k = 0; k < 16; ++k) {
            spc.invViewProj[k] = invData[k];
        }
        // Reconstruct the eye position from the current camera state
        // (same spherical-coords formula as updateViewProjection).
        const qreal theta = qDegreesToRadians(m_cameraCurrent.pitchDeg + 90.0);
        const qreal phi = qDegreesToRadians(m_cameraCurrent.yawDeg);
        const qreal r = m_cameraCurrent.radius;
        spc.cameraPosW[0] = float(r * std::sin(phi) * std::sin(theta));
        spc.cameraPosW[1] = float(r * std::cos(theta));
        spc.cameraPosW[2] = float(r * std::cos(phi) * std::sin(theta));
        spc.cameraPosW[3] = 0.0f;
        spc.opacity = factor;
        vkCmdPushConstants(cmd, m_skyboxPipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(spc), &spc);
        // 3-vert fullscreen triangle.
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_vkPipeline);
    VkViewport vp{0.0f, float(fbSize.height()),
                  float(fbSize.width()), -float(fbSize.height()),
                  0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D scissor{{0, 0}, {uint32_t(fbSize.width()), uint32_t(fbSize.height())}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    const auto desktops = effects->desktops();
    const int n = int(desktops.size());
    if (n == 0) {
        return;
    }
    const qreal atlasSize = qreal(VulkanThumbnailAtlas::kAtlasSize);

    // Painter's algorithm: sort faces by camera-space Z, draw farthest
    // first. Camera-space Z = (view * model_origin).z. More negative
    // means farther away in Vulkan's right-handed view space.
    struct FaceDraw
    {
        VirtualDesktop *desktop;
        int index;
        float cameraZ; // sort key
        QMatrix4x4 mvp;
    };
    std::vector<FaceDraw> draws;
    draws.reserve(n);
    const QMatrix4x4 viewProj = m_viewProj.projection * m_viewProj.view;
    for (int i = 0; i < n; ++i) {
        VirtualDesktop *vd = desktops[i];
        auto it = m_desktopSlots.find(vd);
        if (it == m_desktopSlots.end() || !it->second.hasContent) {
            continue;
        }
        const QMatrix4x4 model = faceModelMatrix(i, n);
        // Camera-space Z of the face's centre for the painter sort.
        const QVector4D centreCam = m_viewProj.view * (model * QVector4D(0, 0, 0, 1));
        draws.push_back({vd, i, centreCam.z(), viewProj * model});
    }
    std::sort(draws.begin(), draws.end(), [](const FaceDraw &a, const FaceDraw &b) {
        return a.cameraZ < b.cameraZ;
    });

    for (const FaceDraw &fd : draws) {
        const auto &ds = m_desktopSlots[fd.desktop];
        if (ds.slot.srgbView == VK_NULL_HANDLE) {
            continue;
        }
        VkDescriptorImageInfo imgInfo{};
        imgInfo.sampler = m_atlas->sampler();
        imgInfo.imageView = ds.slot.srgbView;
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imgInfo;
        pushDescriptor(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_vkPipelineLayout, 0, 1, &write);

        CubePushConstants pc{};
        const float *mvpData = fd.mvp.constData();
        for (int k = 0; k < 16; ++k) {
            pc.mvp[k] = mvpData[k];
        }
        if (ds.slot.isFallback) {
            // Fallback slots own their entire dedicated image — the
            // srgbView is a view of the whole image, so UV space is
            // [0, 1]² regardless of slot.rect (which carries the
            // dedicated image's pixel size, not an atlas-space offset).
            pc.atlasSlotUv[0] = 0.0f;
            pc.atlasSlotUv[1] = 0.0f;
            pc.atlasSlotUv[2] = 1.0f;
            pc.atlasSlotUv[3] = 1.0f;
        } else {
            pc.atlasSlotUv[0] = float(ds.slot.rect.x()) / float(atlasSize);
            pc.atlasSlotUv[1] = float(ds.slot.rect.y()) / float(atlasSize);
            pc.atlasSlotUv[2] = float(ds.slot.rect.width()) / float(atlasSize);
            pc.atlasSlotUv[3] = float(ds.slot.rect.height()) / float(atlasSize);
        }
        pc.opacity = factor;
        vkCmdPushConstants(cmd, m_vkPipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(pc), &pc);
        vkCmdDraw(cmd, 4, 1, 0, 0);
    }
}

void CubeEffectV2::reserveDesktopSlots()
{
    if (!effects || !effects->isVulkanCompositing()) {
        return;
    }
    auto *scene = Compositor::self()->scene();
    if (!scene) {
        return;
    }
    auto *vkRenderer = dynamic_cast<ItemRendererVulkan *>(scene->renderer());
    if (!vkRenderer) {
        return;
    }
    m_vulkanCtx = vkRenderer->context();
    if (!m_vulkanCtx) {
        return;
    }
    m_atlas = VulkanThumbnailAtlas::get(m_vulkanCtx);
    if (!m_atlas) {
        return;
    }

    const QSize fbSize = effects->virtualScreenSize();
    if (fbSize.isEmpty()) {
        return;
    }

    // One slot per desktop, sized to the full framebuffer. Atlas
    // returns an in-atlas rect if the size fits (multiple slots
    // can share the 4096² atlas image) or a dedicated fallback
    // image if not. For 1080p that means up to ~6 desktops share
    // the atlas; for 4K each desktop falls back to its own image.
    const auto desktops = effects->desktops();
    for (VirtualDesktop *vd : desktops) {
        if (!vd) {
            continue;
        }
        auto slot = m_atlas->reserve(fbSize);
        if (!slot.isValid()) {
            qCWarning(KWIN_CUBE_V2_LOG)
                << "CubeEffectV2: atlas reserve failed for desktop"
                << vd->x11DesktopNumber() << "size" << fbSize;
            continue;
        }
        m_desktopSlots.emplace(vd, DesktopSlot{std::move(slot), false});
    }

    // Hold an EffectWindowVisibleRef for every window on a non-
    // current desktop. Without these, WindowItem::computeVisibility
    // returns false for off-desktop windows and renderItem produces
    // empty content. The refs drop in releaseAllResources, at which
    // point the X11 suspend hook + dedicated-memory work return the
    // associated per-window VRAM to the OS.
    auto *currentDesktop = effects->currentDesktop();
    for (EffectWindow *ew : effects->stackingOrder()) {
        if (!ew) {
            continue;
        }
        if (currentDesktop && ew->isOnDesktop(currentDesktop)) {
            continue;
        }
        Window *handle = ew->window();
        if (!handle || !handle->isClient() || !handle->isNormalWindow()) {
            continue;
        }
        // Same conservative filter Overview V2 uses; documented at
        // length there. Catches OSD popups, notifications, tooltips
        // etc. that mustn't appear in the cube faces.
        if (handle->skipSwitcher() || handle->isOnScreenDisplay()
            || handle->isNotification() || handle->isCriticalNotification()
            || handle->isTooltip() || handle->isComboBox()
            || handle->isDNDIcon() || handle->isPopupWindow()
            || !handle->readyForPainting()) {
            continue;
        }
        m_visibilityRefs.emplace_back(ew, EffectWindow::PAINT_DISABLED_BY_DESKTOP);
    }
}

void CubeEffectV2::renderDesktopsToAtlas()
{
    if (!effects || !m_atlas || !m_vulkanCtx || m_desktopSlots.empty()) {
        return;
    }
    // Skip composition during the slide-out tail. Once the cube is
    // mostly invisible (alpha < 0.05) the on-screen pass blends
    // captured content to near-zero anyway — refreshing the atlas
    // would burn ~1ms of GPU per frame for nothing.
    if (m_activationFactor < 0.05 && m_animation.direction() == QVariantAnimation::Backward) {
        return;
    }
    // Defensive: drop any slot whose VirtualDesktop has been removed
    // since reservation. The VDM doesn't fire a callback we listen
    // to in this draft (Phase 5 will hook desktopRemoved); checking
    // membership against the live list catches the dangling pointer
    // case without crashing.
    {
        const auto liveDesktops = effects->desktops();
        const QSet<VirtualDesktop *> liveSet(liveDesktops.begin(), liveDesktops.end());
        for (auto it = m_desktopSlots.begin(); it != m_desktopSlots.end();) {
            if (!liveSet.contains(it->first)) {
                if (m_vulkanCtx && m_lastAtlasSubmit.isValid()) {
                    m_vulkanCtx->waitForSubmit(m_lastAtlasSubmit);
                    m_lastAtlasSubmit = VulkanSubmitHandle{};
                }
                m_atlas->release(it->second.slot);
                m_fallbackFramebuffers.erase(it->first);
                it = m_desktopSlots.erase(it);
            } else {
                ++it;
            }
        }
        if (m_desktopSlots.empty()) {
            return;
        }
    }
    auto *scene = Compositor::self()->scene();
    if (!scene) {
        return;
    }
    auto *vkRenderer = dynamic_cast<ItemRendererVulkan *>(scene->renderer());
    if (!vkRenderer) {
        return;
    }

    constexpr VkFormat kAtlasFormat = VK_FORMAT_R8G8B8A8_SRGB;
    if (!m_atlasRenderPass) {
        m_atlasRenderPass = VulkanRenderPass::createForAtlasWrite(m_vulkanCtx, kAtlasFormat);
        if (!m_atlasRenderPass) {
            qCWarning(KWIN_CUBE_V2_LOG)
                << "CubeEffectV2: createForAtlasWrite failed";
            return;
        }
    }

    // Locate any atlas-resident slot to learn the shared image+view.
    // If every slot ended up as a fallback (e.g. 4K + many desktops)
    // we skip the shared atlas pass and render straight into each
    // fallback framebuffer below.
    VkImage atlasImage = VK_NULL_HANDLE;
    VkImageView atlasMipZero = VK_NULL_HANDLE;
    for (const auto &[_vd, ds] : m_desktopSlots) {
        if (!ds.slot.isFallback) {
            atlasImage = ds.slot.image;
            atlasMipZero = ds.slot.mipZeroView;
            break;
        }
    }
    const bool haveAtlasSlots = atlasImage != VK_NULL_HANDLE;
    if (haveAtlasSlots && !m_atlasFramebuffer) {
        m_atlasFramebuffer = VulkanFramebuffer::create(m_vulkanCtx, m_atlasRenderPass.get(),
                                                       atlasMipZero,
                                                       QSize(VulkanThumbnailAtlas::kAtlasSize,
                                                             VulkanThumbnailAtlas::kAtlasSize));
        if (!m_atlasFramebuffer) {
            qCWarning(KWIN_CUBE_V2_LOG)
                << "CubeEffectV2: atlas framebuffer wrap failed";
            return;
        }
        m_atlasFramebuffer->setColorImage(atlasImage);
    }

    // Wait on the previous frame's atlas submit so the streaming-
    // buffer region we're about to reuse is GPU-finished.
    if (m_lastAtlasSubmit.isValid()) {
        m_vulkanCtx->waitForSubmit(m_lastAtlasSubmit);
        m_lastAtlasSubmit = VulkanSubmitHandle{};
    }

    VkCommandBuffer cmd = m_vulkanCtx->beginSingleTimeCommands();
    if (cmd == VK_NULL_HANDLE) {
        return;
    }
    vkRenderer->pushOffscreenSlot();

    // Composite one desktop's window stack into its slot. Shared by
    // the in-atlas and fallback paths; the caller sets up the active
    // render pass + framebuffer, then calls renderDesktopSlot for
    // each desktop with the right viewport/scissor.
    auto renderDesktopSlot = [&](VirtualDesktop *vd, const QRect &slotRect,
                                 VulkanFramebuffer *fb) {
        VkClearAttachment clearAtt{};
        clearAtt.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        clearAtt.colorAttachment = 0;
        // Clear the slot rect before drawing this desktop's windows
        // so a previous frame's content (or a different desktop's
        // residue when the atlas reuses a rect) doesn't show through
        // transparent areas. vkCmdClearAttachments respects the
        // current scissor — but we pass an explicit clear rect to
        // be unambiguous.
        VkClearRect clearRect{};
        clearRect.rect = VkRect2D{
            {int32_t(slotRect.x()), int32_t(slotRect.y())},
            {uint32_t(slotRect.width()), uint32_t(slotRect.height())},
        };
        clearRect.baseArrayLayer = 0;
        clearRect.layerCount = 1;
        vkCmdClearAttachments(cmd, 1, &clearAtt, 1, &clearRect);

        VkViewport vp{};
        vp.x = float(slotRect.x());
        vp.y = float(slotRect.y() + slotRect.height());
        vp.width = float(slotRect.width());
        vp.height = -float(slotRect.height()); // Y-flip for top-down mip-0
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &vp);

        VkRect2D scissor{
            {int32_t(slotRect.x()), int32_t(slotRect.y())},
            {uint32_t(slotRect.width()), uint32_t(slotRect.height())},
        };
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        auto vkRT = std::make_unique<VulkanRenderTarget>(fb);
        vkRT->setCommandBuffer(cmd);
        RenderTarget atlasTarget(vkRT.get());

        // Render every window on this desktop in stacking order so
        // higher-z windows occlude lower ones. Apply the same
        // conservative window-type filter as reservation — keeps
        // OSDs and tooltips out of the captured face.
        const QRectF screenGeom = effects->virtualScreenGeometry();
        RenderViewport viewport(screenGeom, 1.0, atlasTarget);
        for (EffectWindow *ew : effects->stackingOrder()) {
            if (!ew || !ew->isOnDesktop(vd)) {
                continue;
            }
            Window *handle = ew->window();
            if (!handle || !handle->isClient() || !handle->isNormalWindow()) {
                continue;
            }
            if (handle->skipSwitcher() || handle->isOnScreenDisplay()
                || handle->isNotification() || handle->isCriticalNotification()
                || handle->isTooltip() || handle->isComboBox()
                || handle->isDNDIcon() || handle->isPopupWindow()
                || !handle->readyForPainting()) {
                continue;
            }
            vkRenderer->renderItem(atlasTarget, viewport, handle->windowItem(),
                                   Scene::PAINT_WINDOW_TRANSFORMED, infiniteRegion(),
                                   WindowPaintData{});
        }
        m_desktopSlots[vd].hasContent = true;
    };

    VkClearValue clearVal{};
    if (haveAtlasSlots) {
        // Pre-pass memory barriers — atlas stays in GENERAL so this
        // is a memory barrier, not a layout transition.
        for (auto &[_vd, ds] : m_desktopSlots) {
            if (!ds.slot.isFallback) {
                m_atlas->prepareForRenderTo(cmd, ds.slot);
            }
        }
        const VkRect2D fullArea{
            {0, 0},
            {uint32_t(VulkanThumbnailAtlas::kAtlasSize), uint32_t(VulkanThumbnailAtlas::kAtlasSize)},
        };
        m_atlasRenderPass->begin(cmd, m_atlasFramebuffer->framebuffer(), fullArea, &clearVal, 1);
        for (auto &[vd, ds] : m_desktopSlots) {
            if (ds.slot.isFallback || !vd) {
                continue;
            }
            renderDesktopSlot(vd, ds.slot.rect, m_atlasFramebuffer.get());
        }
        m_atlasRenderPass->end(cmd);
    }

    // Fallback slots — one dedicated image per desktop. Each gets
    // its own framebuffer + render-pass instance.
    for (auto &[vd, ds] : m_desktopSlots) {
        if (!ds.slot.isFallback || !vd) {
            continue;
        }
        auto fbIt = m_fallbackFramebuffers.find(vd);
        VulkanFramebuffer *fb = nullptr;
        if (fbIt == m_fallbackFramebuffers.end()) {
            auto created = VulkanFramebuffer::create(m_vulkanCtx, m_atlasRenderPass.get(),
                                                     ds.slot.mipZeroView, ds.slot.rect.size());
            if (!created) {
                continue;
            }
            created->setColorImage(ds.slot.image);
            fb = created.get();
            m_fallbackFramebuffers.emplace(vd, std::move(created));
        } else {
            fb = fbIt->second.get();
        }
        m_atlas->prepareForRenderTo(cmd, ds.slot);
        const VkRect2D fbArea{
            {0, 0},
            {uint32_t(ds.slot.rect.width()), uint32_t(ds.slot.rect.height())},
        };
        m_atlasRenderPass->begin(cmd, fb->framebuffer(), fbArea, &clearVal, 1);
        renderDesktopSlot(vd, QRect(QPoint(0, 0), ds.slot.rect.size()), fb);
        m_atlasRenderPass->end(cmd);
    }

    // Generate mips + publish (transitions all mips to SHADER_READ_ONLY
    // for the upcoming face-draw pass to sample). One mip cascade per
    // slot.
    for (auto &[_vd, ds] : m_desktopSlots) {
        if (ds.hasContent) {
            m_atlas->generateMipsAndPublish(cmd, ds.slot);
        }
    }

    vkRenderer->popOffscreenSlot();
    m_lastAtlasSubmit = m_vulkanCtx->submitSingleTimeCommandsAsync(cmd);
}

void CubeEffectV2::releaseAllResources()
{
    QObject::disconnect(m_preFrameConnection);
    m_preFrameConnection = QMetaObject::Connection();

    if (m_postPassId != -1) {
        if (auto *scene = Compositor::self()->scene()) {
            if (auto *vkRenderer = dynamic_cast<ItemRendererVulkan *>(scene->renderer())) {
                vkRenderer->unregisterFullscreenPostPass(m_postPassId);
            }
        }
        m_postPassId = -1;
    }
    if (m_vulkanCtx && m_lastAtlasSubmit.isValid()) {
        // Wait on the most recent atlas write so the slot rects we're
        // about to return to the atlas's free list are GPU-finished.
        // Without this the rect could be handed back to a fresh
        // consumer (e.g. Overview V2) mid-flight.
        m_vulkanCtx->waitForSubmit(m_lastAtlasSubmit);
        m_lastAtlasSubmit = VulkanSubmitHandle{};
    }
    if (m_atlas) {
        for (auto &[_desktop, ds] : m_desktopSlots) {
            m_atlas->release(ds.slot);
        }
    }
    m_desktopSlots.clear();
    m_fallbackFramebuffers.clear();
    m_atlasFramebuffer.reset();
    m_atlasRenderPass.reset();
    m_visibilityRefs.clear();
    m_skyboxTexture.reset();
    // Atlas singleton hand-off — same as Overview V2. The atlas
    // image is ~85 MB so holding it across activations would defeat
    // the rewrite's primary goal. If a future cube consumer shares
    // the atlas with overview, replace this with proper ref counting.
    if (m_atlas) {
        VulkanThumbnailAtlas::dropForContext(m_vulkanCtx);
        m_atlas = nullptr;
    }
    m_postPassId = -1;
    m_lastPresentTime = std::chrono::milliseconds{0};
}
#endif // HAVE_VULKAN

} // namespace KWin

#include "moc_cubeeffectv2.cpp"
