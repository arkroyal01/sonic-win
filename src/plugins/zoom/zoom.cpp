/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2006 Lubos Lunak <l.lunak@kde.org>
    SPDX-FileCopyrightText: 2010 Sebastian Sauer <sebsauer@kdab.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "zoom.h"
#include "config-kwin.h"
#include "core/rendertarget.h"
#include "core/renderviewport.h"
#include "effect/effecthandler.h"
#include "opengl/glutils.h"
#include "zoomconfig.h"

#if HAVE_ACCESSIBILITY
#include "accessibilityintegration.h"
#endif

#if HAVE_VULKAN
#include "compositor.h"
#include "platformsupport/scenes/vulkan/vulkanbackend.h"
#include "platformsupport/scenes/vulkan/vulkanbuffer.h"
#include "platformsupport/scenes/vulkan/vulkancontext.h"
#include "platformsupport/scenes/vulkan/vulkanframebuffer.h"
#include "platformsupport/scenes/vulkan/vulkanpipeline.h"
#include "platformsupport/scenes/vulkan/vulkanpipelinemanager.h"
#include "platformsupport/scenes/vulkan/vulkanrenderpass.h"
#include "platformsupport/scenes/vulkan/vulkanrendertarget.h"
#include "platformsupport/scenes/vulkan/vulkantexture.h"
#include "scene/itemrenderer_vulkan.h"
#include "scene/workspacescene_vulkan.h"
#include <array>
#include <cmath>
#include <cstring>
#endif

#include <KConfigGroup>
#include <KGlobalAccel>
#include <KLocalizedString>
#include <KStandardActions>

#include <QAction>

using namespace std::chrono_literals;

static void ensureResources()
{
    // Must initialize resources manually because the effect is a static lib.
    Q_INIT_RESOURCE(zoom);
}

