/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2007 Lubos Lunak <l.lunak@kde.org>
    SPDX-FileCopyrightText: 2010 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "showpaint.h"

#include "config-kwin.h"
#include "core/pixelgrid.h"
#include "core/rendertarget.h"
#include "core/renderviewport.h"
#include "effect/effecthandler.h"
#include "opengl/glutils.h"
#include "scene/workspacescene.h"

#include <QAction>
#include <QPainter>

#if HAVE_VULKAN
#include "platformsupport/scenes/vulkan/vulkanbackend.h"
#include "platformsupport/scenes/vulkan/vulkanbuffer.h"
#include "platformsupport/scenes/vulkan/vulkancontext.h"
#include "platformsupport/scenes/vulkan/vulkanpipeline.h"
#include "platformsupport/scenes/vulkan/vulkanpipelinemanager.h"
#include "platformsupport/scenes/vulkan/vulkantexture.h"
#include "scene/itemrenderer_vulkan.h"
#include <array>
#include <cstring>
#include <vulkan/vulkan.h>
#endif

namespace KWin
{

static const qreal s_alpha = 0.2;
static const QList<QColor> s_colors{
    Qt::red,
    Qt::green,
    Qt::blue,
    Qt::cyan,
    Qt::magenta,
    Qt::yellow,
    Qt::gray};

ShowPaintEffect::ShowPaintEffect() = default;

void ShowPaintEffect::paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport, int mask, const QRegion &region, Output *screen)
{
    m_painted = QRegion();
    effects->paintScreen(renderTarget, viewport, mask, region, screen);
    if (effects->isOpenGLCompositing()) {
        paintGL(renderTarget, viewport.projectionMatrix(), viewport.scale());
    }
#if HAVE_VULKAN
    else if (effects->isVulkanCompositing()) {
        auto *vkRenderer = dynamic_cast<ItemRendererVulkan *>(effects->scene()->renderer());
        if (vkRenderer) {
            paintVulkan(viewport, vkRenderer);
        }
    }
#endif
    if (++m_colorIndex == s_colors.count()) {
        m_colorIndex = 0;
    }
}

void ShowPaintEffect::paintWindow(const RenderTarget &renderTarget, const RenderViewport &viewport, EffectWindow *w, int mask, QRegion region, WindowPaintData &data)
{
    m_painted += region;
    effects->paintWindow(renderTarget, viewport, w, mask, region, data);
}

void ShowPaintEffect::paintGL(const RenderTarget &renderTarget, const QMatrix4x4 &projection, qreal scale)
{
    GLVertexBuffer *vbo = GLVertexBuffer::streamingBuffer();
    vbo->reset();
    ShaderBinder binder(GLShaderTrait::UniformColor | GLShaderTrait::TransformColorspace);
    binder.shader()->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, projection);
    binder.shader()->setColorspaceUniforms(ColorDescription::sRGB, renderTarget.colorDescription(), RenderingIntent::Perceptual);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    QColor color = s_colors[m_colorIndex];
    color.setAlphaF(s_alpha);
    binder.shader()->setUniform(GLShader::ColorUniform::Color, color);
    QList<QVector2D> verts;
    verts.reserve(m_painted.rectCount() * 12);
    for (const QRect &r : m_painted) {
        const auto deviceRect = snapToPixelGridF(scaledRect(r, scale));
        verts.push_back(QVector2D(deviceRect.x() + deviceRect.width(), deviceRect.y()));
        verts.push_back(QVector2D(deviceRect.x(), deviceRect.y()));
        verts.push_back(QVector2D(deviceRect.x(), deviceRect.y() + deviceRect.height()));
        verts.push_back(QVector2D(deviceRect.x(), deviceRect.y() + deviceRect.height()));
        verts.push_back(QVector2D(deviceRect.x() + deviceRect.width(), deviceRect.y() + deviceRect.height()));
        verts.push_back(QVector2D(deviceRect.x() + deviceRect.width(), deviceRect.y()));
    }
    vbo->setVertices(verts);
    vbo->render(GL_TRIANGLES);
    glDisable(GL_BLEND);
}

void ShowPaintEffect::paintQPainter()
{
    QColor color = s_colors[m_colorIndex];
    color.setAlphaF(s_alpha);
    for (const QRect &r : m_painted) {
        effects->scenePainter()->fillRect(r, color);
    }
}

