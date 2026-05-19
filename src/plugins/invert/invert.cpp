/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2007 Rivo Laks <rivolaks@hot.ee>
    SPDX-FileCopyrightText: 2008 Lucas Murray <lmurray@undefinedfire.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "invert.h"

#include "core/colorspace.h"
#include "core/rendertarget.h"
#include "core/renderviewport.h"
#include "cursor.h"
#include "cursorsource.h"
#include "effect/effecthandler.h"
#include "opengl/glshadermanager.h"
#include "opengl/gltexture.h"
#include "opengl/glutils.h"
#include <KGlobalAccel>
#include <KLocalizedString>
#include <QAction>
#include <QMatrix4x4>

#if HAVE_VULKAN
#include "compositor.h"
#include "platformsupport/scenes/vulkan/vulkanbuffer.h"
#include "platformsupport/scenes/vulkan/vulkancontext.h"
#include "platformsupport/scenes/vulkan/vulkanpipeline.h"
#include "platformsupport/scenes/vulkan/vulkanpipelinemanager.h"
#include "platformsupport/scenes/vulkan/vulkantexture.h"
#include "scene/itemrenderer_vulkan.h"
#include "scene/workspacescene_vulkan.h"
#include <array>
#include <cstring>
#endif

Q_LOGGING_CATEGORY(KWIN_INVERT, "kwin_effect_invert", QtWarningMsg)

static QImage invertCursorImage(const QImage &src)
{
    QImage img = src.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < img.height(); ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            const QRgb px = line[x];
            const int a = qAlpha(px);
            // Premultiplied inversion: new_channel = alpha - channel
            line[x] = qRgba(a - qRed(px), a - qGreen(px), a - qBlue(px), a);
        }
    }
    return img;
}

static void ensureResources()
{
    // Must initialize resources manually because the effect is a static lib.
    Q_INIT_RESOURCE(invert);
}