namespace KWin
{

ZoomEffect::ZoomEffect()
{
    ensureResources();

    ZoomConfig::instance(effects->config());
    QAction *a = nullptr;
    a = KStandardActions::zoomIn(this, &ZoomEffect::zoomIn, this);
    KGlobalAccel::self()->setDefaultShortcut(a, QList<QKeySequence>() << (Qt::META | Qt::Key_Plus) << (Qt::META | Qt::Key_Equal));
    KGlobalAccel::self()->setShortcut(a, QList<QKeySequence>() << (Qt::META | Qt::Key_Plus) << (Qt::META | Qt::Key_Equal));
    effects->registerAxisShortcut(Qt::ControlModifier | Qt::MetaModifier, PointerAxisUp, a);

    a = KStandardActions::zoomOut(this, &ZoomEffect::zoomOut, this);
    KGlobalAccel::self()->setDefaultShortcut(a, QList<QKeySequence>() << (Qt::META | Qt::Key_Minus));
    KGlobalAccel::self()->setShortcut(a, QList<QKeySequence>() << (Qt::META | Qt::Key_Minus));
    effects->registerAxisShortcut(Qt::ControlModifier | Qt::MetaModifier, PointerAxisDown, a);

    a = KStandardActions::actualSize(this, &ZoomEffect::actualSize, this);
    KGlobalAccel::self()->setDefaultShortcut(a, QList<QKeySequence>() << (Qt::META | Qt::Key_0));
    KGlobalAccel::self()->setShortcut(a, QList<QKeySequence>() << (Qt::META | Qt::Key_0));

    a = new QAction(this);
    a->setObjectName(QStringLiteral("MoveZoomLeft"));
    a->setText(i18n("Move Zoomed Area to Left"));
    KGlobalAccel::self()->setDefaultShortcut(a, QList<QKeySequence>());
    KGlobalAccel::self()->setShortcut(a, QList<QKeySequence>());
    connect(a, &QAction::triggered, this, &ZoomEffect::moveZoomLeft);

    a = new QAction(this);
    a->setObjectName(QStringLiteral("MoveZoomRight"));
    a->setText(i18n("Move Zoomed Area to Right"));
    KGlobalAccel::self()->setDefaultShortcut(a, QList<QKeySequence>());
    KGlobalAccel::self()->setShortcut(a, QList<QKeySequence>());
    connect(a, &QAction::triggered, this, &ZoomEffect::moveZoomRight);

    a = new QAction(this);
    a->setObjectName(QStringLiteral("MoveZoomUp"));
    a->setText(i18n("Move Zoomed Area Upwards"));
    KGlobalAccel::self()->setDefaultShortcut(a, QList<QKeySequence>());
    KGlobalAccel::self()->setShortcut(a, QList<QKeySequence>());
    connect(a, &QAction::triggered, this, &ZoomEffect::moveZoomUp);

    a = new QAction(this);
    a->setObjectName(QStringLiteral("MoveZoomDown"));
    a->setText(i18n("Move Zoomed Area Downwards"));
    KGlobalAccel::self()->setDefaultShortcut(a, QList<QKeySequence>());
    KGlobalAccel::self()->setShortcut(a, QList<QKeySequence>());
    connect(a, &QAction::triggered, this, &ZoomEffect::moveZoomDown);

    // TODO: these two actions don't belong into the effect. They need to be moved into KWin core
    a = new QAction(this);
    a->setObjectName(QStringLiteral("MoveMouseToFocus"));
    a->setText(i18n("Move Mouse to Focus"));
    KGlobalAccel::self()->setDefaultShortcut(a, QList<QKeySequence>() << (Qt::META | Qt::Key_F5));
    KGlobalAccel::self()->setShortcut(a, QList<QKeySequence>() << (Qt::META | Qt::Key_F5));
    connect(a, &QAction::triggered, this, &ZoomEffect::moveMouseToFocus);

    a = new QAction(this);
    a->setObjectName(QStringLiteral("MoveMouseToCenter"));
    a->setText(i18n("Move Mouse to Center"));
    KGlobalAccel::self()->setDefaultShortcut(a, QList<QKeySequence>() << (Qt::META | Qt::Key_F6));
    KGlobalAccel::self()->setShortcut(a, QList<QKeySequence>() << (Qt::META | Qt::Key_F6));
    connect(a, &QAction::triggered, this, &ZoomEffect::moveMouseToCenter);

    m_timeline.setDuration(350);
    m_timeline.setFrameRange(0, 100);
    connect(&m_timeline, &QTimeLine::frameChanged, this, &ZoomEffect::timelineFrameChanged);
    connect(effects, &EffectsHandler::windowAdded, this, &ZoomEffect::slotWindowAdded);
    connect(effects, &EffectsHandler::screenRemoved, this, &ZoomEffect::slotScreenRemoved);

#if HAVE_VULKAN
    // Vulkan resources (command buffers, semaphores) must be released before the
    // context goes away. Effects can outlive the compositor object in the destruction
    // order, so tear down explicitly when the compositor announces its demise.
    if (Compositor *c = Compositor::self()) {
        connect(c, &Compositor::aboutToDestroy, this, [this]() {
            m_vkOffscreenData.clear();
            m_vkCursorTexture.reset();
            m_vkCursorStaging.reset();
            m_vkCursorImageCacheKey = 0;
        });
    }
#endif

#if HAVE_ACCESSIBILITY
    m_accessibilityIntegration = new ZoomAccessibilityIntegration(this);
    connect(m_accessibilityIntegration, &ZoomAccessibilityIntegration::focusPointChanged, this, &ZoomEffect::moveFocus);
#endif

    const auto windows = effects->stackingOrder();
    for (EffectWindow *w : windows) {
        slotWindowAdded(w);
    }

    reconfigure(ReconfigureAll);

    const double initialZoom = ZoomConfig::initialZoom();
    if (initialZoom > 1.0) {
        zoomTo(initialZoom);
    }
}

ZoomEffect::~ZoomEffect()
{
    // switch off and free resources
    showCursor();
    // Save the zoom value.
    ZoomConfig::setInitialZoom(m_targetZoom);
    ZoomConfig::self()->save();
}

bool ZoomEffect::isFocusTrackingEnabled() const
{
#if HAVE_ACCESSIBILITY
    return m_accessibilityIntegration && m_accessibilityIntegration->isFocusTrackingEnabled();
#else
    return false;
#endif
}

bool ZoomEffect::isTextCaretTrackingEnabled() const
{
#if HAVE_ACCESSIBILITY
    return m_accessibilityIntegration && m_accessibilityIntegration->isTextCaretTrackingEnabled();
#else
    return false;
#endif
}

GLTexture *ZoomEffect::ensureCursorTexture()
{
    if (!m_cursorTexture || m_cursorTextureDirty) {
        m_cursorTexture.reset();
        m_cursorTextureDirty = false;
        const auto cursor = effects->cursorImage();
        if (!cursor.image().isNull()) {
            m_cursorTexture = GLTexture::upload(cursor.image());
            if (!m_cursorTexture) {
                return nullptr;
            }
            m_cursorTexture->setWrapMode(GL_CLAMP_TO_EDGE);
        }
    }
    return m_cursorTexture.get();
}

void ZoomEffect::markCursorTextureDirty()
{
    m_cursorTextureDirty = true;
}

void ZoomEffect::showCursor()
{
    if (m_isMouseHidden) {
        disconnect(effects, &EffectsHandler::cursorShapeChanged, this, &ZoomEffect::markCursorTextureDirty);
        // show the previously hidden mouse-pointer again and free the loaded texture/picture.
        effects->showCursor();
        m_cursorTexture.reset();
#if HAVE_VULKAN
        m_vkCursorTexture.reset();
        m_vkCursorStaging.reset();
        m_vkCursorImageCacheKey = 0;
#endif
        m_isMouseHidden = false;
    }
}

void ZoomEffect::hideCursor()
{
    if (m_mouseTracking == MouseTrackingProportional && m_mousePointer == MousePointerKeep) {
        return; // don't replace the actual cursor by a static image for no reason.
    }
    if (!m_isMouseHidden) {
        // try to load the cursor-theme into a OpenGL texture and if successful then hide the mouse-pointer
        bool haveTexture = false;
        if (effects->isOpenGLCompositing()) {
            haveTexture = ensureCursorTexture() != nullptr;
        }
#if HAVE_VULKAN
        else if (effects->isVulkanCompositing()) {
            // The Vulkan cursor texture is uploaded lazily in paintScreenVulkan,
            // but the OS cursor needs to be hidden up-front to avoid a double cursor.
            haveTexture = !effects->cursorImage().image().isNull();
        }
#endif
        if (haveTexture) {
            effects->hideCursor();
            connect(effects, &EffectsHandler::cursorShapeChanged, this, &ZoomEffect::markCursorTextureDirty);
            m_isMouseHidden = true;
        }
    }
}

void ZoomEffect::reconfigure(ReconfigureFlags)
{
    ZoomConfig::self()->read();
    // On zoom-in and zoom-out change the zoom by the defined zoom-factor.
    m_zoomFactor = std::max(0.1, ZoomConfig::zoomFactor());
    m_pixelGridZoom = ZoomConfig::pixelGridZoom();
    // Visibility of the mouse-pointer.
    m_mousePointer = MousePointerType(ZoomConfig::mousePointer());
    // Track moving of the mouse.
    m_mouseTracking = MouseTrackingType(ZoomConfig::mouseTracking());
#if HAVE_ACCESSIBILITY
    if (m_accessibilityIntegration) {
        // Enable tracking of the focused location.
        m_accessibilityIntegration->setFocusTrackingEnabled(ZoomConfig::enableFocusTracking());
        // Enable tracking of the text caret.
        m_accessibilityIntegration->setTextCaretTrackingEnabled(ZoomConfig::enableTextCaretTracking());
    }
#endif
    // The time in milliseconds to wait before a focus-event takes away a mouse-move.
    m_focusDelay = std::max(uint(0), ZoomConfig::focusDelay());
    // The factor the zoom-area will be moved on touching an edge on push-mode or using the navigation KAction's.
    m_moveFactor = std::max(0.1, ZoomConfig::moveFactor());
}

void ZoomEffect::prePaintScreen(ScreenPrePaintData &data, std::chrono::milliseconds presentTime)
{
    data.mask |= PAINT_SCREEN_TRANSFORMED;
    if (m_zoom != m_targetZoom) {
        int time = 0;
        if (m_lastPresentTime.count()) {
            time = (presentTime - m_lastPresentTime).count();
        }
        m_lastPresentTime = presentTime;

        const float zoomDist = std::abs(m_targetZoom - m_sourceZoom);
        if (m_targetZoom > m_zoom) {
            m_zoom = std::min(m_zoom + ((zoomDist * time) / animationTime(std::chrono::milliseconds(int(150 * m_zoomFactor)))), m_targetZoom);
        } else {
            m_zoom = std::max(m_zoom - ((zoomDist * time) / animationTime(std::chrono::milliseconds(int(150 * m_zoomFactor)))), m_targetZoom);
        }
    }

    if (m_zoom == 1.0) {
        showCursor();
    } else {
        hideCursor();
    }

    effects->prePaintScreen(data, presentTime);
}

ZoomEffect::OffscreenData *ZoomEffect::ensureOffscreenData(const RenderTarget &renderTarget, const RenderViewport &viewport, Output *screen)
{
    const QSize nativeSize = renderTarget.size();

    OffscreenData &data = m_offscreenData[nullptr];
    data.viewport = viewport.renderRect();
    data.color = renderTarget.colorDescription();

    const GLenum textureFormat = renderTarget.colorDescription() == ColorDescription::sRGB ? GL_RGBA8 : GL_RGBA16F;
    if (!data.texture || data.texture->size() != nativeSize || data.texture->internalFormat() != textureFormat) {
        data.texture = GLTexture::allocate(textureFormat, nativeSize);
        if (!data.texture) {
            return nullptr;
        }
        data.texture->setFilter(GL_LINEAR);
        data.texture->setWrapMode(GL_CLAMP_TO_EDGE);
        data.framebuffer = std::make_unique<GLFramebuffer>(data.texture.get());
    }

    data.texture->setContentTransform(renderTarget.transform());
    return &data;
}

GLShader *ZoomEffect::shaderForZoom(double zoom)
{
    if (zoom < m_pixelGridZoom) {
        return GLShaderManager::instance()->shader(GLShaderTrait::MapTexture | GLShaderTrait::TransformColorspace);
    } else {
        if (!m_pixelGridShader) {
            m_pixelGridShader = GLShaderManager::instance()->generateShaderFromFile(GLShaderTrait::MapTexture, QString(), QStringLiteral(":/effects/zoom/shaders/pixelgrid.frag"));
        }
        return m_pixelGridShader.get();
    }
}

#if HAVE_VULKAN
ZoomEffect::VulkanOffscreenData::VulkanOffscreenData(VulkanOffscreenData &&other) noexcept
    : framebuffer(std::move(other.framebuffer))
    , renderPass(std::move(other.renderPass))
    , size(other.size)
    , format(other.format)
    , cmd(other.cmd)
    , semaphore(other.semaphore)
    , ctx(other.ctx)
{
    other.cmd = VK_NULL_HANDLE;
    other.semaphore = VK_NULL_HANDLE;
    other.ctx = nullptr;
}

ZoomEffect::VulkanOffscreenData &ZoomEffect::VulkanOffscreenData::operator=(VulkanOffscreenData &&other) noexcept
{
    if (this != &other) {
        this->~VulkanOffscreenData();
        new (this) VulkanOffscreenData(std::move(other));
    }
    return *this;
}

ZoomEffect::VulkanOffscreenData::~VulkanOffscreenData()
{
    if (!ctx) {
        return;
    }
    VkDevice device = ctx->backend()->device();
    if (device == VK_NULL_HANDLE) {
        return;
    }
    if (cmd != VK_NULL_HANDLE) {
        ctx->freeCommandBuffer(cmd);
    }
    if (semaphore != VK_NULL_HANDLE) {
        vkDestroySemaphore(device, semaphore, nullptr);
    }
}

ZoomEffect::VulkanOffscreenData *ZoomEffect::ensureVulkanOffscreenData(const RenderTarget &renderTarget, const RenderViewport &viewport, Output *screen)
{
    auto *scene = dynamic_cast<WorkspaceSceneVulkan *>(Compositor::self()->scene());
    if (!scene) {
        return nullptr;
    }
    VulkanContext *ctx = scene->backend()->vulkanContext();
    if (!ctx) {
        return nullptr;
    }

    const QSize nativeSize = renderTarget.size();
    const VkFormat format = ctx->backend()->colorFormat();

    VulkanOffscreenData &data = m_vkOffscreenData[screen];
    if (!data.framebuffer || data.size != nativeSize || data.format != format) {
        data.framebuffer.reset();
        data.renderPass = VulkanRenderPass::createForOffscreen(ctx, format, false);
        if (!data.renderPass || !data.renderPass->isValid()) {
            return nullptr;
        }
        data.framebuffer = VulkanFramebuffer::createWithTexture(ctx, data.renderPass.get(), nativeSize, format);
        if (!data.framebuffer || !data.framebuffer->isValid()) {
            return nullptr;
        }
        if (VulkanTexture *tex = data.framebuffer->colorTexture()) {
            tex->setFilter(VK_FILTER_LINEAR);
            tex->setWrapMode(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
        }
        data.size = nativeSize;
        data.format = format;
    }
    return &data;
}

bool ZoomEffect::prepareVulkanCursorUpload(VulkanContext *ctx, VkCommandBuffer cmd)
{
    const auto cursor = effects->cursorImage();
    const QImage img = cursor.image();
    if (img.isNull()) {
        m_vkCursorTexture.reset();
        m_vkCursorStaging.reset();
        m_vkCursorImageCacheKey = 0;
        return false;
    }

    if (!m_cursorTextureDirty && m_vkCursorTexture && m_vkCursorImageCacheKey == img.cacheKey()) {
        return false; // current texture is still good
    }

    auto upload = VulkanTexture::uploadAsync(ctx, img, cmd);
    if (!upload.texture || !upload.texture->isValid()) {
        return false;
    }
    upload.texture->setFilter(VK_FILTER_LINEAR);
    upload.texture->setWrapMode(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);

    // The old staging buffer (if any) was used by a previous frame's cmd2 which has
    // since completed. Replacing it queues the old one for deferred destruction,
    // which fires after the next beginFrame's fence wait — safe.
    m_vkCursorTexture = std::move(upload.texture);
    m_vkCursorStaging = std::move(upload.staging);
    m_vkCursorImageCacheKey = img.cacheKey();
    m_cursorTextureDirty = false;
    return true;
}

void ZoomEffect::paintScreenVulkan(const RenderTarget &renderTarget, const RenderViewport &viewport, int mask, const QRegion &region, Output *screen)
{
    auto *scene = dynamic_cast<WorkspaceSceneVulkan *>(Compositor::self()->scene());
    if (!scene) {
        return;
    }
    auto *renderer = static_cast<ItemRendererVulkan *>(scene->renderer());
    VulkanContext *ctx = renderer->context();
    VkCommandBuffer mainCmd = renderer->currentCommandBuffer();
    if (!ctx || mainCmd == VK_NULL_HANDLE) {
        return;
    }

    VulkanOffscreenData *offscreen = ensureVulkanOffscreenData(renderTarget, viewport, screen);
    if (!offscreen) {
        return;
    }
    VulkanTexture *offscreenTex = offscreen->framebuffer->colorTexture();
    if (!offscreenTex || !offscreenTex->isValid()) {
        return;
    }

    const QSize nativeSize = offscreen->size;
    const qreal scale = viewport.scale();
    const QRectF viewportRect = viewport.renderRect();

    // === 1. Render the scene at native resolution into the offscreen FB. ===
    //
    // Uses a persistent per-output command buffer + binary semaphore. cmd2 is
    // submitted asynchronously; the main command buffer waits on the semaphore
    // at FRAGMENT_SHADER_BIT so the upscale sample can't run until the
    // offscreen render is done — without a CPU vkQueueWaitIdle.
    //
    // doBeginFrame() waits on the previous frame's main inFlightFence before
    // entering beginFrame(), so by the time we get here cmd2's prior
    // submission is also finished (transitive: main waited on its semaphore),
    // making vkResetCommandBuffer safe.
    {
        if (offscreen->cmd == VK_NULL_HANDLE) {
            offscreen->cmd = ctx->allocateCommandBuffer();
            offscreen->ctx = ctx;
            if (offscreen->cmd == VK_NULL_HANDLE) {
                return;
            }
        } else {
            vkResetCommandBuffer(offscreen->cmd, 0);
        }
        if (offscreen->semaphore == VK_NULL_HANDLE) {
            VkSemaphoreCreateInfo si{};
            si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            if (vkCreateSemaphore(ctx->backend()->device(), &si, nullptr, &offscreen->semaphore) != VK_SUCCESS) {
                return;
            }
        }

        VkCommandBuffer cmd2 = offscreen->cmd;
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(cmd2, &begin) != VK_SUCCESS) {
            return;
        }

        // Cursor upload (if the shape changed) must happen before any render pass
        // begins on cmd2 — vkCmdCopyBufferToImage and the layout barriers around it
        // aren't allowed inside a render pass. The main cmd buffer's cursor read
        // waits on offscreen->semaphore at FRAGMENT_SHADER_BIT, so the upload here
        // is visible by the time the main quad samples it.
        if (m_mousePointer != MousePointerHide && m_isMouseHidden) {
            prepareVulkanCursorUpload(ctx, cmd2);
        }

        VulkanRenderTarget vulkanOffscreenRT(offscreen->framebuffer.get(), renderTarget.colorDescription());
        vulkanOffscreenRT.setCommandBuffer(cmd2);
        RenderTarget offscreenRenderTarget(&vulkanOffscreenRT, renderTarget.colorDescription());
        RenderViewport offscreenViewport(viewportRect, scale, offscreenRenderTarget);

        VkClearValue clearValue{};
        clearValue.color.float32[0] = 0.0f;
        clearValue.color.float32[1] = 0.0f;
        clearValue.color.float32[2] = 0.0f;
        clearValue.color.float32[3] = 0.0f;
        VkRect2D renderArea{};
        renderArea.offset = {0, 0};
        renderArea.extent = {static_cast<uint32_t>(nativeSize.width()), static_cast<uint32_t>(nativeSize.height())};
        offscreen->renderPass->begin(cmd2, offscreen->framebuffer->framebuffer(), renderArea, &clearValue, 1);

        // Negative-height viewport for Y-flip, matching ItemRendererVulkan::beginFrame().
        VkViewport vp{};
        vp.x = 0.0f;
        vp.y = static_cast<float>(nativeSize.height());
        vp.width = static_cast<float>(nativeSize.width());
        vp.height = -static_cast<float>(nativeSize.height());
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(cmd2, 0, 1, &vp);
        VkRect2D scissor{renderArea};
        vkCmdSetScissor(cmd2, 0, 1, &scissor);

        // Snapshot the streaming-buffer offset: the recursive paintScreen flows
        // through ItemRendererVulkan::renderItem which advances it. The main
        // command buffer (recorded next) is also issued on the same renderer,
        // so the GPU must not see the offscreen and main draws overlap into
        // the same buffer region. The semaphore wait at FRAGMENT_SHADER_BIT
        // serializes execution between cmd2 and the main submit, so reusing
        // the same offset is safe.
        // The scope flags this nested flow so debug builds catch effects that
        // grab the swapchain command buffer instead of routing via the offscreen
        // renderTarget — see ItemRendererVulkan::currentCommandBuffer().
        const size_t savedVertexOffset = renderer->vertexBufferOffset();
        {
            ItemRendererVulkan::RecursivePaintScope guard;
            effects->paintScreen(offscreenRenderTarget, offscreenViewport, mask, region, screen);
        }
        renderer->setVertexBufferOffset(savedVertexOffset);

        offscreen->renderPass->end(cmd2);

        // The render pass's finalLayout transitions the image to SHADER_READ_ONLY_OPTIMAL.
        offscreenTex->setCurrentLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        if (vkEndCommandBuffer(cmd2) != VK_SUCCESS) {
            return;
        }

        // Async submit: signal offscreen->semaphore; the main submission will
        // wait on it at FRAGMENT_SHADER_BIT.
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd2;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &offscreen->semaphore;
        if (vkQueueSubmit(ctx->backend()->graphicsQueue(), 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) {
            return;
        }
        renderer->addExternalWaitSemaphore(offscreen->semaphore, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }

    // === 2. Compute mouse-tracking translation (same math as OpenGL path). ===
    const QSize screenSize = effects->virtualScreenSize();
    qreal xTranslation = 0;
    qreal yTranslation = 0;
    switch (m_mouseTracking) {
    case MouseTrackingProportional:
        xTranslation = -int(m_cursorPoint.x() * (m_zoom - 1.0));
        yTranslation = -int(m_cursorPoint.y() * (m_zoom - 1.0));
        m_prevPoint = m_cursorPoint;
        break;
    case MouseTrackingCentered:
        m_prevPoint = m_cursorPoint;
        // fall through
    case MouseTrackingDisabled:
        xTranslation = std::min(0, std::max(int(screenSize.width() - screenSize.width() * m_zoom), int(screenSize.width() / 2 - m_prevPoint.x() * m_zoom)));
        yTranslation = std::min(0, std::max(int(screenSize.height() - screenSize.height() * m_zoom), int(screenSize.height() / 2 - m_prevPoint.y() * m_zoom)));
        break;
    case MouseTrackingPush: {
        const int x = m_cursorPoint.x() * m_zoom - m_prevPoint.x() * (m_zoom - 1.0);
        const int y = m_cursorPoint.y() * m_zoom - m_prevPoint.y() * (m_zoom - 1.0);
        const int threshold = 4;
        const QRectF currScreen = effects->screenAt(QPoint(x, y))->geometry();
        const int screenTop = currScreen.top();
        const int screenLeft = currScreen.left();
        const int screenRight = currScreen.right();
        const int screenBottom = currScreen.bottom();
        const int screenCenterX = currScreen.center().x();
        const int screenCenterY = currScreen.center().y();
        const bool adjacentLeft = screenExistsAt(QPoint(screenLeft - 1, screenCenterY));
        const bool adjacentRight = screenExistsAt(QPoint(screenRight + 1, screenCenterY));
        const bool adjacentTop = screenExistsAt(QPoint(screenCenterX, screenTop - 1));
        const bool adjacentBottom = screenExistsAt(QPoint(screenCenterX, screenBottom + 1));
        m_xMove = m_yMove = 0;
        if (x < screenLeft + threshold && !adjacentLeft) {
            m_xMove = (x - threshold - screenLeft) / m_zoom;
        } else if (x > screenRight - threshold && !adjacentRight) {
            m_xMove = (x + threshold - screenRight) / m_zoom;
        }
        if (y < screenTop + threshold && !adjacentTop) {
            m_yMove = (y - threshold - screenTop) / m_zoom;
        } else if (y > screenBottom - threshold && !adjacentBottom) {
            m_yMove = (y + threshold - screenBottom) / m_zoom;
        }
        if (m_xMove) {
            m_prevPoint.setX(m_prevPoint.x() + m_xMove);
        }
        if (m_yMove) {
            m_prevPoint.setY(m_prevPoint.y() + m_yMove);
        }
        xTranslation = -int(m_prevPoint.x() * (m_zoom - 1.0));
        yTranslation = -int(m_prevPoint.y() * (m_zoom - 1.0));
        break;
    }
    }
    if (isFocusTrackingEnabled() || isTextCaretTrackingEnabled()) {
        bool acceptFocus = true;
        if (m_mouseTracking != MouseTrackingDisabled && m_focusDelay > 0) {
            const int msecs = m_lastMouseEvent.msecsTo(m_lastFocusEvent);
            acceptFocus = msecs > m_focusDelay;
        }
        if (acceptFocus) {
            xTranslation = -int(m_focusPoint.x() * (m_zoom - 1.0));
            yTranslation = -int(m_focusPoint.y() * (m_zoom - 1.0));
            m_prevPoint = m_focusPoint;
        }
    }

    // === 3. Draw upscaled offscreen quad on the main command buffer. ===
    auto *pipelineManager = ctx->pipelineManager();
    if (!pipelineManager) {
        return;
    }
    VulkanShaderTraits zoomTraits = VulkanShaderTrait::MapTexture | VulkanShaderTrait::TransformColorspace;
    if (m_zoom >= m_pixelGridZoom) {
        zoomTraits |= VulkanShaderTrait::PixelGrid;
    }
    VulkanPipeline *zoomPipeline = pipelineManager->pipeline(zoomTraits);
    if (!zoomPipeline || !zoomPipeline->isValid()) {
        return;
    }

    const float w = static_cast<float>(nativeSize.width());
    const float h = static_cast<float>(nativeSize.height());
    std::array<VulkanVertex2D, 6> verts = {{
        {{w, 0.0f}, {1.0f, 0.0f}},
        {{0.0f, 0.0f}, {0.0f, 0.0f}},
        {{0.0f, h}, {0.0f, 1.0f}},
        {{0.0f, h}, {0.0f, 1.0f}},
        {{w, h}, {1.0f, 1.0f}},
        {{w, 0.0f}, {1.0f, 0.0f}},
    }};
    auto vertBuf = VulkanBuffer::createStreamingBuffer(ctx, sizeof(verts));
    if (!vertBuf || !vertBuf->isValid()) {
        return;
    }
    vertBuf->upload(verts.data(), sizeof(verts));

    QMatrix4x4 mvp = viewport.projectionMatrix();
    mvp.translate(xTranslation * scale, yTranslation * scale);
    mvp.scale(m_zoom, m_zoom);
    mvp.translate(viewportRect.x() * scale, viewportRect.y() * scale);

    VulkanPushConstants pc{};
    memcpy(pc.mvp, mvp.constData(), sizeof(pc.mvp));
    pc.textureMatrix[0] = pc.textureMatrix[5] = pc.textureMatrix[10] = pc.textureMatrix[15] = 1.0f;

    VulkanUniforms ubo{};
    ubo.uniformColor[0] = ubo.uniformColor[1] = ubo.uniformColor[2] = ubo.uniformColor[3] = 1.0f;
    ubo.opacity = 1.0f;
    ubo.brightness = 1.0f;
    ubo.saturation = 1.0f;
    const ColorDescription srcColor = renderTarget.colorDescription();
    const ColorDescription dstColor = renderTarget.colorDescription();
    QMatrix4x4 colorimetryTransform = srcColor.toOther(dstColor, RenderingIntent::Perceptual);
    memcpy(ubo.colorimetryTransform, colorimetryTransform.data(), sizeof(ubo.colorimetryTransform));
    ubo.sourceTransferFunction = srcColor.transferFunction().type;
    ubo.sourceTransferParams[0] = srcColor.transferFunction().minLuminance;
    ubo.sourceTransferParams[1] = srcColor.transferFunction().maxLuminance - srcColor.transferFunction().minLuminance;
    ubo.sourceReferenceLuminance = srcColor.referenceLuminance();
    ubo.destTransferFunction = dstColor.transferFunction().type;
    ubo.destTransferParams[0] = dstColor.transferFunction().minLuminance;
    ubo.destTransferParams[1] = dstColor.transferFunction().maxLuminance - dstColor.transferFunction().minLuminance;
    ubo.destReferenceLuminance = dstColor.referenceLuminance();
    ubo.maxDestLuminance = dstColor.maxHdrLuminance().value_or(10000.0f);
    ubo.maxTonemappingLuminance = dstColor.referenceLuminance();
    QMatrix4x4 destToLMS = dstColor.containerColorimetry().toLMS();
    QMatrix4x4 lmsToDest = dstColor.containerColorimetry().fromLMS();
    memcpy(ubo.destToLMS, destToLMS.data(), sizeof(ubo.destToLMS));
    memcpy(ubo.lmsToDest, lmsToDest.data(), sizeof(ubo.lmsToDest));
    const auto toXYZ = dstColor.containerColorimetry().toXYZ();
    ubo.primaryBrightness[0] = toXYZ(1, 0);
    ubo.primaryBrightness[1] = toXYZ(1, 1);
    ubo.primaryBrightness[2] = toXYZ(1, 2);

    auto uboBuf = VulkanBuffer::createUniformBuffer(ctx, sizeof(VulkanUniforms));
    if (!uboBuf || !uboBuf->isValid()) {
        return;
    }
    uboBuf->upload(&ubo, sizeof(ubo));

    VkDescriptorSet ds = ctx->allocateDescriptorSet(zoomPipeline->descriptorSetLayout());
    if (ds == VK_NULL_HANDLE) {
        return;
    }
    VkDescriptorImageInfo imgInfo{offscreenTex->sampler(), offscreenTex->imageView(),
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    std::array<VkDescriptorImageInfo, 4> imageInfos{imgInfo, imgInfo, imgInfo, imgInfo};
    VkDescriptorBufferInfo bufInfo{uboBuf->buffer(), 0, sizeof(VulkanUniforms)};
    std::array<VkWriteDescriptorSet, 2> writes{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = ds;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 4;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = imageInfos.data();
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = ds;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[1].pBufferInfo = &bufInfo;
    vkUpdateDescriptorSets(ctx->backend()->device(), writes.size(), writes.data(), 0, nullptr);

    vkCmdBindPipeline(mainCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, zoomPipeline->pipeline());
    vkCmdPushConstants(mainCmd, zoomPipeline->layout(),
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(VulkanPushConstants), &pc);
    vkCmdBindDescriptorSets(mainCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            zoomPipeline->layout(), 0, 1, &ds, 0, nullptr);
    VkBuffer vb = vertBuf->buffer();
    VkDeviceSize vbOffset = 0;
    vkCmdBindVertexBuffers(mainCmd, 0, 1, &vb, &vbOffset);
    vkCmdDraw(mainCmd, static_cast<uint32_t>(verts.size()), 1, 0, 0);

    // === 4. Cursor draw. ===
    // Only draw the fake cursor if we actually hid the OS one; otherwise
    // (e.g. Proportional+Keep) the OS cursor is still visible and drawing
    // another over it would double up.
    if (m_mousePointer != MousePointerHide && m_isMouseHidden) {
        VulkanTexture *cursorTex = m_vkCursorTexture.get();
        if (cursorTex) {
            const auto cursor = effects->cursorImage();
            QSizeF cursorSize = QSizeF(cursor.image().size()) / cursor.image().devicePixelRatio();
            if (m_mousePointer == MousePointerScale) {
                cursorSize *= m_zoom;
            }
            const QPointF p = (effects->cursorPos() - cursor.hotSpot()) * m_zoom + QPoint(xTranslation, yTranslation);

            VulkanPipeline *cursorPipeline = pipelineManager->pipeline(VulkanShaderTrait::MapTexture | VulkanShaderTrait::TransformColorspace);
            if (!cursorPipeline || !cursorPipeline->isValid()) {
                return;
            }

            const float cw = static_cast<float>(cursorSize.width() * scale);
            const float ch = static_cast<float>(cursorSize.height() * scale);
            std::array<VulkanVertex2D, 6> cverts = {{
                {{cw, 0.0f}, {1.0f, 0.0f}},
                {{0.0f, 0.0f}, {0.0f, 0.0f}},
                {{0.0f, ch}, {0.0f, 1.0f}},
                {{0.0f, ch}, {0.0f, 1.0f}},
                {{cw, ch}, {1.0f, 1.0f}},
                {{cw, 0.0f}, {1.0f, 0.0f}},
            }};
            auto cVertBuf = VulkanBuffer::createStreamingBuffer(ctx, sizeof(cverts));
            if (!cVertBuf || !cVertBuf->isValid()) {
                return;
            }
            cVertBuf->upload(cverts.data(), sizeof(cverts));

            QMatrix4x4 cmvp = viewport.projectionMatrix();
            cmvp.translate(p.x() * scale, p.y() * scale);
            VulkanPushConstants cpc{};
            memcpy(cpc.mvp, cmvp.constData(), sizeof(cpc.mvp));
            cpc.textureMatrix[0] = cpc.textureMatrix[5] = cpc.textureMatrix[10] = cpc.textureMatrix[15] = 1.0f;

            VulkanUniforms cubo = ubo; // reuse the colorspace setup; opacity/brightness/saturation already 1
            // The cursor image is sRGB; switch the source description for the colorspace transform.
            const ColorDescription cursorSrc = ColorDescription::sRGB;
            QMatrix4x4 cursorColorimetry = cursorSrc.toOther(dstColor, RenderingIntent::Perceptual);
            memcpy(cubo.colorimetryTransform, cursorColorimetry.data(), sizeof(cubo.colorimetryTransform));
            cubo.sourceTransferFunction = cursorSrc.transferFunction().type;
            cubo.sourceTransferParams[0] = cursorSrc.transferFunction().minLuminance;
            cubo.sourceTransferParams[1] = cursorSrc.transferFunction().maxLuminance - cursorSrc.transferFunction().minLuminance;
            cubo.sourceReferenceLuminance = cursorSrc.referenceLuminance();

            auto cUboBuf = VulkanBuffer::createUniformBuffer(ctx, sizeof(VulkanUniforms));
            if (!cUboBuf || !cUboBuf->isValid()) {
                return;
            }
            cUboBuf->upload(&cubo, sizeof(cubo));

            VkDescriptorSet cds = ctx->allocateDescriptorSet(cursorPipeline->descriptorSetLayout());
            if (cds == VK_NULL_HANDLE) {
                return;
            }
            VkDescriptorImageInfo cImg{cursorTex->sampler(), cursorTex->imageView(),
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            std::array<VkDescriptorImageInfo, 4> cImageInfos{cImg, cImg, cImg, cImg};
            VkDescriptorBufferInfo cBufInfo{cUboBuf->buffer(), 0, sizeof(VulkanUniforms)};
            std::array<VkWriteDescriptorSet, 2> cWrites{};
            cWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            cWrites[0].dstSet = cds;
            cWrites[0].dstBinding = 0;
            cWrites[0].descriptorCount = 4;
            cWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            cWrites[0].pImageInfo = cImageInfos.data();
            cWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            cWrites[1].dstSet = cds;
            cWrites[1].dstBinding = 1;
            cWrites[1].descriptorCount = 1;
            cWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            cWrites[1].pBufferInfo = &cBufInfo;
            vkUpdateDescriptorSets(ctx->backend()->device(), cWrites.size(), cWrites.data(), 0, nullptr);

            vkCmdBindPipeline(mainCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cursorPipeline->pipeline());
            vkCmdPushConstants(mainCmd, cursorPipeline->layout(),
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(VulkanPushConstants), &cpc);
            vkCmdBindDescriptorSets(mainCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    cursorPipeline->layout(), 0, 1, &cds, 0, nullptr);
            VkBuffer cvb = cVertBuf->buffer();
            VkDeviceSize cvbOffset = 0;
            vkCmdBindVertexBuffers(mainCmd, 0, 1, &cvb, &cvbOffset);
            vkCmdDraw(mainCmd, static_cast<uint32_t>(cverts.size()), 1, 0, 0);
        }
    }
}
#endif

void ZoomEffect::paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport, int mask, const QRegion &region, Output *screen)
{
#if HAVE_VULKAN
    if (effects->isVulkanCompositing()) {
        paintScreenVulkan(renderTarget, viewport, mask, region, screen);
        return;
    }
#endif

    OffscreenData *offscreenData = ensureOffscreenData(renderTarget, viewport, screen);
    if (!offscreenData) {
        return;
    }

    // Render the scene in an offscreen texture and then upscale it.
    RenderTarget offscreenRenderTarget(offscreenData->framebuffer.get(), renderTarget.colorDescription());
    RenderViewport offscreenViewport(viewport.renderRect(), viewport.scale(), offscreenRenderTarget);
    GLFramebuffer::pushFramebuffer(offscreenData->framebuffer.get());
    effects->paintScreen(offscreenRenderTarget, offscreenViewport, mask, region, screen);
    GLFramebuffer::popFramebuffer();

    const QSize screenSize = effects->virtualScreenSize();
    const auto scale = viewport.scale();

    // mouse-tracking allows navigation of the zoom-area using the mouse.
    qreal xTranslation = 0;
    qreal yTranslation = 0;
    switch (m_mouseTracking) {
    case MouseTrackingProportional:
        xTranslation = -int(m_cursorPoint.x() * (m_zoom - 1.0));
        yTranslation = -int(m_cursorPoint.y() * (m_zoom - 1.0));
        m_prevPoint = m_cursorPoint;
        break;
    case MouseTrackingCentered:
        m_prevPoint = m_cursorPoint;
        // fall through
    case MouseTrackingDisabled:
        xTranslation = std::min(0, std::max(int(screenSize.width() - screenSize.width() * m_zoom), int(screenSize.width() / 2 - m_prevPoint.x() * m_zoom)));
        yTranslation = std::min(0, std::max(int(screenSize.height() - screenSize.height() * m_zoom), int(screenSize.height() / 2 - m_prevPoint.y() * m_zoom)));
        break;
    case MouseTrackingPush: {
        // touching an edge of the screen moves the zoom-area in that direction.
        const int x = m_cursorPoint.x() * m_zoom - m_prevPoint.x() * (m_zoom - 1.0);
        const int y = m_cursorPoint.y() * m_zoom - m_prevPoint.y() * (m_zoom - 1.0);
        const int threshold = 4;
        const QRectF currScreen = effects->screenAt(QPoint(x, y))->geometry();

        // bounds of the screen the cursor's on
        const int screenTop = currScreen.top();
        const int screenLeft = currScreen.left();
        const int screenRight = currScreen.right();
        const int screenBottom = currScreen.bottom();
        const int screenCenterX = currScreen.center().x();
        const int screenCenterY = currScreen.center().y();

        // figure out whether we have adjacent displays in all 4 directions
        // We pan within the screen in directions where there are no adjacent screens.
        const bool adjacentLeft = screenExistsAt(QPoint(screenLeft - 1, screenCenterY));
        const bool adjacentRight = screenExistsAt(QPoint(screenRight + 1, screenCenterY));
        const bool adjacentTop = screenExistsAt(QPoint(screenCenterX, screenTop - 1));
        const bool adjacentBottom = screenExistsAt(QPoint(screenCenterX, screenBottom + 1));

        m_xMove = m_yMove = 0;
        if (x < screenLeft + threshold && !adjacentLeft) {
            m_xMove = (x - threshold - screenLeft) / m_zoom;
        } else if (x > screenRight - threshold && !adjacentRight) {
            m_xMove = (x + threshold - screenRight) / m_zoom;
        }
        if (y < screenTop + threshold && !adjacentTop) {
            m_yMove = (y - threshold - screenTop) / m_zoom;
        } else if (y > screenBottom - threshold && !adjacentBottom) {
            m_yMove = (y + threshold - screenBottom) / m_zoom;
        }
        if (m_xMove) {
            m_prevPoint.setX(m_prevPoint.x() + m_xMove);
        }
        if (m_yMove) {
            m_prevPoint.setY(m_prevPoint.y() + m_yMove);
        }
        xTranslation = -int(m_prevPoint.x() * (m_zoom - 1.0));
        yTranslation = -int(m_prevPoint.y() * (m_zoom - 1.0));
        break;
    }
    }

    // use the focusPoint if focus tracking is enabled
    if (isFocusTrackingEnabled() || isTextCaretTrackingEnabled()) {
        bool acceptFocus = true;
        if (m_mouseTracking != MouseTrackingDisabled && m_focusDelay > 0) {
            // Wait some time for the mouse before doing the switch. This serves as threshold
            // to prevent the focus from jumping around to much while working with the mouse.
            const int msecs = m_lastMouseEvent.msecsTo(m_lastFocusEvent);
            acceptFocus = msecs > m_focusDelay;
        }
        if (acceptFocus) {
            xTranslation = -int(m_focusPoint.x() * (m_zoom - 1.0));
            yTranslation = -int(m_focusPoint.y() * (m_zoom - 1.0));
            m_prevPoint = m_focusPoint;
        }
    }

    // Render transformed offscreen texture.
    glClearColor(0.0, 0.0, 0.0, 0.0);
    glClear(GL_COLOR_BUFFER_BIT);

    GLShader *shader = shaderForZoom(m_zoom);
    GLShaderManager::instance()->pushShader(shader);
    for (auto &[screen, offscreen] : m_offscreenData) {
        QMatrix4x4 matrix;
        matrix.translate(xTranslation * scale, yTranslation * scale);
        matrix.scale(m_zoom, m_zoom);
        matrix.translate(offscreen.viewport.x() * scale, offscreen.viewport.y() * scale);

        shader->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, viewport.projectionMatrix() * matrix);
        shader->setUniform(GLShader::IntUniform::TextureWidth, offscreen.texture->width());
        shader->setUniform(GLShader::IntUniform::TextureHeight, offscreen.texture->height());
        shader->setColorspaceUniforms(offscreen.color, renderTarget.colorDescription(), RenderingIntent::Perceptual);

        offscreen.texture->render(offscreen.viewport.size() * scale);
    }
    GLShaderManager::instance()->popShader();

    if (m_mousePointer != MousePointerHide && m_isMouseHidden) {
        // Draw the mouse-texture at the position matching to zoomed-in image of the desktop. Hiding the
        // previous mouse-cursor and drawing our own fake mouse-cursor is needed to be able to scale the
        // mouse-cursor up and to re-position those mouse-cursor to match to the chosen zoom-level.

        GLTexture *cursorTexture = ensureCursorTexture();
        if (cursorTexture) {
            const auto cursor = effects->cursorImage();
            QSizeF cursorSize = QSizeF(cursor.image().size()) / cursor.image().devicePixelRatio();
            if (m_mousePointer == MousePointerScale) {
                cursorSize *= m_zoom;
            }

            const QPointF p = (effects->cursorPos() - cursor.hotSpot()) * m_zoom + QPoint(xTranslation, yTranslation);

            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            auto s = GLShaderManager::instance()->pushShader(GLShaderTrait::MapTexture | GLShaderTrait::TransformColorspace);
            s->setColorspaceUniforms(ColorDescription::sRGB, renderTarget.colorDescription(), RenderingIntent::Perceptual);
            QMatrix4x4 mvp = viewport.projectionMatrix();
            mvp.translate(p.x() * scale, p.y() * scale);
            s->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, mvp);
            cursorTexture->render(cursorSize * scale);
            GLShaderManager::instance()->popShader();
            glDisable(GL_BLEND);
        }
    }
}

void ZoomEffect::postPaintScreen()
{
    if (m_zoom == m_targetZoom) {
        m_lastPresentTime = std::chrono::milliseconds::zero();
    }

    if (m_zoom == 1.0 || m_zoom != m_targetZoom) {
        // Either animation is running or the zoom effect has stopped.
        effects->addRepaintFull();
    }

    effects->postPaintScreen();
}

void ZoomEffect::zoomIn()
{
    zoomTo(-1.0);
}

void ZoomEffect::zoomTo(double to)
{
    m_sourceZoom = m_zoom;
    if (to < 0.0) {
        setTargetZoom(m_targetZoom * m_zoomFactor);
    } else {
        setTargetZoom(to);
    }
    m_cursorPoint = effects->cursorPos().toPoint();
    if (m_mouseTracking == MouseTrackingDisabled) {
        m_prevPoint = m_cursorPoint;
    }
    effects->addRepaintFull();
}

void ZoomEffect::zoomOut()
{
    m_sourceZoom = m_zoom;
    setTargetZoom(m_targetZoom / m_zoomFactor);
    if ((m_zoomFactor > 1 && m_targetZoom < 1.01) || (m_zoomFactor < 1 && m_targetZoom > 0.99)) {
        setTargetZoom(1);
    }
    if (m_mouseTracking == MouseTrackingDisabled) {
        m_prevPoint = effects->cursorPos().toPoint();
    }
    effects->addRepaintFull();
}

void ZoomEffect::actualSize()
{
    m_sourceZoom = m_zoom;
    setTargetZoom(1);
    effects->addRepaintFull();
}

void ZoomEffect::timelineFrameChanged(int /* frame */)
{
    const QSize screenSize = effects->virtualScreenSize();
    m_prevPoint.setX(std::max(0, std::min(screenSize.width(), m_prevPoint.x() + m_xMove)));
    m_prevPoint.setY(std::max(0, std::min(screenSize.height(), m_prevPoint.y() + m_yMove)));
    m_cursorPoint = m_prevPoint;
    effects->addRepaintFull();
}

void ZoomEffect::moveZoom(int x, int y)
{
    if (m_timeline.state() == QTimeLine::Running) {
        m_timeline.stop();
    }

    const QSize screenSize = effects->virtualScreenSize();
    if (x < 0) {
        m_xMove = -std::max(1.0, screenSize.width() / m_zoom / m_moveFactor);
    } else if (x > 0) {
        m_xMove = std::max(1.0, screenSize.width() / m_zoom / m_moveFactor);
    } else {
        m_xMove = 0;
    }

    if (y < 0) {
        m_yMove = -std::max(1.0, screenSize.height() / m_zoom / m_moveFactor);
    } else if (y > 0) {
        m_yMove = std::max(1.0, screenSize.height() / m_zoom / m_moveFactor);
    } else {
        m_yMove = 0;
    }

    m_timeline.start();
}

void ZoomEffect::moveZoomLeft()
{
    moveZoom(-1, 0);
}

void ZoomEffect::moveZoomRight()
{
    moveZoom(1, 0);
}

void ZoomEffect::moveZoomUp()
{
    moveZoom(0, -1);
}

void ZoomEffect::moveZoomDown()
{
    moveZoom(0, 1);
}

void ZoomEffect::moveMouseToFocus()
{
    if (!ZoomEffect::isActive()) {
        const auto window = effects->activeWindow();
        if (!window) {
            return;
        }
        const auto center = window->frameGeometry().center();
        QCursor::setPos(center.x(), center.y());
    } else {
        QCursor::setPos(m_focusPoint.x(), m_focusPoint.y());
    }
}

void ZoomEffect::moveMouseToCenter()
{
    const QRect r = effects->activeScreen()->geometry();
    QCursor::setPos(r.x() + r.width() / 2, r.y() + r.height() / 2);
}

void ZoomEffect::slotMouseChanged(const QPointF &pos, const QPointF &old)
{
    if (m_zoom == 1.0) {
        return;
    }
    m_cursorPoint = pos.toPoint();
    if (pos != old) {
        m_lastMouseEvent = QTime::currentTime();
        effects->addRepaintFull();
    }
}

void ZoomEffect::slotWindowAdded(EffectWindow *w)
{
    connect(w, &EffectWindow::windowDamaged, this, &ZoomEffect::slotWindowDamaged);
}

void ZoomEffect::slotWindowDamaged()
{
    if (m_zoom != 1.0) {
        effects->addRepaintFull();
    }
}

void ZoomEffect::slotScreenRemoved(Output *screen)
{
    if (auto it = m_offscreenData.find(screen); it != m_offscreenData.end()) {
        if (effects->isOpenGLCompositing()) {
            effects->makeOpenGLContextCurrent();
        }
        m_offscreenData.erase(it);
    }
#if HAVE_VULKAN
    if (auto it = m_vkOffscreenData.find(screen); it != m_vkOffscreenData.end()) {
        m_vkOffscreenData.erase(it);
    }
#endif
}

void ZoomEffect::moveFocus(const QPoint &point)
{
    if (m_zoom == 1.0) {
        return;
    }
    m_focusPoint = point;
    m_lastFocusEvent = QTime::currentTime();
    effects->addRepaintFull();
}

bool ZoomEffect::supported()
{
    return effects->isOpenGLCompositing() || effects->isVulkanCompositing();
}

bool ZoomEffect::isActive() const
{
    return m_zoom != 1.0 || m_zoom != m_targetZoom;
}

int ZoomEffect::requestedEffectChainPosition() const
{
    return 10;
}

qreal ZoomEffect::configuredZoomFactor() const
{
    return m_zoomFactor;
}

int ZoomEffect::configuredMousePointer() const
{
    return m_mousePointer;
}

int ZoomEffect::configuredMouseTracking() const
{
    return m_mouseTracking;
}

int ZoomEffect::configuredFocusDelay() const
{
    return m_focusDelay;
}

qreal ZoomEffect::configuredMoveFactor() const
{
    return m_moveFactor;
}

qreal ZoomEffect::targetZoom() const
{
    return m_targetZoom;
}

bool ZoomEffect::screenExistsAt(const QPoint &point) const
{
    const Output *output = effects->screenAt(point);
    return output && output->geometry().contains(point);
}

void ZoomEffect::setTargetZoom(double value)
{
    value = std::min(value, 100.0);
    const bool newActive = value != 1.0;
    const bool oldActive = m_targetZoom != 1.0;
    if (newActive && !oldActive) {
        connect(effects, &EffectsHandler::mouseChanged, this, &ZoomEffect::slotMouseChanged);
    } else if (!newActive && oldActive) {
        disconnect(effects, &EffectsHandler::mouseChanged, this, &ZoomEffect::slotMouseChanged);
    }
    m_targetZoom = value;
}

} // namespace

#include "moc_zoom.cpp"
