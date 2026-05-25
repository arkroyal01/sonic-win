/*
    SPDX-FileCopyrightText: 2011 Martin Gräßlin <mgraesslin@kde.org>
    SPDX-FileCopyrightText: 2020 David Edmundson <davidedmundson@kde.org>
    SPDX-FileCopyrightText: 2021 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "windowthumbnailitem.h"
#include "compositor.h"
#include "core/renderbackend.h"
#include "core/rendertarget.h"
#include "core/renderviewport.h"
#include "effect/effect.h"
#include "opengl/glframebuffer.h"
#include "scene/itemrenderer.h"
#include "scene/windowitem.h"
#include "scene/workspacescene.h"
#include "scripting_logging.h"
#include "window.h"
#include "workspace.h"

#include "opengl/gltexture.h"

#if HAVE_VULKAN
#include "platformsupport/scenes/vulkan/vulkanbackend.h"
#include "platformsupport/scenes/vulkan/vulkancontext.h"
#include "platformsupport/scenes/vulkan/vulkanframebuffer.h"
#include "platformsupport/scenes/vulkan/vulkanrenderpass.h"
#include "platformsupport/scenes/vulkan/vulkanrendertarget.h"
#include "platformsupport/scenes/vulkan/vulkantexture.h"
#include "scene/itemrenderer_vulkan.h"
#include "scene/workspacescene_vulkan.h"
#endif

#include <QHash>
#include <QOpenGLContext>
#include <QQuickWindow>
#include <QRunnable>
#include <QSGImageNode>
#include <QSGTextureProvider>
#include <QTimer>

namespace KWin
{

// Damage-rate diagnostic for the WindowThumbnailSource pipeline. When
// KWIN_FRAME_BREAKDOWN_DETAIL=1, counts how many damage signals each
// source receives per second and logs the top contributors. The damage
// signal is what propagates QML "dirty" through to the desktop-tile
// layer FBO; a flood here forces the per-tile FBO re-render every frame.
static const bool s_thumbnailDamageDiagnostic =
    qEnvironmentVariableIntValue("KWIN_FRAME_BREAKDOWN_DETAIL") != 0;

namespace
{
struct DamageCounter
{
    // Window* used as a key; the flush callback only reads caption() for
    // entries whose Window is still alive at flush time. Captions cached
    // alongside the count so a window that fires damage then immediately
    // closes still shows up by name in the last log line.
    QHash<Window *, std::pair<uint32_t, QString>> perWindow;
    QTimer *flushTimer = nullptr;
};
static DamageCounter &damageCounter()
{
    static DamageCounter c;
    return c;
}
static void recordDamage(Window *handle)
{
    if (!s_thumbnailDamageDiagnostic || !handle) {
        return;
    }
    auto &c = damageCounter();
    if (!c.flushTimer) {
        c.flushTimer = new QTimer();
        c.flushTimer->setInterval(1000);
        c.flushTimer->setTimerType(Qt::CoarseTimer);
        QObject::connect(c.flushTimer, &QTimer::timeout, []() {
            auto &cc = damageCounter();
            if (cc.perWindow.empty()) {
                return;
            }
            QStringList parts;
            uint32_t total = 0;
            for (auto it = cc.perWindow.begin(); it != cc.perWindow.end(); ++it) {
                if (it.value().first == 0) {
                    continue;
                }
                total += it.value().first;
                parts.append(QStringLiteral("%1=%2").arg(it.value().second.left(40)).arg(it.value().first));
            }
            if (total == 0) {
                return;
            }
            std::sort(parts.begin(), parts.end(), [](const QString &a, const QString &b) {
                return a.section(QLatin1Char('='), -1).toUInt() > b.section(QLatin1Char('='), -1).toUInt();
            });
            qCWarning(KWIN_SCRIPTING).noquote()
                << "WindowThumbnailSource damage rate (per second): total=" << total
                << " breakdown=" << parts.mid(0, 10).join(QLatin1Char(';'));
            for (auto it = cc.perWindow.begin(); it != cc.perWindow.end(); ++it) {
                it.value().first = 0;
            }
        });
        c.flushTimer->start();
    }
    auto &entry = c.perWindow[handle];
    entry.first++;
    // Refresh caption each tick so renamed windows show their current title.
    entry.second = handle->caption();
}
static void removeDamageEntry(Window *handle)
{
    if (!s_thumbnailDamageDiagnostic) {
        return;
    }
    damageCounter().perWindow.remove(handle);
}
}

static bool useGlThumbnails()
{
    static bool qtQuickIsSoftware = QStringList({QStringLiteral("software"), QStringLiteral("softwarecontext")}).contains(QQuickWindow::sceneGraphBackend());
    return Compositor::self()->backend() && Compositor::self()->backend()->compositingType() == OpenGLCompositing && !qtQuickIsSoftware;
}

static bool useVulkanThumbnails()
{
#if HAVE_VULKAN
    return Compositor::self()->backend() && Compositor::self()->backend()->compositingType() == VulkanCompositing;
#else
    return false;
#endif
}

WindowThumbnailSource::WindowThumbnailSource(QQuickWindow *view, Window *handle)
    : m_view(view)
    , m_handle(handle)
{
    connect(handle, &Window::frameGeometryChanged, this, [this]() {
        m_dirty = true;
        Q_EMIT changed();
    });
    connect(handle, &Window::damaged, this, [this]() {
        recordDamage(m_handle);
        m_dirty = true;
        // Deliberately do not emit changed() here. update() (driven by
        // WorkspaceScene::preFrameRender every frame) will emit it after
        // the texture has actually been re-rendered. Eliminates the
        // duplicate dirty notification per damage event — every damage
        // used to fire two QML "item update" hops (once from this slot,
        // once from update()'s post-render emit), each propagating up
        // the QML tree to any layer-enabled ancestor and forcing the
        // tile FBO to dirty twice.
    });

    connect(Compositor::self()->scene(), &WorkspaceScene::preFrameRender, this, &WindowThumbnailSource::update);

    m_handle->refOffscreenRendering();
}

WindowThumbnailSource::~WindowThumbnailSource()
{

    if (m_handle) {
        removeDamageEntry(m_handle);
        m_handle->unrefOffscreenRendering();
    }

    if (!m_offscreenTexture) {
        return;
    }
    if (!QOpenGLContext::currentContext()) {
        Compositor::self()->scene()->makeOpenGLContextCurrent();
    }
    m_offscreenTarget.reset();
    m_offscreenTexture.reset();

    if (m_acquireFence) {
        glDeleteSync(m_acquireFence);
        m_acquireFence = 0;
    }
}

std::shared_ptr<WindowThumbnailSource> WindowThumbnailSource::getOrCreate(QQuickWindow *window, Window *handle)
{
    using WindowThumbnailSourceKey = std::pair<QQuickWindow *, Window *>;
    const WindowThumbnailSourceKey key{window, handle};

    static std::map<WindowThumbnailSourceKey, std::weak_ptr<WindowThumbnailSource>> sources;
    auto &source = sources[key];
    if (!source.expired()) {
        return source.lock();
    }

    auto s = std::make_shared<WindowThumbnailSource>(window, handle);
    source = s;

    QObject::connect(handle, &Window::destroyed, [key]() {
        sources.erase(key);
    });
    QObject::connect(window, &QQuickWindow::destroyed, [key]() {
        sources.erase(key);
    });
    return s;
}

WindowThumbnailSource::Frame WindowThumbnailSource::acquire()
{
    return Frame{
        .texture = m_offscreenTexture,
        .fence = std::exchange(m_acquireFence, nullptr),
    };
}

void WindowThumbnailSource::update()
{
    if (m_acquireFence || !m_dirty || !m_handle) {
        return;
    }
    Q_ASSERT(m_view);

#if HAVE_VULKAN
    if (useVulkanThumbnails()) {
        updateVulkan();
        return;
    }
#endif

    const QRectF geometry = m_handle->visibleGeometry();
    const qreal devicePixelRatio = m_view->devicePixelRatio();
    const QSize textureSize = geometry.toAlignedRect().size() * devicePixelRatio;

    if (!m_offscreenTexture || m_offscreenTexture->size() != textureSize) {
        m_offscreenTexture = GLTexture::allocate(GL_RGBA8, textureSize);
        if (!m_offscreenTexture) {
            return;
        }
        m_offscreenTexture->setContentTransform(OutputTransform::FlipY);
        m_offscreenTexture->setFilter(GL_LINEAR);
        m_offscreenTexture->setWrapMode(GL_CLAMP_TO_EDGE);
        m_offscreenTarget = std::make_unique<GLFramebuffer>(m_offscreenTexture.get());
    }

    RenderTarget offscreenRenderTarget(m_offscreenTarget.get());
    RenderViewport offscreenViewport(geometry, devicePixelRatio, offscreenRenderTarget);
    GLFramebuffer::pushFramebuffer(m_offscreenTarget.get());
    glClearColor(0.0, 0.0, 0.0, 0.0);
    glClear(GL_COLOR_BUFFER_BIT);

    // The thumbnail must be rendered using kwin's opengl context as VAOs are not
    // shared across contexts. Unfortunately, this also introduces a latency of 1
    // frame, which is not ideal, but it is acceptable for things such as thumbnails.
    const int mask = Scene::PAINT_WINDOW_TRANSFORMED;
    Compositor::self()->scene()->renderer()->renderItem(offscreenRenderTarget, offscreenViewport, m_handle->windowItem(), mask, infiniteRegion(), WindowPaintData{});
    GLFramebuffer::popFramebuffer();

    // The fence is needed to avoid the case where qtquick renderer starts using
    // the texture while all rendering commands to it haven't completed yet.
    m_dirty = false;
    m_acquireFence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

    Q_EMIT changed();
}

QImage WindowThumbnailSource::acquireImage() const
{
#if HAVE_VULKAN
    return m_cachedImage;
#else
    return QImage();
#endif
}

#if HAVE_VULKAN
void WindowThumbnailSource::updateVulkan()
{
    Q_ASSERT(m_view);

    auto *vkScene = dynamic_cast<WorkspaceSceneVulkan *>(Compositor::self()->scene());
    if (!vkScene) {
        qCWarning(KWIN_SCRIPTING) << "VulkanThumbnail: no WorkspaceSceneVulkan";
        return;
    }
    auto *vkRenderer = static_cast<ItemRendererVulkan *>(vkScene->renderer());
    if (!vkRenderer) {
        return;
    }
    VulkanContext *ctx = vkScene->backend()->vulkanContext();
    if (!ctx) {
        return;
    }

    // Render the full WindowItem (surface + decoration children) into an offscreen
    // framebuffer, matching the GL path. Sampling the surface alone would drop the
    // decoration; that's the regression overview surfaced when window thumbnails
    // appeared without titlebars or borders.
    const QRectF geometry = m_handle->visibleGeometry();
    const qreal devicePixelRatio = m_view->devicePixelRatio();
    const QSize textureSize = (geometry.toAlignedRect().size() * devicePixelRatio).expandedTo(QSize(1, 1));

    // RGBA_SRGB so QSGVulkanTexture::fromNative samples the channels in the
    // order Qt's RHI expects (RGBA), avoiding the R/B swap that forced the
    // old code through a CPU readback + reupload. The sRGB encoding matches
    // the swapchain's typical BGRA_SRGB so blending math stays linear and
    // the visual result is identical.
    constexpr VkFormat kThumbnailFormat = VK_FORMAT_R8G8B8A8_SRGB;
    if (!m_vkRenderPass) {
        m_vkRenderPass = VulkanRenderPass::createForOffscreen(ctx, kThumbnailFormat, /*withDepth=*/false);
        if (!m_vkRenderPass || !m_vkRenderPass->isValid()) {
            qCWarning(KWIN_SCRIPTING) << "VulkanThumbnail: failed to create offscreen render pass";
            m_vkRenderPass.reset();
            return;
        }
    }
    if (!m_vkOffscreenFbo || m_vkOffscreenFbo->size() != textureSize) {
        // A size change retires the texture QtQuick has cached — the next
        // updatePaintNode() will rebuild the QSGTexture against the new
        // VkImage. The submit handle from any prior frame is invalidated
        // here too; nothing references the old texture anymore.
        m_vkOffscreenFbo = VulkanFramebuffer::createWithTexture(ctx, m_vkRenderPass.get(), textureSize, kThumbnailFormat);
        if (!m_vkOffscreenFbo || !m_vkOffscreenFbo->isValid()) {
            qCWarning(KWIN_SCRIPTING) << "VulkanThumbnail: failed to create offscreen framebuffer" << textureSize;
            m_vkOffscreenFbo.reset();
            return;
        }
        m_vkSubmitHandle = VulkanSubmitHandle{};
    }
    m_vkLastDpr = devicePixelRatio;

    // If a previous submit is still in flight, wait for it before recording
    // a new one that will write the same texture. In steady state this is a
    // no-op (the fence will already be signaled by the next time we get
    // here), but it guarantees we never alias GPU writes to the texture
    // QtQuick may also be sampling.
    if (m_vkSubmitHandle.isValid()) {
        ctx->waitForSubmit(m_vkSubmitHandle);
        m_vkSubmitHandle = VulkanSubmitHandle{};
    }

    VkCommandBuffer cmd = ctx->beginSingleTimeCommands();
    if (cmd == VK_NULL_HANDLE) {
        return;
    }

    auto vkRT = std::make_unique<VulkanRenderTarget>(m_vkOffscreenFbo.get());
    vkRT->setCommandBuffer(cmd);
    RenderTarget offscreenRT(vkRT.get());
    RenderViewport viewport(geometry, devicePixelRatio, offscreenRT);

    VkClearValue clearVal{};
    VkRect2D area{{0, 0}, {uint32_t(textureSize.width()), uint32_t(textureSize.height())}};
    m_vkRenderPass->begin(cmd, m_vkOffscreenFbo->framebuffer(), area, &clearVal, 1);

    // Y-flipped viewport (VK_KHR_maintenance1) matches the main render path and the
    // screenshot effect, so the resulting framebuffer layout is canonical top-down.
    VkViewport vp{};
    vp.x = 0.0f;
    vp.y = float(textureSize.height());
    vp.width = float(textureSize.width());
    vp.height = -float(textureSize.height());
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D scissor{{0, 0}, {uint32_t(textureSize.width()), uint32_t(textureSize.height())}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Save/restore vertex buffer offset: renderItem() advances the shared
    // streaming buffer. With async submission the wait above (or, in steady
    // state, the previous submission's fence already signaling) ensures the
    // GPU is done with the offset region before we reuse it.
    const size_t savedOffset = vkRenderer->vertexBufferOffset();
    vkRenderer->renderItem(offscreenRT, viewport, m_handle->windowItem(),
                           Scene::PAINT_WINDOW_TRANSFORMED, infiniteRegion(), WindowPaintData{});
    vkRenderer->setVertexBufferOffset(savedOffset);

    m_vkRenderPass->end(cmd);

    // Transition to SHADER_READ_ONLY_OPTIMAL so readTextureToImage can copy
    // the texture out (it expects that as the starting layout).
    m_vkOffscreenFbo->colorTexture()->transitionLayout(cmd,
                                                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    // Submit the render and wait scoped to its fence (Phase 0 shim), then
    // read back into m_cachedImage. QtQuick's GL RHI consumes this via
    // createTextureFromImage in updatePaintNode(); see the header for why
    // we can't hand it the VkImage directly.
    m_vkSubmitHandle = ctx->submitSingleTimeCommandsAsync(cmd);
    if (m_vkSubmitHandle.isValid()) {
        ctx->waitForSubmit(m_vkSubmitHandle);
    }
    m_cachedImage = ctx->readTextureToImage(m_vkOffscreenFbo->colorTexture());
    m_cachedImage.setDevicePixelRatio(devicePixelRatio);

    m_dirty = false;
    Q_EMIT changed();
}
#endif

class ThumbnailTextureProvider : public QSGTextureProvider
{
public:
    explicit ThumbnailTextureProvider(QQuickWindow *window);

    QSGTexture *texture() const override;
    void setTexture(const std::shared_ptr<GLTexture> &nativeTexture);
    void setTexture(QSGTexture *texture);

private:
    QQuickWindow *m_window;
    std::shared_ptr<GLTexture> m_nativeTexture;
    std::unique_ptr<QSGTexture> m_texture;
};

ThumbnailTextureProvider::ThumbnailTextureProvider(QQuickWindow *window)
    : m_window(window)
{
}

QSGTexture *ThumbnailTextureProvider::texture() const
{
    return m_texture.get();
}

void ThumbnailTextureProvider::setTexture(const std::shared_ptr<GLTexture> &nativeTexture)
{
    if (m_nativeTexture != nativeTexture) {
        const GLuint textureId = nativeTexture->texture();
        m_nativeTexture = nativeTexture;
        m_texture.reset(QNativeInterface::QSGOpenGLTexture::fromNative(textureId, m_window,
                                                                       nativeTexture->size(),
                                                                       QQuickWindow::TextureHasAlphaChannel));
        m_texture->setFiltering(QSGTexture::Linear);
        m_texture->setHorizontalWrapMode(QSGTexture::ClampToEdge);
        m_texture->setVerticalWrapMode(QSGTexture::ClampToEdge);
    }

    // The textureChanged signal must be emitted also if only texture data changes.
    Q_EMIT textureChanged();
}

void ThumbnailTextureProvider::setTexture(QSGTexture *texture)
{
    m_nativeTexture = nullptr;
    m_texture.reset(texture);
    Q_EMIT textureChanged();
}

class ThumbnailTextureProviderCleanupJob : public QRunnable
{
public:
    explicit ThumbnailTextureProviderCleanupJob(ThumbnailTextureProvider *provider)
        : m_provider(provider)
    {
    }

    void run() override
    {
        m_provider.reset();
    }

private:
    std::unique_ptr<ThumbnailTextureProvider> m_provider;
};

WindowThumbnailItem::WindowThumbnailItem(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents);

    connect(Compositor::self(), &Compositor::aboutToToggleCompositing,
            this, &WindowThumbnailItem::resetSource);
    connect(Compositor::self(), &Compositor::compositingToggled,
            this, &WindowThumbnailItem::updateSource);
}

