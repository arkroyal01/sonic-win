/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2010 Martin Gräßlin <mgraesslin@kde.org>
    SPDX-FileCopyrightText: 2010 Nokia Corporation and /or its subsidiary(-ies)
    SPDX-FileCopyrightText: 2021 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "screenshot.h"
#include "screenshotdbusinterface2.h"

#include "config-kwin.h"

#include "compositor.h"
#include "core/output.h"
#include "core/pixelgrid.h"
#include "core/rendertarget.h"
#include "core/renderviewport.h"
#include "effect/effecthandler.h"
#include "opengl/glplatform.h"
#include "opengl/glutils.h"

#if HAVE_VULKAN
#include "platformsupport/scenes/vulkan/vulkanbackend.h"
#include "platformsupport/scenes/vulkan/vulkancontext.h"
#include "platformsupport/scenes/vulkan/vulkanframebuffer.h"
#include "platformsupport/scenes/vulkan/vulkanrenderpass.h"
#include "platformsupport/scenes/vulkan/vulkanrendertarget.h"
#include "scene/itemrenderer_vulkan.h"
#include "scene/workspacescene_vulkan.h"
#include <vulkan/vulkan.h>
#endif

#include <QPainter>

namespace KWin
{

struct ScreenShotWindowData
{
    QPromise<QImage> promise;
    ScreenShotFlags flags;
    EffectWindow *window = nullptr;
};

struct ScreenShotAreaData
{
    QPromise<QImage> promise;
    ScreenShotFlags flags;
    QRect area;
    QImage result;
    QList<Output *> screens;
};

struct ScreenShotScreenData
{
    QPromise<QImage> promise;
    ScreenShotFlags flags;
    Output *screen = nullptr;
};

static void convertFromGLImage(QImage &img, int w, int h, const OutputTransform &renderTargetTransformation)
{
    // from QtOpenGL/qgl.cpp
    // SPDX-FileCopyrightText: 2010 Nokia Corporation and /or its subsidiary(-ies)
    // see https://github.com/qt/qtbase/blob/dev/src/opengl/qgl.cpp
    if (QSysInfo::ByteOrder == QSysInfo::BigEndian) {
        // OpenGL gives RGBA; Qt wants ARGB
        uint *p = reinterpret_cast<uint *>(img.bits());
        uint *end = p + w * h;
        while (p < end) {
            uint a = *p << 24;
            *p = (*p >> 8) | a;
            p++;
        }
    } else {
        // OpenGL gives ABGR (i.e. RGBA backwards); Qt wants ARGB
        for (int y = 0; y < h; y++) {
            uint *q = reinterpret_cast<uint *>(img.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const uint pixel = *q;
                *q = ((pixel << 16) & 0xff0000) | ((pixel >> 16) & 0xff)
                    | (pixel & 0xff00ff00);

                q++;
            }
        }
    }

    QMatrix4x4 matrix;
    // apply render target transformation
    matrix *= renderTargetTransformation.inverted().toMatrix();
    // OpenGL textures are flipped vs QImage
    matrix.scale(1, -1);
    img = img.transformed(matrix.toTransform());
}

bool ScreenShotEffect::supported()
{
    return effects->isOpenGLCompositing() || effects->isVulkanCompositing();
}

ScreenShotEffect::ScreenShotEffect()
    : m_dbusInterface2(new ScreenShotDBusInterface2(this))
{
    connect(effects, &EffectsHandler::screenAdded, this, &ScreenShotEffect::handleScreenAdded);
    connect(effects, &EffectsHandler::screenRemoved, this, &ScreenShotEffect::handleScreenRemoved);
    connect(effects, &EffectsHandler::windowClosed, this, &ScreenShotEffect::handleWindowClosed);
}

ScreenShotEffect::~ScreenShotEffect()
{
    cancelWindowScreenShots();
    cancelAreaScreenShots();
    cancelScreenScreenShots();
}

QFuture<QImage> ScreenShotEffect::scheduleScreenShot(Output *screen, ScreenShotFlags flags)
{
    for (const ScreenShotScreenData &data : m_screenScreenShots) {
        if (data.screen == screen && data.flags == flags) {
            return data.promise.future();
        }
    }

    ScreenShotScreenData data;
    data.screen = screen;
    data.flags = flags;

    data.promise.start();
    QFuture<QImage> future = data.promise.future();

    m_screenScreenShots.push_back(std::move(data));
    effects->addRepaint(screen->geometry());

    return future;
}

QFuture<QImage> ScreenShotEffect::scheduleScreenShot(const QRect &area, ScreenShotFlags flags)
{
    for (const ScreenShotAreaData &data : m_areaScreenShots) {
        if (data.area == area && data.flags == flags) {
            return data.promise.future();
        }
    }

    ScreenShotAreaData data;
    data.area = area;
    data.flags = flags;

    const QList<Output *> screens = effects->screens();
    for (Output *screen : screens) {
        if (screen->geometry().intersects(area)) {
            data.screens.append(screen);
        }
    }

    qreal devicePixelRatio = 1.0;
    if (flags & ScreenShotNativeResolution) {
        for (const Output *screen : std::as_const(data.screens)) {
            if (screen->scale() > devicePixelRatio) {
                devicePixelRatio = screen->scale();
            }
        }
    }

    data.result = QImage(area.size() * devicePixelRatio, QImage::Format_ARGB32_Premultiplied);
    data.result.fill(Qt::transparent);
    data.result.setDevicePixelRatio(devicePixelRatio);

    data.promise.start();
    QFuture<QImage> future = data.promise.future();

    m_areaScreenShots.push_back(std::move(data));
    effects->addRepaint(area);

    return future;
}

QFuture<QImage> ScreenShotEffect::scheduleScreenShot(EffectWindow *window, ScreenShotFlags flags)
{
    for (const ScreenShotWindowData &data : m_windowScreenShots) {
        if (data.window == window && data.flags == flags) {
            return data.promise.future();
        }
    }

    ScreenShotWindowData data;
    data.window = window;
    data.flags = flags;

    data.promise.start();
    QFuture<QImage> future = data.promise.future();

    m_windowScreenShots.push_back(std::move(data));
    window->addRepaintFull();

    return future;
}

void ScreenShotEffect::cancelWindowScreenShots()
{
    m_windowScreenShots.clear();
}

void ScreenShotEffect::cancelAreaScreenShots()
{
    m_areaScreenShots.clear();
}

void ScreenShotEffect::cancelScreenScreenShots()
{
    m_screenScreenShots.clear();
}

void ScreenShotEffect::paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport, int mask, const QRegion &region, Output *screen)
{
    m_paintedScreen = screen;
    effects->paintScreen(renderTarget, viewport, mask, region, screen);

    for (ScreenShotWindowData &data : m_windowScreenShots) {
        takeScreenShot(&data);
    }
    m_windowScreenShots.clear();

    for (int i = m_areaScreenShots.size() - 1; i >= 0; --i) {
        if (takeScreenShot(renderTarget, viewport, &m_areaScreenShots[i])) {
            m_areaScreenShots.erase(m_areaScreenShots.begin() + i);
        }
    }

    for (int i = m_screenScreenShots.size() - 1; i >= 0; --i) {
        if (takeScreenShot(renderTarget, viewport, &m_screenScreenShots[i])) {
            m_screenScreenShots.erase(m_screenScreenShots.begin() + i);
        }
    }
}

