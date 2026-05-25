/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2019 David Edmundson <davidedmundson@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "effect/offscreenquickview.h"
#include "effect/effecthandler.h"

#include "logging_p.h"
#include "opengl/glutils.h"
#include "opengl/openglcontext.h"

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickView>
#include <QStyleHints>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>

#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QQuickGraphicsDevice>
#include <QQuickOpenGLUtils>
#include <QQuickRenderTarget>
#include <QTimer>
#include <private/qeventpoint_p.h> // for QMutableEventPoint

#if HAVE_VULKAN
#include "platformsupport/scenes/vulkan/vulkanbackend.h"
#include "platformsupport/scenes/vulkan/vulkancontext.h"
#include "platformsupport/scenes/vulkan/vulkantexture.h"
#include <QVulkanInstance>
#include <vulkan/vulkan.h>
#endif

namespace KWin
{

// OffscreenQuickView stage breakdown (KWIN_FRAME_BREAKDOWN_DETAIL=1). The
// QML scene-graph update is the suspected hot path for QuickSceneEffect
// stutter (Overview, Window-View, etc.); split it into polishItems,
// beginFrame, sync, render, endFrame to attribute the cost. Threshold
// shared with the per-effect breakdown so the sidecar stays spike-only.
static const bool s_offscreenStageBreakdown =
    qEnvironmentVariableIntValue("KWIN_FRAME_BREAKDOWN_DETAIL") != 0;
static const std::chrono::nanoseconds s_offscreenStageThreshold =
    std::chrono::milliseconds(qEnvironmentVariableIsSet("KWIN_FRAME_BREAKDOWN_DETAIL_THRESHOLD_MS")
                                  ? qEnvironmentVariableIntValue("KWIN_FRAME_BREAKDOWN_DETAIL_THRESHOLD_MS")
                                  : 1);

// One sidecar file shared across all OffscreenQuickView instances in the
// process. update() runs on the main thread so the mutex is uncontended
// in practice, but it's cheap insurance against unexpected threading.
static std::mutex s_offscreenStageMutex;
static std::optional<std::fstream> s_offscreenStageOutput;
static bool s_offscreenStageHeaderWritten = false;

static void writeOffscreenStageRow(QQuickWindow *view,
                                   std::chrono::nanoseconds wall,
                                   std::chrono::nanoseconds total,
                                   std::chrono::nanoseconds polish,
                                   std::chrono::nanoseconds beginFrame,
                                   std::chrono::nanoseconds sync,
                                   std::chrono::nanoseconds render,
                                   std::chrono::nanoseconds endFrame)
{
    std::lock_guard lock(s_offscreenStageMutex);
    if (!s_offscreenStageOutput) {
        const std::string path = "kwin perf offscreenquick detail (default).csv";
        s_offscreenStageOutput = std::fstream(path, std::ios::out);
        std::error_code ec;
        const auto absPath = std::filesystem::absolute(path, ec);
        qCWarning(LIBKWINEFFECTS).noquote()
            << "OffscreenQuickView: writing stage breakdown to"
            << QString::fromStdString(ec ? path : absPath.string());
    }
    if (!s_offscreenStageHeaderWritten) {
        *s_offscreenStageOutput
            << "wall_ns,view_size,total_ns,polish_ns,beginframe_ns,sync_ns,render_ns,endframe_ns\n";
        s_offscreenStageHeaderWritten = true;
    }
    const QSize size = view ? view->size() : QSize();
    *s_offscreenStageOutput
        << wall.count() << ","
        << size.width() << "x" << size.height() << ","
        << total.count() << "," << polish.count() << "," << beginFrame.count()
        << "," << sync.count() << "," << render.count() << "," << endFrame.count() << "\n";
}

class Q_DECL_HIDDEN OffscreenQuickView::Private
{
public:
    std::unique_ptr<QQuickWindow> m_view;
    std::unique_ptr<QQuickRenderControl> m_renderControl;
    std::unique_ptr<QOffscreenSurface> m_offscreenSurface;
    std::unique_ptr<QOpenGLContext> m_glcontext;
    std::unique_ptr<QOpenGLFramebufferObject> m_fbo;

