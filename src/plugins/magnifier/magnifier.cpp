/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2007 Lubos Lunak <l.lunak@kde.org>
    SPDX-FileCopyrightText: 2007 Christian Nitschkowski <christian.nitschkowski@kdemail.net>
    SPDX-FileCopyrightText: 2011 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "magnifier.h"
// KConfigSkeleton
#include "magnifierconfig.h"

#include <QAction>

#include <KStandardActions>

#include "config-kwin.h"
#include "core/rendertarget.h"
#include "core/renderviewport.h"
#include "effect/effecthandler.h"
#include "opengl/glutils.h"
#include "opengl/openglcontext.h"
#include <KGlobalAccel>

#if HAVE_VULKAN
#include "compositor.h"
#include "platformsupport/scenes/vulkan/vulkanbackend.h"
#include "platformsupport/scenes/vulkan/vulkanbuffer.h"
#include "platformsupport/scenes/vulkan/vulkancontext.h"
#include "platformsupport/scenes/vulkan/vulkanframebuffer.h"
#include "platformsupport/scenes/vulkan/vulkanpipeline.h"
#include "platformsupport/scenes/vulkan/vulkanpipelinemanager.h"
#include "platformsupport/scenes/vulkan/vulkantexture.h"
#include "scene/itemrenderer_vulkan.h"
#include "scene/workspacescene_vulkan.h"
#include <array>
#include <cstring>
#endif

using namespace std::chrono_literals;