void ScreenShotEffect::takeScreenShot(ScreenShotWindowData *screenshot)
{
    EffectWindow *window = screenshot->window;

    QRectF geometry = window->expandedGeometry();
    qreal devicePixelRatio = 1;

    if (window->hasDecoration() && !(screenshot->flags & ScreenShotIncludeDecoration)) {
        geometry = window->clientGeometry();
    } else if (!(screenshot->flags & ScreenShotIncludeShadow)) {
        geometry = window->frameGeometry();
    }

    if (screenshot->flags & ScreenShotNativeResolution) {
        if (const Output *screen = window->screen()) {
            devicePixelRatio = screen->scale();
        }
    }

    const QRect scaledGeometry = snapToPixelGrid(scaledRect(geometry, devicePixelRatio));
    bool validTarget = true;
    std::unique_ptr<GLTexture> offscreenTexture;
    std::unique_ptr<GLFramebuffer> target;
    if (effects->isOpenGLCompositing()) {
        offscreenTexture = GLTexture::allocate(GL_RGBA8, scaledGeometry.size());
        if (!offscreenTexture) {
            return;
        }
        offscreenTexture->setFilter(GL_LINEAR);
        offscreenTexture->setWrapMode(GL_CLAMP_TO_EDGE);
        target = std::make_unique<GLFramebuffer>(offscreenTexture.get());
        validTarget = target->valid();
    }
    if (validTarget) {
        // render window into offscreen texture
        int mask = PAINT_WINDOW_TRANSFORMED | PAINT_WINDOW_TRANSLUCENT;
        QImage img;
        if (const auto context = effects->openglContext()) {
            RenderTarget renderTarget(target.get());
            RenderViewport viewport(geometry, devicePixelRatio, renderTarget);
            GLFramebuffer::pushFramebuffer(target.get());
            glClearColor(0.0, 0.0, 0.0, 0.0);
            glClear(GL_COLOR_BUFFER_BIT);
            glClearColor(0.0, 0.0, 0.0, 1.0);

            WindowPaintData d;
            effects->drawWindow(renderTarget, viewport, window, mask, infiniteRegion(), d);

            // copy content from framebuffer into image
            img = QImage(offscreenTexture->size(), QImage::Format_ARGB32);
            img.setDevicePixelRatio(devicePixelRatio);
            context->glReadnPixels(0, 0, img.width(), img.height(), GL_RGBA, GL_UNSIGNED_BYTE, img.sizeInBytes(), static_cast<GLvoid *>(img.bits()));
            GLFramebuffer::popFramebuffer();
            convertFromGLImage(img, img.width(), img.height(), renderTarget.transform());
        }
#if HAVE_VULKAN
        else if (effects->isVulkanCompositing()) {
            VulkanContext *ctx = VulkanContext::currentContext();
            if (ctx) {
                const VkFormat fmt = ctx->backend()->colorFormat();
                auto rp = VulkanRenderPass::createForOffscreen(ctx, fmt, false);
                auto fbo = rp ? VulkanFramebuffer::createWithTexture(ctx, rp.get(), scaledGeometry.size(), fmt) : nullptr;
                if (fbo && fbo->isValid()) {
                    VkCommandBuffer cmd = ctx->beginSingleTimeCommands();
                    if (cmd != VK_NULL_HANDLE) {
                        auto vkRT = std::make_unique<VulkanRenderTarget>(fbo.get());
                        vkRT->setCommandBuffer(cmd);
                        RenderTarget offscreenRT(vkRT.get());
                        RenderViewport viewport(geometry, devicePixelRatio, offscreenRT);

                        VkClearValue clearVal{};
                        VkRect2D area{{0, 0}, {uint32_t(scaledGeometry.width()), uint32_t(scaledGeometry.height())}};
                        rp->begin(cmd, fbo->framebuffer(), area, &clearVal, 1);

                        VkViewport vp{};
                        vp.x = 0.0f;
                        vp.y = float(scaledGeometry.height());
                        vp.width = float(scaledGeometry.width());
                        vp.height = -float(scaledGeometry.height());
                        vp.minDepth = 0.0f;
                        vp.maxDepth = 1.0f;
                        vkCmdSetViewport(cmd, 0, 1, &vp);
                        VkRect2D scissor{{0, 0}, {uint32_t(scaledGeometry.width()), uint32_t(scaledGeometry.height())}};
                        vkCmdSetScissor(cmd, 0, 1, &scissor);

                        // Save/restore vertex buffer offset: drawWindow() advances the shared
                        // streaming buffer. endSingleTimeCommands() calls vkQueueWaitIdle so
                        // the GPU finishes before we restore, making the region safe to reuse.
                        auto *vkScene = dynamic_cast<WorkspaceSceneVulkan *>(Compositor::self()->scene());
                        auto *vkRenderer = vkScene ? static_cast<ItemRendererVulkan *>(vkScene->renderer()) : nullptr;
                        const size_t savedOffset = vkRenderer ? vkRenderer->vertexBufferOffset() : 0;

                        WindowPaintData d;
                        // RecursivePaintScope flags this nested flow so debug
                        // builds catch effects that record on the swapchain
                        // command buffer instead of the offscreenRT one.
                        {
                            ItemRendererVulkan::RecursivePaintScope guard;
                            effects->drawWindow(offscreenRT, viewport, window, mask, infiniteRegion(), d);
                        }

                        if (vkRenderer) {
                            vkRenderer->setVertexBufferOffset(savedOffset);
                        }

                        rp->end(cmd);
                        if (VulkanTexture *tex = fbo->colorTexture()) {
                            tex->setCurrentLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                        }
                        // Apply whole-screen post-FX (invert / colorblindness) to the
                        // captured window. These run as a fullscreen post-pass under
                        // Vulkan and so are not part of drawWindow(); without this the
                        // window screenshot would lack the effects visible on screen.
                        if (vkRenderer) {
                            vkRenderer->runFullscreenPostPassesOffscreen(cmd, fbo.get(), offscreenRT);
                        }
                        ctx->endSingleTimeCommands(cmd); // blocks until GPU idle

                        if (VulkanTexture *tex = fbo->colorTexture()) {
                            img = ctx->readTextureToImage(tex);
                            img.setDevicePixelRatio(devicePixelRatio);
                        }
                    }
                }
            }
        }
#endif

        if (screenshot->flags & ScreenShotIncludeCursor) {
            grabPointerImage(img, geometry.x(), geometry.y());
        }

        screenshot->promise.addResult(img);
        screenshot->promise.finish();
    }
}