    std::unique_ptr<QTimer> m_repaintTimer;
    QImage m_image;
    std::unique_ptr<GLTexture> m_textureExport;
    // if we should capture a QImage after rendering into our BO.
    // Used for either software QtQuick rendering and nonGL kwin rendering
    bool m_useBlit = false;
    bool m_visible = true;
    bool m_hasAlphaChannel = true;
    bool m_automaticRepaint = true;

#if HAVE_VULKAN
    // Set when Qt Quick has been initialized with kwin's Vulkan device. In this mode
    // Qt Quick renders directly into m_vkColorTexture and the renderer samples it
    // without a CPU roundtrip.
    VulkanBackend *m_vulkanBackend = nullptr;
    std::unique_ptr<VulkanTexture> m_vkColorTexture;
    // Layout the image is in when handed back to Qt for the next frame. Qt's RHI
    // accepts UNDEFINED for the very first frame, then SHADER_READ_ONLY_OPTIMAL.
    VkImageLayout m_vkLastLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // Escape-hatch fallback (KWIN_FORCE_QT_GL_RHI=1 on Vulkan compositor):
    // Qt rendered into a GL FBO, m_image was populated from it via the blit
    // path, and we upload that into a VulkanTexture so the Vulkan compositor
    // consumer (EffectsHandler::renderOffscreenQuickView's Vulkan branch)
    // can still sample it. Refreshed only when m_image's cache key changes.
    std::unique_ptr<VulkanTexture> m_vkUploadedFromImage;
    qint64 m_vkUploadedCacheKey = 0;
#endif

    std::optional<qreal> m_explicitDpr;

    QList<QEventPoint> touchPoints;
    QSet<uint32_t> acceptedTouchPoints;
    QPointingDevice *touchDevice;

    ulong lastMousePressTime = 0;
    Qt::MouseButton lastMousePressButton = Qt::NoButton;

    void releaseResources();

    void updateTouchState(Qt::TouchPointState state, qint32 id, const QPointF &pos);
};

class Q_DECL_HIDDEN OffscreenQuickScene::Private
{
public:
    Private()
    {
    }