WindowThumbnailItem::~WindowThumbnailItem()
{
    if (m_provider) {
        if (window()) {
            window()->scheduleRenderJob(new ThumbnailTextureProviderCleanupJob(m_provider),
                                        QQuickWindow::AfterSynchronizingStage);
        } else {
            qCCritical(KWIN_SCRIPTING) << "Can't destroy thumbnail texture provider because window is null";
        }
    }
}

void WindowThumbnailItem::releaseResources()
{
    if (m_provider) {
        window()->scheduleRenderJob(new ThumbnailTextureProviderCleanupJob(m_provider),
                                    QQuickWindow::AfterSynchronizingStage);
        m_provider = nullptr;
    }
}

void WindowThumbnailItem::itemChange(QQuickItem::ItemChange change, const QQuickItem::ItemChangeData &value)
{
    if (change == QQuickItem::ItemSceneChange) {
        updateSource();
    }
    QQuickItem::itemChange(change, value);
}

bool WindowThumbnailItem::isTextureProvider() const
{
    return true;
}

QSGTextureProvider *WindowThumbnailItem::textureProvider() const
{
    if (QQuickItem::isTextureProvider()) {
        return QQuickItem::textureProvider();
    }
    if (!m_provider) {
        m_provider = new ThumbnailTextureProvider(window());
    }
    return m_provider;
}