#if HAVE_VULKAN
// Schedule a post-render-pass capture of @p captureRect (in logical coords) and
// finish @p promise asynchronously once the staging readback completes. Returns
// false on setup failure (caller may fall through to an empty result).
//
// We can't sample the rendered framebuffer from inside paintScreen because
// Vulkan disallows reading from a color attachment while its render pass is
// active. ItemRendererVulkan::registerPostPassCopy queues the copy to be
// recorded right after vkCmdEndRenderPass, then drains the result after the
// submission fence signals.
bool ScreenShotEffect::scheduleVulkanScreenshot(const RenderTarget &renderTarget, const RenderViewport &viewport,
                                                const QRect &captureRect, qreal devicePixelRatio,
                                                ScreenShotFlags flags, const QPoint &cursorOriginInGlobal,
                                                QPromise<QImage> promise)
{
    const auto *vkRT = renderTarget.vulkanTarget();
    if (!vkRT || !vkRT->framebuffer() || !vkRT->framebuffer()->renderPass()) {
        promise.finish();
        return false;
    }
    VulkanFramebuffer *fb = vkRT->framebuffer();
    VkImage srcImage = fb->colorImage();
    if (srcImage == VK_NULL_HANDLE && fb->colorTexture()) {
        srcImage = fb->colorTexture()->image();
    }
    if (srcImage == VK_NULL_HANDLE) {
        promise.finish();
        return false;
    }

    auto *vkScene = dynamic_cast<WorkspaceSceneVulkan *>(Compositor::self()->scene());
    auto *vkRenderer = vkScene ? static_cast<ItemRendererVulkan *>(vkScene->renderer()) : nullptr;
    if (!vkRenderer) {
        promise.finish();
        return false;
    }

    const QRect fbRect = viewport.mapToRenderTarget(captureRect).intersected(QRect(QPoint(0, 0), fb->size()));
    if (fbRect.isEmpty()) {
        promise.finish();
        return false;
    }

    const VkFormat fmt = vkRenderer->backend()->colorFormat();
    const VkImageLayout finalLayout = fb->renderPass()->config().finalLayout;

    // std::function requires copy-constructible callables; QPromise is move-only,
    // so wrap it in a shared_ptr to make the lambda copyable.
    auto sharedPromise = std::make_shared<QPromise<QImage>>(std::move(promise));
    vkRenderer->registerPostPassCopy(srcImage, finalLayout, fmt,
                                     VkOffset3D{fbRect.x(), fbRect.y(), 0},
                                     VkExtent3D{uint32_t(fbRect.width()), uint32_t(fbRect.height()), 1},
                                     [sharedPromise, flags, cursorOriginInGlobal, devicePixelRatio, this](const QImage &img) {
        if (img.isNull()) {
            sharedPromise->finish();
            return;
        }
        QImage out = img.copy();
        out.setDevicePixelRatio(devicePixelRatio);
        if (flags & ScreenShotIncludeCursor) {
            grabPointerImage(out, cursorOriginInGlobal.x(), cursorOriginInGlobal.y());
        }
        sharedPromise->addResult(out);
        sharedPromise->finish();
    });
    return true;
}
#endif