    std::unique_ptr<QQmlComponent> qmlComponent;
    std::unique_ptr<QQuickItem> quickItem;
};

OffscreenQuickView::OffscreenQuickView(ExportMode exportMode, bool alpha)
    : d(new OffscreenQuickView::Private)
{
#if HAVE_VULKAN
    // KWIN_FORCE_QT_GL_RHI=1 routes Qt Quick onto the OpenGL RHI even when
    // kwin's compositor is Vulkan — the escape hatch for bisecting Qt
    // Vulkan-RHI regressions and for drivers where Qt's Vulkan backend is
    // known broken. The flag is read once at process start to match the
    // process-global nature of setGraphicsApi(). compositor_x11.cpp gates
    // its own setGraphicsApi(OpenGL) on the same env var; both gates have
    // to agree, since OffscreenQuickView is the first thing to actually
    // create a QQuickWindow and Qt locks the API at that point.
    static const bool s_forceQtGl = qEnvironmentVariableIsSet("KWIN_FORCE_QT_GL_RHI");
    const bool useVulkan = effects && effects->compositingType() == VulkanCompositing && !s_forceQtGl;
#else
    const bool useVulkan = false;
#endif

    // When the compositor uses OpenGL we force Qt Quick's scene graph onto the OpenGL
    // backend (it would otherwise pick Vulkan if no global GL share context exists and
    // crash inside QRhi with a null device). Under a Vulkan compositor we instead hand
    // Qt Quick our VkDevice directly — see the Vulkan branch below.
    if (!useVulkan
        && !QOpenGLContext::globalShareContext()
        && QQuickWindow::sceneGraphBackend() != QLatin1String("opengl")) {
        QQuickWindow::setSceneGraphBackend(QStringLiteral("opengl"));
    }
#if HAVE_VULKAN
    // Qt's RHI scene-graph backend defaults to OpenGL on Linux and only honors
    // setGraphicsDevice() if the requested graphics API matches. setGraphicsApi()
    // is process-global and must be called before any QQuickWindow is created;
    // doing it here means the very first OffscreenQuickView pins the API for the
    // process. That matches kwin's single-backend lifetime.
    if (useVulkan) {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
    }
#endif

    d->m_renderControl = std::make_unique<QQuickRenderControl>();

    d->m_view = std::make_unique<QQuickWindow>(d->m_renderControl.get());
    Q_ASSERT(d->m_view->setProperty("_KWIN_WINDOW_IS_OFFSCREEN", true) || true);
    d->m_view->setFlags(Qt::FramelessWindowHint);
    d->m_view->setColor(Qt::transparent);

    d->m_hasAlphaChannel = alpha;
    if (exportMode == ExportMode::Image) {
        d->m_useBlit = true;
    }

#if HAVE_VULKAN
    // Native Vulkan path: hand Qt Quick kwin's VkInstance/VkDevice so the scene graph
    // renders directly into a VkImage we own. No FBO, no QImage roundtrip.
    bool vulkanReady = false;
    if (useVulkan) {
        VulkanContext *ctx = VulkanContext::currentContext();
        VulkanBackend *backend = ctx ? ctx->backend() : nullptr;
        QVulkanInstance *qInst = backend ? backend->qVulkanInstance() : nullptr;
        if (backend && qInst && backend->device() != VK_NULL_HANDLE) {
            d->m_view->setVulkanInstance(qInst);
            d->m_view->setGraphicsDevice(QQuickGraphicsDevice::fromDeviceObjects(
                backend->physicalDevice(),
                backend->device(),
                int(backend->graphicsQueueFamily()),
                /*queueIndex=*/0));
            const bool initialized = d->m_renderControl->initialize();
            const auto api = d->m_view->rendererInterface()->graphicsApi();
            if (initialized && api == QSGRendererInterface::Vulkan) {
                d->m_vulkanBackend = backend;
                d->m_useBlit = false;
                vulkanReady = true;
            } else {
                qCWarning(LIBKWINEFFECTS) << "Qt Quick failed to initialize with Vulkan (initialized="
                                          << initialized << "api=" << api
                                          << "), Qt Quick effects will be disabled";
                d->m_useBlit = true;
            }
        } else {
            qCWarning(LIBKWINEFFECTS) << "VulkanBackend/QVulkanInstance unavailable; Qt Quick effects disabled";
            d->m_useBlit = true;
        }
    }
    if (!vulkanReady) {
#else
    {
#endif
        // Always try to create an OpenGL context for Qt Quick rendering.
        // On X11, GLX is available independently of the compositor's backend,
        // so a standalone GL context works even when kwin uses Vulkan.
        // Checking graphicsApi() == OpenGL is not reliable here: under a Vulkan
        // compositor Qt Quick reports Vulkan as its API, causing the old code to
        // skip GL setup and call sync() with a null QRhi → SIGSEGV.
        QSurfaceFormat format;
        format.setOption(QSurfaceFormat::ResetNotification);
        format.setDepthBufferSize(16);
        format.setStencilBufferSize(8);
        if (alpha) {
            format.setAlphaBufferSize(8);
        }

        d->m_view->setFormat(format);

        auto shareContext = QOpenGLContext::globalShareContext();
        auto glcontext = std::make_unique<QOpenGLContext>();
        glcontext->setShareContext(shareContext);
        glcontext->setFormat(format);

        if (glcontext->create()) {
            d->m_glcontext = std::move(glcontext);

            d->m_offscreenSurface = std::make_unique<QOffscreenSurface>();
            d->m_offscreenSurface->setFormat(d->m_glcontext->format());
            d->m_offscreenSurface->create();

            d->m_glcontext->makeCurrent(d->m_offscreenSurface.get());
            d->m_view->setGraphicsDevice(QQuickGraphicsDevice::fromOpenGLContext(d->m_glcontext.get()));
            const bool initialized = d->m_renderControl->initialize();

            // Under a Vulkan compositor Qt Quick may ignore our setGraphicsDevice()
            // and use Vulkan anyway, leaving QSGRenderContext::rhi() null. Verify
            // the result is actually OpenGL before committing to the GL path.
            const auto actualApi = d->m_view->rendererInterface()->graphicsApi();
            if (!initialized || actualApi != QSGRendererInterface::OpenGL) {
                qCWarning(LIBKWINEFFECTS) << "Qt Quick render control did not initialize with OpenGL (initialized="
                                          << initialized << "api=" << actualApi
                                          << "), Qt Quick effects will be disabled";
                d->m_glcontext->doneCurrent();
                d->m_glcontext.reset();
                d->m_offscreenSurface.reset();
                d->m_useBlit = true;
            } else {
                d->m_glcontext->doneCurrent();

                // If the GL context is not sharing with kwin's main context, or if
                // the compositor is not OpenGL (Vulkan reads via bufferAsImage(), not
                // bufferAsTexture()), force blit mode so the QImage path is populated.
                if (!d->m_glcontext->shareContext() || effects->compositingType() != OpenGLCompositing) {
                    d->m_useBlit = true;
                }
            }
        } else {
            // GL context creation failed — only the software backend works without it.
            const auto api = d->m_view->rendererInterface()->graphicsApi();
            if (api == QSGRendererInterface::Software) {
                qCDebug(LIBKWINEFFECTS) << "No GL context available, using Qt Quick software renderer";
                d->m_useBlit = true;
                // explicitly do not call QQuickRenderControl::initialize for software mode
            } else {
                qCWarning(LIBKWINEFFECTS) << "No GL context and Qt Quick is not in software mode (api=" << api
                                          << "), Qt Quick effects will be disabled";
                d->m_useBlit = true;
            }
        }
    }

    // KWIN_DEBUG_QT_RHI=1 emits one line per OffscreenQuickView construction
    // logging which graphics API Qt Quick actually ended up using and
    // whether the blit-via-QImage fallback was forced. Discoverable
    // diagnostic for the Vulkan-Qt-Quick rollout; doesn't ship anything at
    // runtime cost beyond an env-var lookup. Logged at warning level so it
    // appears without QT_LOGGING_RULES tweaks — the env var is the opt-in.
    static const bool debugQtRhi = qEnvironmentVariableIsSet("KWIN_DEBUG_QT_RHI");
    if (debugQtRhi) {
        const auto api = d->m_view->rendererInterface() ? d->m_view->rendererInterface()->graphicsApi()
                                                        : QSGRendererInterface::Unknown;
        qCWarning(LIBKWINEFFECTS) << "OffscreenQuickView: Qt Quick graphicsApi=" << api
                                  << "useBlit=" << d->m_useBlit
                                  << "(compositor=" << (effects ? effects->compositingType() : -1) << ")";
    }

    auto updateSize = [this]() {
        contentItem()->setSize(d->m_view->size());
    };
    updateSize();
    connect(d->m_view.get(), &QWindow::widthChanged, this, updateSize);
    connect(d->m_view.get(), &QWindow::heightChanged, this, updateSize);

    d->m_repaintTimer = std::make_unique<QTimer>();
    d->m_repaintTimer->setSingleShot(true);
    d->m_repaintTimer->setInterval(10);

    connect(d->m_repaintTimer.get(), &QTimer::timeout, this, &OffscreenQuickView::update);
    connect(d->m_renderControl.get(), &QQuickRenderControl::renderRequested, this, &OffscreenQuickView::handleRenderRequested);
    connect(d->m_renderControl.get(), &QQuickRenderControl::sceneChanged, this, &OffscreenQuickView::handleSceneChanged);

    d->touchDevice = new QPointingDevice(QStringLiteral("ForwardingTouchDevice"), {}, QInputDevice::DeviceType::TouchScreen, QPointingDevice::PointerType::Finger, QInputDevice::Capability::Position, 10, {});
}

OffscreenQuickView::~OffscreenQuickView()
{
    disconnect(d->m_renderControl.get(), &QQuickRenderControl::renderRequested, this, &OffscreenQuickView::handleRenderRequested);
    disconnect(d->m_renderControl.get(), &QQuickRenderControl::sceneChanged, this, &OffscreenQuickView::handleSceneChanged);

    if (d->m_glcontext) {
        // close the view whilst we have an active GL context
        d->m_glcontext->makeCurrent(d->m_offscreenSurface.get());
    }

    d->m_view.reset();
    d->m_renderControl.reset();
}

bool OffscreenQuickView::automaticRepaint() const
{
    return d->m_automaticRepaint;
}

void OffscreenQuickView::setAutomaticRepaint(bool set)
{
    if (d->m_automaticRepaint != set) {
        d->m_automaticRepaint = set;

        // If there's an in-flight update, disable it.
        if (!d->m_automaticRepaint) {
            d->m_repaintTimer->stop();
        }
    }
}

void OffscreenQuickView::setDevicePixelRatio(qreal dpr)
{
    d->m_explicitDpr = dpr;
}

void OffscreenQuickView::handleSceneChanged()
{
    if (d->m_automaticRepaint) {
        d->m_repaintTimer->start();
    }
    Q_EMIT sceneChanged();
}

void OffscreenQuickView::handleRenderRequested()
{
    if (d->m_automaticRepaint) {
        d->m_repaintTimer->start();
    }
    Q_EMIT renderRequested();
}

void OffscreenQuickView::update()
{
    if (!d->m_visible) {
        return;
    }
    if (d->m_view->size().isEmpty()) {
        return;
    }

    bool usingGl = d->m_glcontext != nullptr;
#if HAVE_VULKAN
    bool usingVulkan = d->m_vulkanBackend != nullptr;
#else
    constexpr bool usingVulkan = false;
#endif
    OpenGlContext *previousContext = OpenGlContext::currentContext();

    if (!usingGl && !usingVulkan) {
        // No hardware context. Only the software backend can render without one;
        // any hardware backend (Vulkan, RHI) requires initialize() to have
        // been called, which we skip when there is no GL context. Guard here
        // so that sync() never reaches QSGBatchRenderer with a null QRhi.
        const auto api = d->m_view->rendererInterface()->graphicsApi();
        if (api != QSGRendererInterface::Software) {
            return;
        }
    }

    if (usingGl) {
        if (!d->m_glcontext->makeCurrent(d->m_offscreenSurface.get())) {
            // probably a context loss event, kwin is about to reset all the effects anyway
            return;
        }

        qreal dpr = d->m_view->screen() ? d->m_view->screen()->devicePixelRatio() : 1.0;
        if (d->m_explicitDpr.has_value()) {
            dpr = d->m_explicitDpr.value();
        }

        const QSize nativeSize = d->m_view->size() * dpr;
        if (!d->m_fbo || d->m_fbo->size() != nativeSize) {
            d->m_textureExport.reset(nullptr);

            QOpenGLFramebufferObjectFormat fboFormat;
            fboFormat.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
            fboFormat.setInternalTextureFormat(GL_RGBA8);

            d->m_fbo = std::make_unique<QOpenGLFramebufferObject>(nativeSize, fboFormat);
            if (!d->m_fbo->isValid()) {
                d->m_fbo.reset();
                d->m_glcontext->doneCurrent();
                return;
            }
        }

        QQuickRenderTarget renderTarget = QQuickRenderTarget::fromOpenGLTexture(d->m_fbo->texture(), d->m_fbo->size());
        renderTarget.setDevicePixelRatio(dpr);
        d->m_view->setRenderTarget(renderTarget);
    }

#if HAVE_VULKAN
    if (usingVulkan) {
        qreal dpr = d->m_view->screen() ? d->m_view->screen()->devicePixelRatio() : 1.0;
        if (d->m_explicitDpr.has_value()) {
            dpr = d->m_explicitDpr.value();
        }

        const QSize nativeSize = d->m_view->size() * dpr;
        if (!d->m_vkColorTexture || d->m_vkColorTexture->size() != nativeSize) {
            VulkanContext *ctx = VulkanContext::currentContext();
            if (!ctx) {
                return;
            }
            // Mutable-format storage: Qt writes its sRGB-encoded color values into a
            // UNORM image (so neither Qt's QRhi nor the hardware re-encodes them);
            // our renderer samples through the SRGB alias view so the hardware does
            // sRGB→linear on read, and the swapchain SRGB attachment encodes once
            // on write. The X11 surface texture uses the same trick (sample via SRGB
            // view) for the same reason.
            d->m_vkColorTexture = VulkanTexture::createMutableRenderTarget(
                ctx, nativeSize,
                VK_FORMAT_R8G8B8A8_UNORM,
                VK_FORMAT_R8G8B8A8_SRGB);
            if (!d->m_vkColorTexture || !d->m_vkColorTexture->isValid()) {
                d->m_vkColorTexture.reset();
                return;
            }
            // Newly allocated image: Qt expects UNDEFINED for the first frame.
            d->m_vkLastLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        }

        QQuickRenderTarget renderTarget = QQuickRenderTarget::fromVulkanImage(
            d->m_vkColorTexture->image(),
            d->m_vkLastLayout,
            VK_FORMAT_R8G8B8A8_UNORM,
            d->m_vkColorTexture->size());
        renderTarget.setDevicePixelRatio(dpr);
        d->m_view->setRenderTarget(renderTarget);
    }
#endif

    if (!s_offscreenStageBreakdown) {
        d->m_renderControl->polishItems();
        if (usingGl || usingVulkan) {
            d->m_renderControl->beginFrame();
        }
        d->m_renderControl->sync();
        d->m_renderControl->render();
        if (usingGl || usingVulkan) {
            d->m_renderControl->endFrame();
        }
    } else {
        // Stage-level breakdown for QuickSceneEffect stutter attribution.
        // The same five calls, each bracketed; sidecar row only when total
        // exceeds the threshold so steady-state frames don't flood the log.
        using clock = std::chrono::steady_clock;
        const auto t0 = clock::now();
        d->m_renderControl->polishItems();
        const auto t1 = clock::now();
        if (usingGl || usingVulkan) {
            d->m_renderControl->beginFrame();
        }
        const auto t2 = clock::now();
        d->m_renderControl->sync();
        const auto t3 = clock::now();
        d->m_renderControl->render();
        const auto t4 = clock::now();
        if (usingGl || usingVulkan) {
            d->m_renderControl->endFrame();
        }
        const auto t5 = clock::now();
        const auto total = t5 - t0;
        if (total >= s_offscreenStageThreshold) {
            writeOffscreenStageRow(d->m_view.get(),
                                   t5.time_since_epoch(),
                                   total,
                                   t1 - t0, t2 - t1, t3 - t2, t4 - t3, t5 - t4);
        }
    }

    if (usingGl) {
        QQuickOpenGLUtils::resetOpenGLState();
    }

#if HAVE_VULKAN
    if (usingVulkan) {
        // Qt's RHI emits a barrier to SHADER_READ_ONLY_OPTIMAL at end-of-frame when
        // the render target is also a sampled texture (which createRenderTarget()
        // configured above). Track that so the next frame and downstream samplers
        // both know the current layout.
        d->m_vkLastLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        d->m_vkColorTexture->setCurrentLayout(d->m_vkLastLayout);
    }
#endif

    if (d->m_useBlit) {
        if (usingGl) {
            d->m_image = d->m_fbo->toImage();
            d->m_image.setDevicePixelRatio(d->m_view->effectiveDevicePixelRatio());
        } else if (!usingVulkan) {
            // Pre-Vulkan-RHI fallback (software / failed-init): grabWindow()
            // readback is the only image path available, and the consumer
            // either explicitly asked for an image (ExportMode::Image) or
            // is sampling via bufferAsTexture which uploads from this image.
            d->m_image = d->m_view->grabWindow();
        }
        // usingVulkan: skip the eager readback. The in-tree consumer on the
        // Vulkan compositor (effecthandler.cpp's renderOffscreenQuickView
        // Vulkan branch) reads vulkanTexture() directly, so m_image is
        // computed and never used. bufferAsImage() lazily grabs the window
        // if anything outside that path actually needs the QImage.
    }

    if (usingGl) {
        QOpenGLFramebufferObject::bindDefault();
        d->m_glcontext->doneCurrent();
        if (previousContext) {
            previousContext->makeCurrent();
        }
    }
    Q_EMIT repaintNeeded();
}

void OffscreenQuickView::forwardMouseEvent(QEvent *e)
{
    if (!d->m_visible) {
        return;
    }
    switch (e->type()) {
    case QEvent::MouseMove:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease: {
        QMouseEvent *me = static_cast<QMouseEvent *>(e);
        const QPoint widgetPos = d->m_view->mapFromGlobal(me->pos());
        QMouseEvent cloneEvent(me->type(), widgetPos, me->pos(), me->button(), me->buttons(), me->modifiers());
        cloneEvent.setAccepted(false);
        QCoreApplication::sendEvent(d->m_view.get(), &cloneEvent);
        e->setAccepted(cloneEvent.isAccepted());

        if (e->type() == QEvent::MouseButtonPress) {
            const ulong doubleClickInterval = static_cast<ulong>(QGuiApplication::styleHints()->mouseDoubleClickInterval());
            const bool doubleClick = (me->timestamp() - d->lastMousePressTime < doubleClickInterval) && me->button() == d->lastMousePressButton;
            d->lastMousePressTime = me->timestamp();
            d->lastMousePressButton = me->button();
            if (doubleClick) {
                d->lastMousePressButton = Qt::NoButton;
                QMouseEvent doubleClickEvent(QEvent::MouseButtonDblClick, me->position(), me->globalPosition(), me->button(), me->buttons(), me->modifiers());
                QCoreApplication::sendEvent(d->m_view.get(), &doubleClickEvent);
            }
        }

        return;
    }
    case QEvent::HoverEnter:
    case QEvent::HoverLeave:
    case QEvent::HoverMove: {
        QHoverEvent *he = static_cast<QHoverEvent *>(e);
        const QPointF widgetPos = d->m_view->mapFromGlobal(he->position().toPoint());
        const QPointF oldWidgetPos = d->m_view->mapFromGlobal(he->oldPos());
        QHoverEvent cloneEvent(he->type(), widgetPos, oldWidgetPos, he->modifiers());
        cloneEvent.setAccepted(false);
        QCoreApplication::sendEvent(d->m_view.get(), &cloneEvent);
        e->setAccepted(cloneEvent.isAccepted());
        return;
    }
    case QEvent::Wheel: {
        QWheelEvent *we = static_cast<QWheelEvent *>(e);
        const QPointF widgetPos = d->m_view->mapFromGlobal(we->position().toPoint());
        QWheelEvent cloneEvent(widgetPos, we->globalPosition(), we->pixelDelta(), we->angleDelta(), we->buttons(),
                               we->modifiers(), we->phase(), we->inverted());
        cloneEvent.setAccepted(false);
        QCoreApplication::sendEvent(d->m_view.get(), &cloneEvent);
        e->setAccepted(cloneEvent.isAccepted());
        return;
    }
    default:
        return;
    }
}

void OffscreenQuickView::forwardKeyEvent(QKeyEvent *keyEvent)
{
    if (!d->m_visible) {
        return;
    }
    QCoreApplication::sendEvent(d->m_view.get(), keyEvent);
}

bool OffscreenQuickView::forwardTouchDown(qint32 id, const QPointF &pos, std::chrono::microseconds time)
{
    d->updateTouchState(Qt::TouchPointPressed, id, pos);

    QTouchEvent event(QEvent::TouchBegin, d->touchDevice, Qt::NoModifier, d->touchPoints);
    event.setTimestamp(std::chrono::duration_cast<std::chrono::milliseconds>(time).count());
    event.setAccepted(false);
    QCoreApplication::sendEvent(d->m_view.get(), &event);

    const bool ret = event.isAccepted();
    if (ret) {
        d->acceptedTouchPoints.insert(id);
    }
    return ret;
}

bool OffscreenQuickView::forwardTouchMotion(qint32 id, const QPointF &pos, std::chrono::microseconds time)
{
    d->updateTouchState(Qt::TouchPointMoved, id, pos);

    if (!d->acceptedTouchPoints.contains(id)) {
        return false;
    }

    QTouchEvent event(QEvent::TouchUpdate, d->touchDevice, Qt::NoModifier, d->touchPoints);
    event.setTimestamp(std::chrono::duration_cast<std::chrono::milliseconds>(time).count());
    event.setAccepted(false);
    QCoreApplication::sendEvent(d->m_view.get(), &event);

    return event.isAccepted();
}

bool OffscreenQuickView::forwardTouchUp(qint32 id, std::chrono::microseconds time)
{
    d->updateTouchState(Qt::TouchPointReleased, id, QPointF{});

    if (!d->acceptedTouchPoints.contains(id)) {
        return false;
    }

    QTouchEvent event(QEvent::TouchEnd, d->touchDevice, Qt::NoModifier, d->touchPoints);
    event.setTimestamp(std::chrono::duration_cast<std::chrono::milliseconds>(time).count());
    event.setAccepted(false);
    QCoreApplication::sendEvent(d->m_view.get(), &event);

    d->acceptedTouchPoints.remove(id);

    return event.isAccepted();
}

void OffscreenQuickView::forwardTouchCancel()
{
    d->acceptedTouchPoints.clear();
    d->touchPoints.clear();
    QTouchEvent event(QEvent::TouchCancel, d->touchDevice, Qt::NoModifier, d->touchPoints);
    event.setTimestamp(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    event.setAccepted(false);
    QCoreApplication::sendEvent(d->m_view.get(), &event);
}

QRect OffscreenQuickView::geometry() const
{
    return d->m_view->geometry();
}

void OffscreenQuickView::setOpacity(qreal opacity)
{
    d->m_view->setOpacity(opacity);
}

qreal OffscreenQuickView::opacity() const
{
    return d->m_view->opacity();
}

bool OffscreenQuickView::hasAlphaChannel() const
{
    return d->m_hasAlphaChannel;
}

QQuickItem *OffscreenQuickView::contentItem() const
{
    return d->m_view->contentItem();
}

QQuickWindow *OffscreenQuickView::window() const
{
    return d->m_view.get();
}

void OffscreenQuickView::setVisible(bool visible)
{
    if (d->m_visible == visible) {
        return;
    }
    d->m_visible = visible;

    if (visible) {
        Q_EMIT d->m_renderControl->renderRequested();
    } else {
        // deferred to not change GL context
        QTimer::singleShot(0, this, [this]() {
            d->releaseResources();
        });
    }
}

bool OffscreenQuickView::isVisible() const
{
    return d->m_visible;
}

void OffscreenQuickView::show()
{
    setVisible(true);
}

void OffscreenQuickView::hide()
{
    setVisible(false);
}

GLTexture *OffscreenQuickView::bufferAsTexture()
{
    if (d->m_useBlit) {
        d->m_textureExport = GLTexture::upload(d->m_image);
    } else {
        if (!d->m_fbo) {
            return nullptr;
        }
        if (!d->m_textureExport) {
            d->m_textureExport = GLTexture::createNonOwningWrapper(d->m_fbo->texture(), d->m_fbo->format().internalTextureFormat(), d->m_fbo->size());
        }
    }
    return d->m_textureExport.get();
}

QImage OffscreenQuickView::bufferAsImage() const
{
#if HAVE_VULKAN
    // On the Vulkan-Qt-RHI path update() deliberately skipped the eager
    // grabWindow() readback because nothing on the Vulkan compositor's
    // render path actually consumes m_image — vulkanTexture() is the
    // direct consumer. The public API still has to honor a call, though,
    // so populate lazily here. Pointer-PIMPL means d->m_image is mutable
    // from a const method without needing the mutable keyword.
    if (d->m_image.isNull() && d->m_vulkanBackend && d->m_view) {
        d->m_image = d->m_view->grabWindow();
    }
#endif
    return d->m_image;
}

#if HAVE_VULKAN
VulkanTexture *OffscreenQuickView::vulkanTexture() const
{
    if (d->m_vkColorTexture) {
        return d->m_vkColorTexture.get();
    }
    // Escape-hatch path: Qt rendered into a GL FBO and we have a CPU-side
    // QImage of it. Lazily upload to a cached VulkanTexture so the
    // consumer can sample without changing its code. Per-frame upload is
    // expected on this path — it's only taken when KWIN_FORCE_QT_GL_RHI=1
    // intentionally trades performance for a working escape from Qt's
    // Vulkan RHI.
    if (d->m_image.isNull()) {
        return nullptr;
    }
    const qint64 currentKey = d->m_image.cacheKey();
    if (!d->m_vkUploadedFromImage || d->m_vkUploadedCacheKey != currentKey) {
        if (VulkanContext *ctx = VulkanContext::currentContext()) {
            auto uploaded = VulkanTexture::upload(ctx, d->m_image);
            if (uploaded && uploaded->isValid()) {
                d->m_vkUploadedFromImage = std::move(uploaded);
                d->m_vkUploadedCacheKey = currentKey;
            }
        }
    }
    return d->m_vkUploadedFromImage.get();
}
#endif

QSize OffscreenQuickView::size() const
{
    return d->m_view->geometry().size();
}

void OffscreenQuickView::setGeometry(const QRect &rect)
{
    const QRect oldGeometry = d->m_view->geometry();
    d->m_view->setGeometry(rect);
    // QWindow::setGeometry() won't sync output if there's no platform window.
    d->m_view->setScreen(QGuiApplication::screenAt(rect.center()));
    Q_EMIT geometryChanged(oldGeometry, rect);
}

void OffscreenQuickView::Private::releaseResources()
{
    if (m_glcontext) {
        m_glcontext->makeCurrent(m_offscreenSurface.get());
        m_view->releaseResources();
        m_glcontext->doneCurrent();
    } else {
        m_view->releaseResources();
    }
}

void OffscreenQuickView::Private::updateTouchState(Qt::TouchPointState state, qint32 id, const QPointF &pos)
{
    // Remove the points that were previously in a released state, since they
    // are no longer relevant. Additionally, reset the state of all remaining
    // points to Stationary so we only have one touch point with a different
    // state.
    touchPoints.erase(std::remove_if(touchPoints.begin(), touchPoints.end(), [](QTouchEvent::TouchPoint &point) {
                          if (point.state() == QEventPoint::Released) {
                              return true;
                          }
                          QMutableEventPoint::setState(point, QEventPoint::Stationary);
                          return false;
                      }),
                      touchPoints.end());

    // QtQuick Pointer Handlers incorrectly consider a touch point with ID 0
    // to be an invalid touch point. This has been fixed in Qt 6 but could not
    // be fixed for Qt 5. Instead, we offset kwin's internal IDs with this
    // offset to trick QtQuick into treating them as valid points.
    static const qint32 idOffset = 111;

    // Find the touch point that has changed. This is separate from the above
    // loop because removing the released touch points invalidates iterators.
    auto changed = std::find_if(touchPoints.begin(), touchPoints.end(), [id](const QTouchEvent::TouchPoint &point) {
        return point.id() == id + idOffset;
    });

    switch (state) {
    case Qt::TouchPointPressed: {
        if (changed != touchPoints.end()) {
            return;
        }

        QTouchEvent::TouchPoint point;
        QMutableEventPoint::setState(point, QEventPoint::Pressed);
        QMutableEventPoint::setId(point, id + idOffset);
        QMutableEventPoint::setGlobalPosition(point, pos);
        QMutableEventPoint::setScenePosition(point, m_view->mapFromGlobal(pos.toPoint()));
        QMutableEventPoint::setPosition(point, m_view->mapFromGlobal(pos.toPoint()));

        touchPoints.append(point);
    } break;
    case Qt::TouchPointMoved: {
        if (changed == touchPoints.end()) {
            return;
        }

        auto &point = *changed;
        QMutableEventPoint::setGlobalLastPosition(point, point.globalPosition());
        QMutableEventPoint::setState(point, QEventPoint::Updated);
        QMutableEventPoint::setScenePosition(point, m_view->mapFromGlobal(pos.toPoint()));
        QMutableEventPoint::setPosition(point, m_view->mapFromGlobal(pos.toPoint()));
        QMutableEventPoint::setGlobalPosition(point, pos);
    } break;
    case Qt::TouchPointReleased: {
        if (changed == touchPoints.end()) {
            return;
        }

        auto &point = *changed;
        QMutableEventPoint::setGlobalLastPosition(point, point.globalPosition());
        QMutableEventPoint::setState(point, QEventPoint::Released);
    } break;
    default:
        break;
    }
}

OffscreenQuickScene::OffscreenQuickScene(OffscreenQuickView::ExportMode exportMode, bool alpha)
    : OffscreenQuickView(exportMode, alpha)
    , d(new OffscreenQuickScene::Private)
{
}

OffscreenQuickScene::~OffscreenQuickScene() = default;

void OffscreenQuickScene::setSource(const QUrl &source)
{
    setSource(source, QVariantMap());
}

void OffscreenQuickScene::setSource(const QUrl &source, const QVariantMap &initialProperties)
{
    if (!d->qmlComponent) {
        d->qmlComponent = std::make_unique<QQmlComponent>(effects->qmlEngine());
    }

    d->qmlComponent->loadUrl(source);
    if (d->qmlComponent->isError()) {
        qCWarning(LIBKWINEFFECTS).nospace() << "Failed to load effect quick view " << source << ": " << d->qmlComponent->errors();
        d->qmlComponent.reset();
        return;
    }

    d->quickItem.reset();

    std::unique_ptr<QObject> qmlObject(d->qmlComponent->createWithInitialProperties(initialProperties));
    QQuickItem *item = qobject_cast<QQuickItem *>(qmlObject.get());
    if (!item) {
        qCWarning(LIBKWINEFFECTS) << "Root object of effect quick view" << source << "is not a QQuickItem";
        return;
    }

    qmlObject.release();
    d->quickItem.reset(item);

    item->setParentItem(contentItem());

    auto updateSize = [item, this]() {
        item->setSize(contentItem()->size());
    };
    updateSize();
    connect(contentItem(), &QQuickItem::widthChanged, item, updateSize);
    connect(contentItem(), &QQuickItem::heightChanged, item, updateSize);
}

QQuickItem *OffscreenQuickScene::rootItem() const
{
    return d->quickItem.get();
}

} // namespace KWin

#include "moc_offscreenquickview.cpp"
