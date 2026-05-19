/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2016 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "colorpicker.h"
#include "config-kwin.h"
#include "core/rendertarget.h"
#include "core/renderviewport.h"
#include "effect/effecthandler.h"
#include "opengl/openglcontext.h"
#include <KLocalizedString>
#include <QDBusConnection>
#include <QDBusMetaType>

#include <epoxy/gl.h>

#if HAVE_VULKAN
#include "compositor.h"
#include "platformsupport/scenes/vulkan/vulkanbackend.h"
#include "platformsupport/scenes/vulkan/vulkanframebuffer.h"
#include "platformsupport/scenes/vulkan/vulkanrenderpass.h"
#include "platformsupport/scenes/vulkan/vulkanrendertarget.h"
#include "scene/itemrenderer_vulkan.h"
#include "scene/workspacescene_vulkan.h"
#include <vulkan/vulkan.h>
#endif

Q_DECLARE_METATYPE(QColor)

QDBusArgument &operator<<(QDBusArgument &argument, const QColor &color)
{
    argument.beginStructure();
    argument << color.rgba();
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, QColor &color)
{
    argument.beginStructure();
    QRgb rgba;
    argument >> rgba;
    argument.endStructure();
    color = QColor::fromRgba(rgba);
    return argument;
}

namespace KWin
{

bool ColorPickerEffect::supported()
{
    return effects->isOpenGLCompositing() || effects->isVulkanCompositing();
}

ColorPickerEffect::ColorPickerEffect()
    : m_scheduledPosition(QPoint(-1, -1))
{
    qDBusRegisterMetaType<QColor>();
    QDBusConnection::sessionBus().registerObject(QStringLiteral("/ColorPicker"), this, QDBusConnection::ExportScriptableContents);
}

ColorPickerEffect::~ColorPickerEffect()
{
    setPicking(false);
}

void ColorPickerEffect::paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport, int mask, const QRegion &region, Output *screen)
{
    effects->paintScreen(renderTarget, viewport, mask, region, screen);

    const QRectF geo = viewport.renderRect();
    if (m_scheduledPosition == QPoint(-1, -1) || !exclusiveContains(geo, m_scheduledPosition)) {
        return;
    }

    if (OpenGlContext *context = effects->openglContext()) {
        std::array<float, 4> data;
        constexpr GLsizei PIXEL_SIZE = 1;
        const QPoint texturePosition = viewport.mapToRenderTarget(m_scheduledPosition).toPoint();

        context->glReadnPixels(texturePosition.x(), renderTarget.size().height() - texturePosition.y() - PIXEL_SIZE, PIXEL_SIZE, PIXEL_SIZE, GL_RGBA, GL_FLOAT, sizeof(float) * data.size(), data.data());
        QVector3D sRGB = 255 * renderTarget.colorDescription().mapTo(QVector3D(data[0], data[1], data[2]), ColorDescription::sRGB, RenderingIntent::RelativeColorimetric);
        QDBusConnection::sessionBus().send(m_replyMessage.createReply(QColor(sRGB.x(), sRGB.y(), sRGB.z())));
        setPicking(false);
        m_scheduledPosition = QPoint(-1, -1);
        return;
    }

#if HAVE_VULKAN
    if (effects->isVulkanCompositing()) {
        scheduleVulkanPick(renderTarget, viewport);
    }
#endif
}

#if HAVE_VULKAN
// Vulkan disallows sampling/transferring from a color attachment while its render
// pass is active, so we defer the 1x1 readback to ItemRendererVulkan's post-pass
// copy queue (same primitive used by the screenshot effect). The callback fires
// after the frame's submission fence signals; we read the single pixel out of
// the resulting QImage and send the DBus reply then.
void ColorPickerEffect::scheduleVulkanPick(const RenderTarget &renderTarget, const RenderViewport &viewport)
{
    const auto *vkRT = renderTarget.vulkanTarget();
    if (!vkRT || !vkRT->framebuffer() || !vkRT->framebuffer()->renderPass()) {
        return;
    }
    VulkanFramebuffer *fb = vkRT->framebuffer();
    VkImage srcImage = fb->colorImage();
    if (srcImage == VK_NULL_HANDLE && fb->colorTexture()) {
        srcImage = fb->colorTexture()->image();
    }
    if (srcImage == VK_NULL_HANDLE) {
        return;
    }

    auto *vkScene = dynamic_cast<WorkspaceSceneVulkan *>(Compositor::self()->scene());
    auto *vkRenderer = vkScene ? static_cast<ItemRendererVulkan *>(vkScene->renderer()) : nullptr;
    if (!vkRenderer) {
        return;
    }

    const QPoint texPos = viewport.mapToRenderTarget(m_scheduledPosition).toPoint();
    const QRect fbRect = QRect(texPos, QSize(1, 1)).intersected(QRect(QPoint(0, 0), fb->size()));
    if (fbRect.isEmpty()) {
        return;
    }

    const VkFormat fmt = vkRenderer->backend()->colorFormat();
    const VkImageLayout finalLayout = fb->renderPass()->config().finalLayout;
    const QDBusMessage reply = m_replyMessage;

    vkRenderer->registerPostPassCopy(srcImage, finalLayout, fmt,
                                     VkOffset3D{fbRect.x(), fbRect.y(), 0},
                                     VkExtent3D{1, 1, 1},
                                     [this, reply](const QImage &img) {
        if (img.isNull()) {
            QDBusConnection::sessionBus().send(reply.createErrorReply(
                QStringLiteral("org.kde.kwin.ColorPicker.Error.Failed"),
                QStringLiteral("Failed to read pixel from framebuffer")));
        } else {
            const QRgb pixel = img.pixel(0, 0);
            QDBusConnection::sessionBus().send(reply.createReply(
                QColor(qRed(pixel), qGreen(pixel), qBlue(pixel))));
        }
        setPicking(false);
    });
    m_scheduledPosition = QPoint(-1, -1);
}
#endif

QColor ColorPickerEffect::pick()
{
    if (!calledFromDBus()) {
        return QColor();
    }
    if (m_picking) {
        sendErrorReply(QDBusError::Failed, "Color picking is already in progress");
        return QColor();
    }
    setPicking(true);
    m_replyMessage = message();
    setDelayedReply(true);
    showInfoMessage();
    effects->startInteractivePositionSelection(
        [this](const QPointF &p) {
            hideInfoMessage();
            if (p == QPointF(-1, -1)) {
                // error condition
                QDBusConnection::sessionBus().send(m_replyMessage.createErrorReply(QStringLiteral("org.kde.kwin.ColorPicker.Error.Cancelled"), "Color picking got cancelled"));
                setPicking(false);
            } else {
                m_scheduledPosition = p;
                effects->addRepaintFull();
            }
        });
    return QColor();
}

void ColorPickerEffect::showInfoMessage()
{
    effects->showOnScreenMessage(i18n("Select a position for color picking with left click or enter.\nEscape or right click to cancel."), QStringLiteral("color-picker"));
}

void ColorPickerEffect::hideInfoMessage()
{
    effects->hideOnScreenMessage();
}

void ColorPickerEffect::setPicking(bool picking)
{
    if (m_picking != picking) {
        m_picking = picking;
        Q_EMIT effects->colorPickerActiveChanged();
    }
}

bool ColorPickerEffect::isActive() const
{
    return m_picking && !effects->isScreenLocked();
}

} // namespace

#include "moc_colorpicker.cpp"