bool ScreenShotEffect::takeScreenShot(const RenderTarget &renderTarget, const RenderViewport &viewport, ScreenShotAreaData *screenshot)
{
#if HAVE_VULKAN
    if (effects->isVulkanCompositing()) {
        return scheduleVulkanScreenshot(renderTarget, viewport, screenshot->area, 1.0,
                                        screenshot->flags, screenshot->area.topLeft(),
                                        std::move(screenshot->promise));
    }
#endif
    // On X11, all screens are painted simultaneously and there is no native HiDPI support.
    QImage snapshot = blitScreenshot(renderTarget, viewport, screenshot->area);
    if (screenshot->flags & ScreenShotIncludeCursor) {
        grabPointerImage(snapshot, screenshot->area.x(), screenshot->area.y());
    }
    screenshot->promise.addResult(snapshot);
    screenshot->promise.finish();
    return true;
}

bool ScreenShotEffect::takeScreenShot(const RenderTarget &renderTarget, const RenderViewport &viewport, ScreenShotScreenData *screenshot)
{

    qreal devicePixelRatio = 1.0;
    if (screenshot->flags & ScreenShotNativeResolution) {
        devicePixelRatio = screenshot->screen->scale();
    }

#if HAVE_VULKAN
    if (effects->isVulkanCompositing()) {
        return scheduleVulkanScreenshot(renderTarget, viewport, screenshot->screen->geometry(),
                                        devicePixelRatio, screenshot->flags,
                                        screenshot->screen->geometry().topLeft(),
                                        std::move(screenshot->promise));
    }
#endif

    QImage snapshot = blitScreenshot(renderTarget, viewport, screenshot->screen->geometry(), devicePixelRatio);
    if (screenshot->flags & ScreenShotIncludeCursor) {
        const int xOffset = screenshot->screen->geometry().x();
        const int yOffset = screenshot->screen->geometry().y();
        grabPointerImage(snapshot, xOffset, yOffset);
    }

    screenshot->promise.addResult(snapshot);
    screenshot->promise.finish();

    return true;
}