namespace KWin
{

InvertEffect::InvertEffect()
    : m_inited(false)
    , m_valid(true)
    , m_shader(nullptr)
    , m_allWindows(true)
{
    QAction *a = new QAction(this);
    a->setAutoRepeat(false);
    a->setObjectName(QStringLiteral("Invert"));
    a->setText(i18n("Toggle Invert Effect"));
    KGlobalAccel::self()->setDefaultShortcut(a, QList<QKeySequence>() << (Qt::CTRL | Qt::META | Qt::Key_I));
    KGlobalAccel::self()->setShortcut(a, QList<QKeySequence>() << (Qt::CTRL | Qt::META | Qt::Key_I));
    connect(a, &QAction::triggered, this, &InvertEffect::toggleScreenInversion);

    QAction *b = new QAction(this);
    b->setAutoRepeat(false);
    b->setObjectName(QStringLiteral("InvertWindow"));
    b->setText(i18n("Toggle Invert Effect on Window"));
    KGlobalAccel::self()->setDefaultShortcut(b, QList<QKeySequence>() << (Qt::CTRL | Qt::META | Qt::Key_U));
    KGlobalAccel::self()->setShortcut(b, QList<QKeySequence>() << (Qt::CTRL | Qt::META | Qt::Key_U));
    connect(b, &QAction::triggered, this, &InvertEffect::toggleWindow);

    QAction *c = new QAction(this);
    c->setObjectName(QStringLiteral("Invert Screen Colors"));
    c->setText(i18n("Invert Screen Colors"));
    KGlobalAccel::self()->setDefaultShortcut(c, QList<QKeySequence>());
    KGlobalAccel::self()->setShortcut(c, QList<QKeySequence>());
    connect(c, &QAction::triggered, this, &InvertEffect::toggleScreenInversion);

    connect(effects, &EffectsHandler::windowAdded, this, &InvertEffect::slotWindowAdded);
    connect(effects, &EffectsHandler::windowClosed, this, &InvertEffect::slotWindowClosed);

    // Invert all windows that exist at load time (plugin enabled in KCM).
    for (EffectWindow *window : effects->stackingOrder()) {
        invert(window);
    }

    // Hide hardware cursor and draw a software-inverted copy in paintScreen().
    effects->hideCursor();
    connect(Cursors::self()->mouse(), &Cursor::cursorChanged, this, [this]() {
        m_cursorDirty = true;
        if (m_allWindows) {
            effects->addRepaintFull();
        }
    });
    // Repaint only when the cursor actually moves, not every frame.
    connect(effects, &EffectsHandler::mouseChanged, this,
            [this](const QPointF &, const QPointF &, Qt::MouseButtons, Qt::MouseButtons,
                   Qt::KeyboardModifiers, Qt::KeyboardModifiers) {
        if (m_allWindows && m_valid) {
            effects->addRepaintFull();
        }
    });

    effects->addRepaintFull();
}

InvertEffect::~InvertEffect()
{
    if (m_allWindows) {
        effects->showCursor();
    }
}

bool InvertEffect::supported()
{
    return effects->isOpenGLCompositing() || effects->isVulkanCompositing();
}

bool InvertEffect::isInvertable(EffectWindow *window) const
{
    return m_allWindows != m_windows.contains(window);
}

void InvertEffect::invert(EffectWindow *window)
{
    if (m_valid && !m_inited) {
        m_valid = loadData();
    }

    redirect(window);

    if (effects->isOpenGLCompositing()) {
        setShader(window, m_shader.get());
    }
#if HAVE_VULKAN
    else if (effects->isVulkanCompositing()) {
        if (!m_vkPipeline) {
            auto *scene = dynamic_cast<WorkspaceSceneVulkan *>(Compositor::self()->scene());
            auto *renderer = scene ? static_cast<ItemRendererVulkan *>(scene->renderer()) : nullptr;
            auto *ctx = renderer ? renderer->context() : nullptr;
            if (ctx && ctx->pipelineManager()) {
                m_vkPipeline = ctx->pipelineManager()->pipeline(
                    VulkanShaderTrait::MapTexture | VulkanShaderTrait::Modulate | VulkanShaderTrait::Invert);
            }
        }
        if (m_vkPipeline) {
            setPipeline(window, m_vkPipeline);
        }
    }
#endif
}

void InvertEffect::uninvert(EffectWindow *window)
{
    unredirect(window);
}

bool InvertEffect::loadData()
{
    ensureResources();
    m_inited = true;

    if (effects->isOpenGLCompositing()) {
        m_shader = GLShaderManager::instance()->generateShaderFromFile(GLShaderTrait::MapTexture, QString(), QStringLiteral(":/effects/invert/shaders/invert.frag"));
        if (!m_shader->isValid()) {
            qCCritical(KWIN_INVERT) << "The shader failed to load!";
            return false;
        }
    }

    return true;
}

void InvertEffect::slotWindowAdded(KWin::EffectWindow *w)
{
    if (isInvertable(w)) {
        invert(w);
    }
}

void InvertEffect::slotWindowClosed(EffectWindow *w)
{
    m_windows.removeOne(w);
}

void InvertEffect::toggleScreenInversion()
{
    m_allWindows = !m_allWindows;

    if (m_allWindows) {
        effects->hideCursor();
    } else {
        effects->showCursor();
        m_glCursorTexture.reset();
        m_vkCursorTexture.reset();
    }

    const auto windows = effects->stackingOrder();
    for (EffectWindow *window : windows) {
        if (isInvertable(window)) {
            invert(window);
        } else {
            uninvert(window);
        }
    }

    effects->addRepaintFull();
}

void InvertEffect::toggleWindow()
{
    if (!effects->activeWindow()) {
        return;
    }
    if (!m_windows.contains(effects->activeWindow())) {
        m_windows.append(effects->activeWindow());
    } else {
        m_windows.removeOne(effects->activeWindow());
    }
    if (isInvertable(effects->activeWindow())) {
        invert(effects->activeWindow());
    } else {
        uninvert(effects->activeWindow());
    }
    effects->activeWindow()->addRepaintFull();
}

bool InvertEffect::isActive() const
{
    return m_valid && (m_allWindows || !m_windows.isEmpty());
}

bool InvertEffect::provides(Feature f)
{
    return f == ScreenInversion;
}

void InvertEffect::rebuildCursorImage()
{
    m_cursorDirty = false;
    m_glCursorTexture.reset();
    m_vkCursorTexture.reset();

    auto *cursor = Cursors::self()->mouse();
    auto *src = qobject_cast<ShapeCursorSource *>(cursor->source());
    if (!src || src->image().isNull()) {
        m_cursorImage = {};
        return;
    }
    m_cursorImage = invertCursorImage(src->image());
    m_cursorHotspot = src->hotspot().toPoint();
}

#if HAVE_VULKAN
void InvertEffect::paintVulkanCursor(const RenderTarget &renderTarget, const RenderViewport &viewport)
{
    auto *ctx = VulkanContext::currentContext();
    if (!ctx) {
        return;
    }
    auto *vkRenderer = dynamic_cast<ItemRendererVulkan *>(effects->scene()->renderer());
    if (!vkRenderer) {
        return;
    }
    // Use activeCommandBuffer so the cursor lands in the caller's render pass
    // (e.g. ZoomEffect's offscreen capture during fullscreen zoom) and not the
    // swapchain pass.
    VkCommandBuffer cmd = vkRenderer->activeCommandBuffer(renderTarget);
    if (cmd == VK_NULL_HANDLE) {
        return;
    }

    if (!m_vkCursorTexture) {
        m_vkCursorTexture = VulkanTexture::upload(ctx, m_cursorImage);
        if (!m_vkCursorTexture) {
            return;
        }
    }

    if (!m_vkCursorPipeline) {
        m_vkCursorPipeline = ctx->pipelineManager()->pipeline(
            VulkanShaderTrait::MapTexture | VulkanShaderTrait::Modulate | VulkanShaderTrait::TransformColorspace);
    }
    if (!m_vkCursorPipeline || !m_vkCursorPipeline->isValid()) {
        return;
    }

    const qreal scale = viewport.scale();
    const QSizeF cursorSize = QSizeF(m_cursorImage.size()) / m_cursorImage.devicePixelRatio();
    const QPointF pos = effects->cursorPos() - m_cursorHotspot;
    const float x0 = pos.x() * scale, y0 = pos.y() * scale;
    const float x1 = x0 + cursorSize.width() * scale, y1 = y0 + cursorSize.height() * scale;

    const VulkanVertex2D verts[6] = {
        {{x0, y0}, {0.0f, 0.0f}},
        {{x1, y0}, {1.0f, 0.0f}},
        {{x0, y1}, {0.0f, 1.0f}},
        {{x1, y0}, {1.0f, 0.0f}},
        {{x1, y1}, {1.0f, 1.0f}},
        {{x0, y1}, {0.0f, 1.0f}},
    };
    auto vertBuf = VulkanBuffer::createStreamingBuffer(ctx, sizeof(verts));
    if (!vertBuf) {
        return;
    }
    vertBuf->upload(verts, sizeof(verts));

    VulkanPushConstants pc{};
    memcpy(pc.mvp, viewport.projectionMatrix().constData(), sizeof(pc.mvp));
    pc.textureMatrix[0] = pc.textureMatrix[5] = pc.textureMatrix[10] = pc.textureMatrix[15] = 1.0f;

    const ColorDescription srcColor = ColorDescription::sRGB;
    const ColorDescription dstColor = renderTarget.colorDescription();
    VulkanUniforms uniforms{};
    uniforms.opacity = 1.0f;
    uniforms.brightness = 1.0f;
    uniforms.saturation = 1.0f;
    const QMatrix4x4 colorXform = srcColor.toOther(dstColor, RenderingIntent::Perceptual);
    memcpy(uniforms.colorimetryTransform, colorXform.data(), sizeof(uniforms.colorimetryTransform));
    // SRGB texture is hardware-decoded to linear; use linear TF to avoid double pow(x,2.2)
    uniforms.sourceTransferFunction = TransferFunction::linear;
    uniforms.sourceTransferParams[0] = srcColor.transferFunction().minLuminance;
    uniforms.sourceTransferParams[1] = srcColor.transferFunction().maxLuminance - srcColor.transferFunction().minLuminance;
    uniforms.sourceReferenceLuminance = srcColor.referenceLuminance();
    uniforms.destTransferFunction = dstColor.transferFunction().type;
    uniforms.destTransferParams[0] = dstColor.transferFunction().minLuminance;
    uniforms.destTransferParams[1] = dstColor.transferFunction().maxLuminance - dstColor.transferFunction().minLuminance;
    uniforms.destReferenceLuminance = dstColor.referenceLuminance();
    uniforms.maxDestLuminance = dstColor.maxHdrLuminance().value_or(10000.0f);
    uniforms.maxTonemappingLuminance = dstColor.referenceLuminance();
    const QMatrix4x4 destToLMS = dstColor.containerColorimetry().toLMS();
    const QMatrix4x4 lmsToDest = dstColor.containerColorimetry().fromLMS();
    memcpy(uniforms.destToLMS, destToLMS.data(), sizeof(uniforms.destToLMS));
    memcpy(uniforms.lmsToDest, lmsToDest.data(), sizeof(uniforms.lmsToDest));
    const auto toXYZ = dstColor.containerColorimetry().toXYZ();
    uniforms.primaryBrightness[0] = toXYZ(1, 0);
    uniforms.primaryBrightness[1] = toXYZ(1, 1);
    uniforms.primaryBrightness[2] = toXYZ(1, 2);

    auto uboBuf = VulkanBuffer::createUniformBuffer(ctx, sizeof(VulkanUniforms));
    if (!uboBuf) {
        return;
    }
    uboBuf->upload(&uniforms, sizeof(uniforms));

    VkDescriptorSet ds = ctx->allocateDescriptorSet(m_vkCursorPipeline->descriptorSetLayout());
    if (ds == VK_NULL_HANDLE) {
        return;
    }

    const VkDescriptorImageInfo imgInfo{m_vkCursorTexture->sampler(), m_vkCursorTexture->imageView(),
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const std::array<VkDescriptorImageInfo, 4> imageInfos{imgInfo, imgInfo, imgInfo, imgInfo};
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
    vkUpdateDescriptorSets(ctx->backend()->device(), 2, writes.data(), 0, nullptr);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_vkCursorPipeline->pipeline());
    vkCmdPushConstants(cmd, m_vkCursorPipeline->layout(),
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(VulkanPushConstants), &pc);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_vkCursorPipeline->layout(), 0, 1, &ds, 0, nullptr);
    const VkBuffer vb = vertBuf->buffer();
    const VkDeviceSize vbOffset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &vbOffset);
    vkCmdDraw(cmd, 6, 1, 0, 0);
}
#endif