void WindowThumbnailItem::resetSource()
{
    m_source.reset();
}

void WindowThumbnailItem::updateSource()
{
    const bool gl = useGlThumbnails();
    const bool vulkan = useVulkanThumbnails();
    qCDebug(KWIN_SCRIPTING) << "VulkanThumbnail: updateSource gl=" << gl << "vulkan=" << vulkan << "window=" << (window() != nullptr) << "client=" << (m_client != nullptr);
    if ((gl || vulkan) && window() && m_client) {
        m_source = WindowThumbnailSource::getOrCreate(window(), m_client);
        connect(m_source.get(), &WindowThumbnailSource::changed, this, &WindowThumbnailItem::update);
    } else {
        m_source.reset();
    }
}

QSGNode *WindowThumbnailItem::updatePaintNode(QSGNode *oldNode, QQuickItem::UpdatePaintNodeData *)
{
    if (Compositor::compositing()) {
        if (!m_source) {
            return oldNode;
        }

#if HAVE_VULKAN
        if (useVulkanThumbnails()) {
            const QImage img = m_source->acquireImage();
            if (img.isNull()) {
                // First update hasn't run yet or rendering failed; reuse
                // whatever node is already on screen.
                return oldNode;
            }
            if (!m_provider) {
                m_provider = new ThumbnailTextureProvider(window());
            }
            // Qt RHI is OpenGL on this codebase (see compositor_x11.cpp's
            // setGraphicsApi(OpenGL) under the Vulkan compositor), so the
            // upload must go through Qt's GL backend. createTextureFromImage
            // wraps that. A previous attempt to hand Qt a VkImage directly
            // via QSGVulkanTexture::fromNative produced wrong-sized nodes
            // because Qt's GL RHI couldn't interpret the Vulkan handle.
            m_provider->setTexture(window()->createTextureFromImage(img));
        } else
#endif
        {
            auto [texture, acquireFence] = m_source->acquire();
            if (!texture) {
                return oldNode;
            }

            // Wait for rendering commands to the offscreen texture complete if there are any.
            if (acquireFence) {
                glWaitSync(acquireFence, 0, GL_TIMEOUT_IGNORED);
                glDeleteSync(acquireFence);
            }

            if (!m_provider) {
                m_provider = new ThumbnailTextureProvider(window());
            }
            m_provider->setTexture(texture);
        }
    } else {
        if (!m_provider) {
            m_provider = new ThumbnailTextureProvider(window());
        }

        const QImage placeholderImage = fallbackImage();
        m_provider->setTexture(window()->createTextureFromImage(placeholderImage));
    }

    QSGImageNode *node = static_cast<QSGImageNode *>(oldNode);
    if (!node) {
        node = window()->createImageNode();
        node->setFiltering(QSGTexture::Linear);
    }
    node->setTexture(m_provider->texture());
    node->setTextureCoordinatesTransform(QSGImageNode::NoTransform);
    node->setRect(paintedRect());

    return node;
}