QImage ScreenShotEffect::blitScreenshot(const RenderTarget &renderTarget, const RenderViewport &viewport, const QRect &geometry, qreal devicePixelRatio) const
{
    QImage image;

    if (OpenGlContext *context = effects->openglContext()) {
        const auto screenGeometry = m_paintedScreen ? m_paintedScreen->geometry() : effects->virtualScreenGeometry();
        const QSize nativeSize = renderTarget.transform().map(
            snapToPixelGrid(scaledRect(geometry, devicePixelRatio))
                .translated(-snapToPixelGrid(scaledRect(screenGeometry, devicePixelRatio)).topLeft())
                .size());
        image = QImage(nativeSize, QImage::Format_ARGB32);

        const auto texture = GLTexture::allocate(GL_RGBA8, nativeSize);
        if (!texture) {
            return {};
        }
        GLFramebuffer target(texture.get());
        if (renderTarget.texture()) {
            GLFramebuffer::pushFramebuffer(&target);
            ShaderBinder binder(GLShaderTrait::MapTexture | GLShaderTrait::TransformColorspace);
            binder.shader()->setColorspaceUniforms(renderTarget.colorDescription(), ColorDescription::sRGB, RenderingIntent::RelativeColorimetricWithBPC);
            QMatrix4x4 projectionMatrix;
            projectionMatrix.scale(1, -1);
            projectionMatrix *= renderTarget.transform().toMatrix();
            projectionMatrix.scale(1, -1);
            projectionMatrix.ortho(QRect(QPoint(), nativeSize));
            binder.shader()->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, projectionMatrix);
            renderTarget.texture()->render(viewport.mapToRenderTargetTexture(geometry), infiniteRegion(), nativeSize);
        } else {
            target.blitFromFramebuffer(viewport.mapToRenderTarget(geometry));
            GLFramebuffer::pushFramebuffer(&target);
        }
        context->glReadnPixels(0, 0, nativeSize.width(), nativeSize.height(), GL_RGBA,
                               GL_UNSIGNED_BYTE, image.sizeInBytes(), image.bits());
        GLFramebuffer::popFramebuffer();
        convertFromGLImage(image, nativeSize.width(), nativeSize.height(), renderTarget.transform());
    }

    image.setDevicePixelRatio(devicePixelRatio);
    return image;
}

void ScreenShotEffect::grabPointerImage(QImage &snapshot, int xOffset, int yOffset) const
{
    if (effects->isCursorHidden()) {
        return;
    }

    const PlatformCursorImage cursor = effects->cursorImage();
    if (cursor.image().isNull()) {
        return;
    }

    QPainter painter(&snapshot);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(effects->cursorPos() - cursor.hotSpot() - QPoint(xOffset, yOffset), cursor.image());
}

bool ScreenShotEffect::isActive() const
{
    return (!m_windowScreenShots.empty() || !m_areaScreenShots.empty() || !m_screenScreenShots.empty())
        && !effects->isScreenLocked();
}

int ScreenShotEffect::requestedEffectChainPosition() const
{
    return 0;
}

void ScreenShotEffect::handleScreenAdded()
{
    cancelAreaScreenShots();
}

void ScreenShotEffect::handleScreenRemoved(Output *screen)
{
    cancelAreaScreenShots();

    std::erase_if(m_screenScreenShots, [screen](const auto &screenshot) {
        return screenshot.screen == screen;
    });
}

void ScreenShotEffect::handleWindowClosed(EffectWindow *window)
{
    std::erase_if(m_windowScreenShots, [window](const auto &screenshot) {
        return screenshot.window == window;
    });
}

} // namespace KWin

#include "moc_screenshot.cpp"