namespace KWin
{

const int FRAME_WIDTH = 5;

MagnifierEffect::MagnifierEffect()
    : m_zoom(1)
    , m_targetZoom(1)
    , m_lastPresentTime(std::chrono::milliseconds::zero())
    , m_texture(nullptr)
    , m_fbo(nullptr)
{
    MagnifierConfig::instance(effects->config());
    QAction *a;
    a = KStandardActions::zoomIn(this, &MagnifierEffect::zoomIn, this);
    KGlobalAccel::self()->setDefaultShortcut(a, QList<QKeySequence>() << (Qt::META | Qt::Key_Plus) << (Qt::META | Qt::Key_Equal));
    KGlobalAccel::self()->setShortcut(a, QList<QKeySequence>() << (Qt::META | Qt::Key_Plus) << (Qt::META | Qt::Key_Equal));

    a = KStandardActions::zoomOut(this, &MagnifierEffect::zoomOut, this);
    KGlobalAccel::self()->setDefaultShortcut(a, QList<QKeySequence>() << (Qt::META | Qt::Key_Minus));
    KGlobalAccel::self()->setShortcut(a, QList<QKeySequence>() << (Qt::META | Qt::Key_Minus));

    a = KStandardActions::actualSize(this, &MagnifierEffect::toggle, this);
    KGlobalAccel::self()->setDefaultShortcut(a, QList<QKeySequence>() << (Qt::META | Qt::Key_0));
    KGlobalAccel::self()->setShortcut(a, QList<QKeySequence>() << (Qt::META | Qt::Key_0));

    connect(effects, &EffectsHandler::mouseChanged, this, &MagnifierEffect::slotMouseChanged);
    connect(effects, &EffectsHandler::windowAdded, this, &MagnifierEffect::slotWindowAdded);

    const auto windows = effects->stackingOrder();
    for (EffectWindow *window : windows) {
        slotWindowAdded(window);
    }

    reconfigure(ReconfigureAll);
}

MagnifierEffect::~MagnifierEffect()
{
    // Save the zoom value.
    MagnifierConfig::setInitialZoom(m_targetZoom);
    MagnifierConfig::self()->save();

#if HAVE_VULKAN
    if (m_vkCtx) {
        m_vkCaptureTexture.reset();
        if (m_vkResumePass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(m_vkCtx->backend()->device(), m_vkResumePass, nullptr);
            m_vkResumePass = VK_NULL_HANDLE;
        }
    }
#endif
}

bool MagnifierEffect::supported()
{
#if HAVE_VULKAN
    if (effects->isVulkanCompositing()) {
        return true;
    }
#endif
    return effects->openglContext() && effects->openglContext()->supportsBlits();
}

void MagnifierEffect::reconfigure(ReconfigureFlags)
{
    MagnifierConfig::self()->read();
    int width, height;
    width = MagnifierConfig::width();
    height = MagnifierConfig::height();
    m_magnifierSize = QSize(width, height);
    // Load the saved zoom value.
    m_zoomFactor = MagnifierConfig::zoomFactor();
    m_targetZoom = MagnifierConfig::initialZoom();
    if (m_targetZoom != m_zoom) {
        toggle();
    }
}

void MagnifierEffect::prePaintScreen(ScreenPrePaintData &data, std::chrono::milliseconds presentTime)
{
    const int time = m_lastPresentTime.count() ? (presentTime - m_lastPresentTime).count() : 0;

    if (m_zoom != m_targetZoom) {
        double diff = time / animationTime(500ms);
        if (m_targetZoom > m_zoom) {
            m_zoom = std::min(m_zoom * std::max(1 + diff, 1.2), m_targetZoom);
        } else {
            m_zoom = std::max(m_zoom * std::min(1 - diff, 0.8), m_targetZoom);
            if (m_zoom == 1.0) {
                // zoom ended - delete FBO and texture
                m_fbo.reset();
                m_texture.reset();
#if HAVE_VULKAN
                m_vkCaptureTexture.reset();
                m_vkCaptureSize = QSize();
#endif
            }
        }
    }

    if (m_zoom != m_targetZoom) {
        m_lastPresentTime = presentTime;
    } else {
        m_lastPresentTime = std::chrono::milliseconds::zero();
    }

    effects->prePaintScreen(data, presentTime);
    if (m_zoom != 1.0) {
        data.paint += magnifierArea().adjusted(-FRAME_WIDTH, -FRAME_WIDTH, FRAME_WIDTH, FRAME_WIDTH);
    }
}

void MagnifierEffect::paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport, int mask, const QRegion &region, Output *screen)
{
    effects->paintScreen(renderTarget, viewport, mask, region, screen); // paint normal screen
    if (m_zoom != 1.0) {
        // get the right area from the current rendered screen
        const QRect area = magnifierArea();
        const QPointF cursor = cursorPos();
        const auto scale = viewport.scale();

        QRectF srcArea(cursor.x() - (double)area.width() / (m_zoom * 2),
                       cursor.y() - (double)area.height() / (m_zoom * 2),
                       (double)area.width() / m_zoom, (double)area.height() / m_zoom);
        if (effects->isOpenGLCompositing()) {
            m_fbo->blitFromRenderTarget(renderTarget, viewport, srcArea.toRect(), QRect(QPoint(), m_fbo->size()));
            // paint magnifier
            auto s = GLShaderManager::instance()->pushShader(GLShaderTrait::MapTexture);
            QMatrix4x4 mvp = viewport.projectionMatrix();
            mvp.translate(area.x() * scale, area.y() * scale);
            s->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, mvp);
            m_texture->render(area.size() * scale);
            GLShaderManager::instance()->popShader();

            GLVertexBuffer *vbo = GLVertexBuffer::streamingBuffer();
            vbo->reset();

            QRectF areaF = scaledRect(area, scale);
            const QRectF frame = scaledRect(area.adjusted(-FRAME_WIDTH, -FRAME_WIDTH, FRAME_WIDTH, FRAME_WIDTH), scale);
            QList<QVector2D> verts;
            verts.reserve(4 * 6 * 2);
            // top frame
            verts.push_back(QVector2D(frame.right(), frame.top()));
            verts.push_back(QVector2D(frame.left(), frame.top()));
            verts.push_back(QVector2D(frame.left(), areaF.top()));
            verts.push_back(QVector2D(frame.left(), areaF.top()));
            verts.push_back(QVector2D(frame.right(), areaF.top()));
            verts.push_back(QVector2D(frame.right(), frame.top()));
            // left frame
            verts.push_back(QVector2D(areaF.left(), frame.top()));
            verts.push_back(QVector2D(frame.left(), frame.top()));
            verts.push_back(QVector2D(frame.left(), frame.bottom()));
            verts.push_back(QVector2D(frame.left(), frame.bottom()));
            verts.push_back(QVector2D(areaF.left(), frame.bottom()));
            verts.push_back(QVector2D(areaF.left(), frame.top()));
            // right frame
            verts.push_back(QVector2D(frame.right(), frame.top()));
            verts.push_back(QVector2D(areaF.right(), frame.top()));
            verts.push_back(QVector2D(areaF.right(), frame.bottom()));
            verts.push_back(QVector2D(areaF.right(), frame.bottom()));
            verts.push_back(QVector2D(frame.right(), frame.bottom()));
            verts.push_back(QVector2D(frame.right(), frame.top()));
            // bottom frame
            verts.push_back(QVector2D(frame.right(), areaF.bottom()));
            verts.push_back(QVector2D(frame.left(), areaF.bottom()));
            verts.push_back(QVector2D(frame.left(), frame.bottom()));
            verts.push_back(QVector2D(frame.left(), frame.bottom()));
            verts.push_back(QVector2D(frame.right(), frame.bottom()));
            verts.push_back(QVector2D(frame.right(), areaF.bottom()));
            vbo->setVertices(verts);

            ShaderBinder binder(GLShaderTrait::UniformColor);
            binder.shader()->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, viewport.projectionMatrix());
            binder.shader()->setUniform(GLShader::ColorUniform::Color, QColor(0, 0, 0));
            vbo->render(GL_TRIANGLES);
        }