#if HAVE_VULKAN
void ShowPaintEffect::paintVulkan(const RenderViewport &viewport, ItemRendererVulkan *vkRenderer)
{
    VulkanContext *ctx = VulkanContext::currentContext();
    if (!ctx) {
        return;
    }
    VkCommandBuffer cmd = vkRenderer->currentCommandBuffer();
    if (cmd == VK_NULL_HANDLE) {
        return;
    }
    VulkanTexture *whiteTex = vkRenderer->defaultWhiteTexture();
    if (!whiteTex || !whiteTex->isValid()) {
        return;
    }

    VulkanPipeline *pipeline = ctx->pipelineManager()->pipeline(VulkanShaderTrait::UniformColor);
    if (!pipeline || !pipeline->isValid()) {
        return;
    }

    // Collect triangle vertices for all painted rects (screen-space, already scaled)
    const qreal scale = viewport.scale();
    QList<VulkanVertex2D> verts;
    verts.reserve(m_painted.rectCount() * 6);
    for (const QRect &r : m_painted) {
        const QRectF dr = snapToPixelGridF(scaledRect(r, scale));
        const float x0 = dr.x(), y0 = dr.y();
        const float x1 = x0 + dr.width(), y1 = y0 + dr.height();
        verts.push_back({{x1, y0}, {1.0f, 0.0f}});
        verts.push_back({{x0, y0}, {0.0f, 0.0f}});
        verts.push_back({{x0, y1}, {0.0f, 1.0f}});
        verts.push_back({{x0, y1}, {0.0f, 1.0f}});
        verts.push_back({{x1, y1}, {1.0f, 1.0f}});
        verts.push_back({{x1, y0}, {1.0f, 0.0f}});
    }
    if (verts.isEmpty()) {
        return;
    }

    auto vertBuf = VulkanBuffer::createStreamingBuffer(ctx, verts.size() * sizeof(VulkanVertex2D));
    if (!vertBuf) {
        return;
    }
    vertBuf->upload(verts.constData(), verts.size() * sizeof(VulkanVertex2D));

    // MVP = projection only (rects are already in device pixels)
    VulkanPushConstants pc{};
    const QMatrix4x4 mvp = viewport.projectionMatrix();
    memcpy(pc.mvp, mvp.constData(), sizeof(pc.mvp));
    pc.textureMatrix[0] = pc.textureMatrix[5] = pc.textureMatrix[10] = pc.textureMatrix[15] = 1.0f;
    vkCmdPushConstants(cmd, pipeline->layout(),
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(VulkanPushConstants), &pc);

    // UBO: premultiplied RGBA colour (UniformColor trait outputs this directly)
    const QColor c = s_colors[m_colorIndex];
    VulkanUniforms ubo{};
    ubo.uniformColor[0] = c.redF() * s_alpha;
    ubo.uniformColor[1] = c.greenF() * s_alpha;
    ubo.uniformColor[2] = c.blueF() * s_alpha;
    ubo.uniformColor[3] = s_alpha;
    ubo.opacity = 1.0f;
    ubo.brightness = 1.0f;
    ubo.saturation = 1.0f;

    auto uboBuf = VulkanBuffer::createUniformBuffer(ctx, sizeof(VulkanUniforms));
    if (!uboBuf) {
        return;
    }
    uboBuf->upload(&ubo, sizeof(ubo));

    VkDescriptorSet ds = ctx->allocateDescriptorSet(pipeline->descriptorSetLayout());
    if (ds == VK_NULL_HANDLE) {
        return;
    }

    const VkDescriptorImageInfo imgInfo{whiteTex->sampler(), whiteTex->imageView(),
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const std::array<VkDescriptorImageInfo, 4> imageInfos{imgInfo, imgInfo, imgInfo, imgInfo};
    VkDescriptorBufferInfo bufInfo{};
    bufInfo.buffer = uboBuf->buffer();
    bufInfo.offset = 0;
    bufInfo.range = sizeof(VulkanUniforms);

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

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->layout(), 0, 1, &ds, 0, nullptr);

    const VkBuffer vb = vertBuf->buffer();
    const VkDeviceSize vbOffset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &vbOffset);
    vkCmdDraw(cmd, verts.size(), 1, 0, 0);
}
#endif

} // namespace KWin

#include "moc_showpaint.cpp"