QUuid WindowThumbnailItem::wId() const
{
    return m_wId;
}

void WindowThumbnailItem::setWId(const QUuid &wId)
{
    if (m_wId == wId) {
        return;
    }
    m_wId = wId;
    if (!m_wId.isNull()) {
        setClient(workspace()->findWindow(wId));
    } else if (m_client) {
        m_client = nullptr;
        updateSource();
        updateImplicitSize();
        Q_EMIT clientChanged();
    }

    Q_EMIT wIdChanged();
}

Window *WindowThumbnailItem::client() const
{
    return m_client;
}

void WindowThumbnailItem::setClient(Window *client)
{
    if (m_client == client) {
        return;
    }
    if (m_client) {
        disconnect(m_client, &Window::frameGeometryChanged,
                   this, &WindowThumbnailItem::updateImplicitSize);
    }
    m_client = client;
    if (m_client) {
        connect(m_client, &Window::frameGeometryChanged,
                this, &WindowThumbnailItem::updateImplicitSize);
        setWId(m_client->internalId());
    } else {
        setWId(QUuid());
    }
    updateSource();
    updateImplicitSize();
    Q_EMIT clientChanged();
}

void WindowThumbnailItem::updateImplicitSize()
{
    QSize frameSize;
    if (m_client) {
        frameSize = m_client->frameGeometry().toAlignedRect().size();
    }
    setImplicitSize(frameSize.width(), frameSize.height());
}