#if HAVE_VULKAN
        else if (effects->isVulkanCompositing()) {
            paintVulkan(renderTarget, viewport);
        }
#endif
    }
}

#if HAVE_VULKAN
bool MagnifierEffect::ensureVulkanResumePass(VkFormat swapchainFormat)
{
    if (m_vkResumePass != VK_NULL_HANDLE && m_vkResumePassFormat == swapchainFormat) {
        return true;
    }
    if (m_vkResumePass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(m_vkCtx->backend()->device(), m_vkResumePass, nullptr);
        m_vkResumePass = VK_NULL_HANDLE;
    }

    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = swapchainFormat;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = 1;
    rpInfo.pAttachments = &colorAttachment;
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpass;
    rpInfo.dependencyCount = 1;
    rpInfo.pDependencies = &dependency;

    if (vkCreateRenderPass(m_vkCtx->backend()->device(), &rpInfo, nullptr, &m_vkResumePass) != VK_SUCCESS) {
        m_vkResumePass = VK_NULL_HANDLE;
        return false;
    }
    m_vkResumePassFormat = swapchainFormat;
    return true;
}

void MagnifierEffect::paintVulkan(const RenderTarget &renderTarget, const RenderViewport &viewport)
{
    auto *scene = dynamic_cast<WorkspaceSceneVulkan *>(Compositor::self()->scene());
    if (!scene) {
        return;
    }
    auto *vkRenderer = static_cast<ItemRendererVulkan *>(scene->renderer());
    if (!vkRenderer) {
        return;
    }
    m_vkCtx = vkRenderer->context();
    if (!m_vkCtx) {
        return;
    }

    // FIXME: this also samples currentFramebuffer() — under a recursive paint
    // flow we'd want the caller's framebuffer instead.
    VkCommandBuffer cmd = vkRenderer->activeCommandBuffer(renderTarget);
    VulkanFramebuffer *fb = vkRenderer->currentFramebuffer();
    if (cmd == VK_NULL_HANDLE || !fb) {
        return;
    }
    VkImage swapchainImage = fb->colorImage();
    if (swapchainImage == VK_NULL_HANDLE) {
        return;
    }
    const VkFormat swapchainFormat = m_vkCtx->backend()->colorFormat();
    if (!ensureVulkanResumePass(swapchainFormat)) {
        return;
    }

    // Compute logical/device-pixel rectangles
    const qreal scale = viewport.scale();
    const QRect area = magnifierArea();
    const QPointF cursor = cursorPos();
    const QRectF srcLogical(cursor.x() - (double)area.width() / (m_zoom * 2),
                            cursor.y() - (double)area.height() / (m_zoom * 2),
                            (double)area.width() / m_zoom, (double)area.height() / m_zoom);

    // Device-pixel rectangles
    const QRect dstDev = scaledRect(area, scale).toAlignedRect();
    QRect srcDev = scaledRect(srcLogical, scale).toAlignedRect();

    // Clamp the source area to the framebuffer bounds; vkCmdBlitImage rejects
    // out-of-bounds offsets. If the cursor is near the screen edge the
    // magnification will appear slightly off-center.
    const QSize fbSize(fb->width(), fb->height());
    srcDev = srcDev.intersected(QRect(QPoint(0, 0), fbSize));
    if (srcDev.isEmpty() || dstDev.isEmpty()) {
        return;
    }

    // Allocate/recreate the capture texture when the destination size changes.
    if (!m_vkCaptureTexture || m_vkCaptureSize != dstDev.size()) {
        m_vkCaptureTexture = VulkanTexture::createRenderTarget(m_vkCtx, dstDev.size(), swapchainFormat);
        if (!m_vkCaptureTexture) {
            return;
        }
        m_vkCaptureSize = dstDev.size();
        // First-use transition from UNDEFINED to TRANSFER_DST so the blit can write.
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = m_vkCaptureTexture->image();
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
    } else {
        // Subsequent frames: capture image was left in SHADER_READ_ONLY_OPTIMAL.
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = m_vkCaptureTexture->image();
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
    }

    // End the current render pass so we can do transfer operations on the swapchain.
    vkCmdEndRenderPass(cmd);

    auto imgBarrier = [&](VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                          VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                          VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = oldLayout;
        b.newLayout = newLayout;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask = srcAccess;
        b.dstAccessMask = dstAccess;
        vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
    };

    // Swapchain: PRESENT_SRC → TRANSFER_SRC (capture the area under the lens)
    imgBarrier(swapchainImage,
               VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    // Blit source area from swapchain → capture texture, magnifying via linear filter.
    {
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[0] = {srcDev.x(), srcDev.y(), 0};
        blit.srcOffsets[1] = {srcDev.x() + srcDev.width(), srcDev.y() + srcDev.height(), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[0] = {0, 0, 0};
        blit.dstOffsets[1] = {m_vkCaptureTexture->width(), m_vkCaptureTexture->height(), 1};
        vkCmdBlitImage(cmd,
                       swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       m_vkCaptureTexture->image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit, VK_FILTER_LINEAR);
    }

    // Swapchain: TRANSFER_SRC → COLOR_ATTACHMENT_OPTIMAL for the resume render pass.
    imgBarrier(swapchainImage,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_ACCESS_TRANSFER_READ_BIT,
               VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    // Capture texture: TRANSFER_DST → SHADER_READ_ONLY for sampling.
    imgBarrier(m_vkCaptureTexture->image(),
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    // Resume rendering on the swapchain with LOAD_OP_LOAD so the blit is preserved.
    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass = m_vkResumePass;
    rpBegin.framebuffer = fb->framebuffer();
    rpBegin.renderArea.offset = {0, 0};
    rpBegin.renderArea.extent = {static_cast<uint32_t>(fb->width()), static_cast<uint32_t>(fb->height())};
    vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    // Restore full-framebuffer viewport/scissor (dynamic state persists from prior passes).
    {
        VkViewport vp{};
        vp.x = 0.0f;
        vp.y = static_cast<float>(fb->height());
        vp.width = static_cast<float>(fb->width());
        vp.height = -static_cast<float>(fb->height());
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        VkRect2D sc{};
        sc.extent = {static_cast<uint32_t>(fb->width()), static_cast<uint32_t>(fb->height())};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
    }

    // 1. Draw the magnified content as a textured quad at the lens area.
    {
        VulkanPipeline *texPipeline = m_vkCtx->pipelineManager()->pipeline(
            VulkanShaderTrait::MapTexture | VulkanShaderTrait::Modulate);
        if (!texPipeline || !texPipeline->isValid()) {
            return;
        }

        const float x0 = static_cast<float>(dstDev.x());
        const float y0 = static_cast<float>(dstDev.y());
        const float x1 = x0 + static_cast<float>(dstDev.width());
        const float y1 = y0 + static_cast<float>(dstDev.height());
        const VulkanVertex2D verts[6] = {
            {{x0, y0}, {0.0f, 0.0f}},
            {{x1, y0}, {1.0f, 0.0f}},
            {{x0, y1}, {0.0f, 1.0f}},
            {{x1, y0}, {1.0f, 0.0f}},
            {{x1, y1}, {1.0f, 1.0f}},
            {{x0, y1}, {0.0f, 1.0f}},
        };
        auto vertBuf = VulkanBuffer::createStreamingBuffer(m_vkCtx, sizeof(verts));
        if (!vertBuf) {
            return;
        }
        vertBuf->upload(verts, sizeof(verts));

        VulkanPushConstants pc{};
        const QMatrix4x4 mvp = viewport.projectionMatrix();
        memcpy(pc.mvp, mvp.constData(), sizeof(pc.mvp));
        pc.textureMatrix[0] = pc.textureMatrix[5] = pc.textureMatrix[10] = pc.textureMatrix[15] = 1.0f;

        VulkanUniforms ubo{};
        ubo.opacity = 1.0f;
        ubo.brightness = 1.0f;
        ubo.saturation = 1.0f;

        auto uboBuf = VulkanBuffer::createUniformBuffer(m_vkCtx, sizeof(VulkanUniforms));
        if (!uboBuf) {
            return;
        }
        uboBuf->upload(&ubo, sizeof(ubo));

        const VkDescriptorImageInfo imgInfo{m_vkCaptureTexture->sampler(),
                                            m_vkCaptureTexture->imageView(),
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const std::array<VkDescriptorImageInfo, 4> imageInfos{imgInfo, imgInfo, imgInfo, imgInfo};
        VkDescriptorBufferInfo bufInfo{uboBuf->buffer(), 0, sizeof(VulkanUniforms)};

        std::array<VkWriteDescriptorSet, 2> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 4;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = imageInfos.data();
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[1].pBufferInfo = &bufInfo;

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, texPipeline->pipeline());
        vkCmdPushConstants(cmd, texPipeline->layout(),
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(VulkanPushConstants), &pc);
        if (!m_vkCtx->bindDescriptors(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      texPipeline->layout(), texPipeline->descriptorSetLayout(),
                                      0, writes.size(), writes.data())) {
            return;
        }
        const VkBuffer vb = vertBuf->buffer();
        const VkDeviceSize vbOffset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &vbOffset);
        vkCmdDraw(cmd, 6, 1, 0, 0);
    }

    // 2. Draw the four black frame rectangles around the lens.
    {
        VulkanPipeline *colorPipeline = m_vkCtx->pipelineManager()->pipeline(VulkanShaderTrait::UniformColor);
        VulkanTexture *whiteTex = vkRenderer->defaultWhiteTexture();
        if (!colorPipeline || !colorPipeline->isValid() || !whiteTex || !whiteTex->isValid()) {
            return;
        }

        const QRectF areaF = scaledRect(area, scale);
        const QRectF frame = scaledRect(area.adjusted(-FRAME_WIDTH, -FRAME_WIDTH, FRAME_WIDTH, FRAME_WIDTH), scale);
        const float fL = static_cast<float>(frame.left());
        const float fR = static_cast<float>(frame.right());
        const float fT = static_cast<float>(frame.top());
        const float fB = static_cast<float>(frame.bottom());
        const float aL = static_cast<float>(areaF.left());
        const float aR = static_cast<float>(areaF.right());
        const float aT = static_cast<float>(areaF.top());
        const float aB = static_cast<float>(areaF.bottom());

        auto rectVerts = [](std::vector<VulkanVertex2D> &v, float x0, float y0, float x1, float y1) {
            v.push_back({{x1, y0}, {1.0f, 0.0f}});
            v.push_back({{x0, y0}, {0.0f, 0.0f}});
            v.push_back({{x0, y1}, {0.0f, 1.0f}});
            v.push_back({{x0, y1}, {0.0f, 1.0f}});
            v.push_back({{x1, y1}, {1.0f, 1.0f}});
            v.push_back({{x1, y0}, {1.0f, 0.0f}});
        };

        std::vector<VulkanVertex2D> verts;
        verts.reserve(24);
        rectVerts(verts, fL, fT, fR, aT); // top strip
        rectVerts(verts, fL, aT, aL, aB); // left strip
        rectVerts(verts, aR, aT, fR, aB); // right strip
        rectVerts(verts, fL, aB, fR, fB); // bottom strip

        const VkDeviceSize vertBytes = verts.size() * sizeof(VulkanVertex2D);
        auto vertBuf = VulkanBuffer::createStreamingBuffer(m_vkCtx, vertBytes);
        if (!vertBuf) {
            return;
        }
        vertBuf->upload(verts.data(), vertBytes);

        VulkanPushConstants pc{};
        const QMatrix4x4 mvp = viewport.projectionMatrix();
        memcpy(pc.mvp, mvp.constData(), sizeof(pc.mvp));
        pc.textureMatrix[0] = pc.textureMatrix[5] = pc.textureMatrix[10] = pc.textureMatrix[15] = 1.0f;

        VulkanUniforms ubo{};
        // Opaque black, premultiplied (RGB=0, A=1).
        ubo.uniformColor[0] = 0.0f;
        ubo.uniformColor[1] = 0.0f;
        ubo.uniformColor[2] = 0.0f;
        ubo.uniformColor[3] = 1.0f;
        ubo.opacity = 1.0f;
        ubo.brightness = 1.0f;
        ubo.saturation = 1.0f;

        auto uboBuf = VulkanBuffer::createUniformBuffer(m_vkCtx, sizeof(VulkanUniforms));
        if (!uboBuf) {
            return;
        }
        uboBuf->upload(&ubo, sizeof(ubo));

        const VkDescriptorImageInfo imgInfo{whiteTex->sampler(), whiteTex->imageView(),
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const std::array<VkDescriptorImageInfo, 4> imageInfos{imgInfo, imgInfo, imgInfo, imgInfo};
        VkDescriptorBufferInfo bufInfo{uboBuf->buffer(), 0, sizeof(VulkanUniforms)};

        std::array<VkWriteDescriptorSet, 2> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 4;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = imageInfos.data();
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[1].pBufferInfo = &bufInfo;

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, colorPipeline->pipeline());
        vkCmdPushConstants(cmd, colorPipeline->layout(),
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(VulkanPushConstants), &pc);
        if (!m_vkCtx->bindDescriptors(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      colorPipeline->layout(), colorPipeline->descriptorSetLayout(),
                                      0, writes.size(), writes.data())) {
            return;
        }
        const VkBuffer vb = vertBuf->buffer();
        const VkDeviceSize vbOffset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &vbOffset);
        vkCmdDraw(cmd, static_cast<uint32_t>(verts.size()), 1, 0, 0);
    }
}
#endif

void MagnifierEffect::postPaintScreen()
{
    if (m_zoom != m_targetZoom) {
        QRect framedarea = magnifierArea().adjusted(-FRAME_WIDTH, -FRAME_WIDTH, FRAME_WIDTH, FRAME_WIDTH);
        effects->addRepaint(framedarea);
    }
    effects->postPaintScreen();
}

QRect MagnifierEffect::magnifierArea(QPointF pos) const
{
    return QRect(pos.x() - m_magnifierSize.width() / 2, pos.y() - m_magnifierSize.height() / 2,
                 m_magnifierSize.width(), m_magnifierSize.height());
}

void MagnifierEffect::zoomIn()
{
    m_targetZoom *= m_zoomFactor;
    if (effects->isOpenGLCompositing() && !m_texture) {
        effects->makeOpenGLContextCurrent();
        m_texture = GLTexture::allocate(GL_RGBA16F, m_magnifierSize);
        if (!m_texture) {
            return;
        }
        m_texture->setContentTransform(OutputTransform());
        m_fbo = std::make_unique<GLFramebuffer>(m_texture.get());
    }
    // For Vulkan, the capture texture is created lazily in paintVulkan().
    effects->addRepaint(magnifierArea().adjusted(-FRAME_WIDTH, -FRAME_WIDTH, FRAME_WIDTH, FRAME_WIDTH));
}

void MagnifierEffect::zoomOut()
{
    m_targetZoom /= m_zoomFactor;
    if (m_targetZoom <= 1) {
        m_targetZoom = 1;
        if (m_zoom == m_targetZoom) {
            if (effects->isOpenGLCompositing()) {
                effects->makeOpenGLContextCurrent();
            }
            m_fbo.reset();
            m_texture.reset();
#if HAVE_VULKAN
            m_vkCaptureTexture.reset();
            m_vkCaptureSize = QSize();
#endif
        }
    }
    effects->addRepaint(magnifierArea().adjusted(-FRAME_WIDTH, -FRAME_WIDTH, FRAME_WIDTH, FRAME_WIDTH));
}

void MagnifierEffect::toggle()
{
    if (m_zoom == 1.0) {
        if (m_targetZoom == 1.0) {
            m_targetZoom = 2;
        }
        if (effects->isOpenGLCompositing() && !m_texture) {
            effects->makeOpenGLContextCurrent();
            m_texture = GLTexture::allocate(GL_RGBA16F, m_magnifierSize);
            if (!m_texture) {
                return;
            }
            m_texture->setContentTransform(OutputTransform());
            m_fbo = std::make_unique<GLFramebuffer>(m_texture.get());
        }
        // For Vulkan, the capture texture is created lazily in paintVulkan().
    } else {
        m_targetZoom = 1;
    }
    effects->addRepaint(magnifierArea().adjusted(-FRAME_WIDTH, -FRAME_WIDTH, FRAME_WIDTH, FRAME_WIDTH));
}

void MagnifierEffect::slotMouseChanged(const QPointF &pos, const QPointF &old,
                                       Qt::MouseButtons, Qt::MouseButtons, Qt::KeyboardModifiers, Qt::KeyboardModifiers)
{
    if (pos != old && m_zoom != 1) {
        // need full repaint as we might lose some change events on fast mouse movements
        // see Bug 187658
        effects->addRepaintFull();
    }
}

void MagnifierEffect::slotWindowAdded(EffectWindow *w)
{
    connect(w, &EffectWindow::windowDamaged, this, &MagnifierEffect::slotWindowDamaged);
}

void MagnifierEffect::slotWindowDamaged()
{
    if (isActive()) {
        effects->addRepaint(magnifierArea());
    }
}

bool MagnifierEffect::isActive() const
{
    return m_zoom != 1.0 || m_zoom != m_targetZoom;
}

QSize MagnifierEffect::magnifierSize() const
{
    return m_magnifierSize;
}

qreal MagnifierEffect::targetZoom() const
{
    return m_targetZoom;
}

} // namespace

#include "moc_magnifier.cpp"