void InvertEffect::paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport, int mask, const QRegion &region, Output *screen)
{
    effects->paintScreen(renderTarget, viewport, mask, region, screen);

    if (!m_allWindows || !m_valid) {
        return;
    }

    if (m_cursorDirty) {
        rebuildCursorImage();
    }
    if (m_cursorImage.isNull()) {
        return;
    }

#if HAVE_VULKAN
    if (effects->isVulkanCompositing()) {
        paintVulkanCursor(renderTarget, viewport);
        return;
    }
#endif

    // GL path
    if (!m_glCursorTexture) {
        m_glCursorTexture = GLTexture::upload(m_cursorImage);
        if (!m_glCursorTexture) {
            return;
        }
        m_glCursorTexture->setWrapMode(GL_CLAMP_TO_EDGE);
    }
    const qreal scale = viewport.scale();
    const QSizeF cursorSize = QSizeF(m_cursorImage.size()) / m_cursorImage.devicePixelRatio();
    const QPointF pos = effects->cursorPos() - m_cursorHotspot;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    auto *s = GLShaderManager::instance()->pushShader(GLShaderTrait::MapTexture | GLShaderTrait::TransformColorspace);
    s->setColorspaceUniforms(ColorDescription::sRGB, renderTarget.colorDescription(), RenderingIntent::Perceptual);
    QMatrix4x4 mvp = viewport.projectionMatrix();
    mvp.translate(pos.x() * scale, pos.y() * scale);
    s->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, mvp);
    m_glCursorTexture->render(cursorSize * scale);
    GLShaderManager::instance()->popShader();
    glDisable(GL_BLEND);
}

} // namespace

#include "moc_invert.cpp"