QImage WindowThumbnailItem::fallbackImage() const
{
    if (m_client) {
        return m_client->icon().pixmap(boundingRect().size().toSize(), window()->devicePixelRatio()).toImage();
    }
    return QImage();
}

static QRectF centeredSize(const QRectF &boundingRect, const QSizeF &size)
{
    const QSizeF scaled = size.scaled(boundingRect.size(), Qt::KeepAspectRatio);
    const qreal x = boundingRect.x() + (boundingRect.width() - scaled.width()) / 2;
    const qreal y = boundingRect.y() + (boundingRect.height() - scaled.height()) / 2;
    return QRectF(QPointF(x, y), scaled);
}

QRectF WindowThumbnailItem::paintedRect() const
{
    if (!m_client) {
        return QRectF();
    }
    if (!Compositor::compositing()) {
        const QSizeF iconSize = m_client->icon().actualSize(boundingRect().size().toSize());
        return centeredSize(boundingRect(), iconSize);
    }

    const QRectF visibleGeometry = m_client->visibleGeometry();
    const QRectF frameGeometry = m_client->frameGeometry();
    const QSizeF scaled = QSizeF(frameGeometry.size()).scaled(boundingRect().size(), Qt::KeepAspectRatio);

    const qreal xScale = scaled.width() / frameGeometry.width();
    const qreal yScale = scaled.height() / frameGeometry.height();

    QRectF paintedRect(boundingRect().x() + (boundingRect().width() - scaled.width()) / 2,
                       boundingRect().y() + (boundingRect().height() - scaled.height()) / 2,
                       visibleGeometry.width() * xScale,
                       visibleGeometry.height() * yScale);

    paintedRect.moveLeft(paintedRect.x() + (visibleGeometry.x() - frameGeometry.x()) * xScale);
    paintedRect.moveTop(paintedRect.y() + (visibleGeometry.y() - frameGeometry.y()) * yScale);

    return paintedRect;
}

} // namespace KWin

#include "moc_windowthumbnailitem.cpp"
