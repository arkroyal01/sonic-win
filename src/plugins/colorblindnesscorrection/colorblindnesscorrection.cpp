/*
    SPDX-FileCopyrightText: 2023 Fushan Wen <qydwhotmail@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "colorblindnesscorrection.h"

#include "config-kwin.h"
#include "core/colorspace.h"
#include "core/rendertarget.h"
#include "core/renderviewport.h"
#include "cursor.h"
#include "cursorsource.h"
#include "effect/effecthandler.h"
#include "opengl/glshader.h"
#include "opengl/gltexture.h"
#include "opengl/glutils.h"

#include "colorblindnesscorrectionconfig.h"

#include <algorithm>
#include <cmath>

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

Q_LOGGING_CATEGORY(KWIN_COLORBLINDNESS_CORRECTION, "kwin_effect_colorblindnesscorrection", QtWarningMsg)

static float srgbToLinear(float c)
{
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
static float linearToSrgb(float c)
{
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

static void ensureResources()
{
    // Must initialize resources manually because the effect is a static lib.
    Q_INIT_RESOURCE(colorblindnesscorrection);
}

namespace KWin
{

ColorBlindnessCorrectionEffect::ColorBlindnessCorrectionEffect()
    : OffscreenEffect()
{
    ColorBlindnessCorrectionSettings::instance(effects->config());
    m_mode = static_cast<Mode>(ColorBlindnessCorrectionSettings::mode());
    m_intensity = std::clamp<float>(ColorBlindnessCorrectionSettings::intensity(), 0.0f, 1.0f);

    loadData();

    effects->hideCursor();
    connect(Cursors::self()->mouse(), &Cursor::cursorChanged, this, [this]() {
        m_cursorDirty = true;
        effects->addRepaintFull();
    });
}

ColorBlindnessCorrectionEffect::~ColorBlindnessCorrectionEffect()
{
    effects->showCursor();
}

bool ColorBlindnessCorrectionEffect::supported()
{
    return effects->isOpenGLCompositing() || effects->isVulkanCompositing();
}

void ColorBlindnessCorrectionEffect::loadData()
{
    ensureResources();

    if (m_mode == Greyscale) {
#if HAVE_VULKAN
        if (effects->isVulkanCompositing()) {
            auto *scene = dynamic_cast<WorkspaceSceneVulkan *>(Compositor::self()->scene());
            auto *renderer = scene ? static_cast<ItemRendererVulkan *>(scene->renderer()) : nullptr;
            auto *ctx = renderer ? renderer->context() : nullptr;
            if (ctx && ctx->pipelineManager()) {
                // Reuse the standard pipeline; setPipeline(..., 1.0f, 0.0f) will lock saturation=0.
                m_vkGreyscalePipeline = ctx->pipelineManager()->pipeline(
                    VulkanShaderTrait::MapTexture | VulkanShaderTrait::Modulate | VulkanShaderTrait::AdjustSaturation | VulkanShaderTrait::TransformColorspace);
            }
        }
#endif
        // For GL greyscale: no custom shader; apply() sets saturation=0 on the default path.
    } else {
        QMatrix3x3 defectMatrix;
        switch (m_mode) {
        case Deuteranopia:
            defectMatrix(0, 0) = 1.0;
            defectMatrix(1, 0) = 0.494207;
            defectMatrix(2, 0) = 0.0;
            defectMatrix(0, 1) = 0.0;
            defectMatrix(1, 1) = 0.0;
            defectMatrix(2, 1) = 0.0;
            defectMatrix(0, 2) = 0.0;
            defectMatrix(1, 2) = 1.24827;
            defectMatrix(2, 2) = 1.0;
            break;
        case Tritanopia:
            defectMatrix(0, 0) = 1.0;
            defectMatrix(1, 0) = 0.0;
            defectMatrix(2, 0) = -0.395913;
            defectMatrix(0, 1) = 0.0;
            defectMatrix(1, 1) = 1.0;
            defectMatrix(2, 1) = 0.801109;
            defectMatrix(0, 2) = 0.0;
            defectMatrix(1, 2) = 0.0;
            defectMatrix(2, 2) = 0.0;
            break;
        case Protanopia:
        default:
            defectMatrix(0, 0) = 0.0;
            defectMatrix(1, 0) = 0.0;
            defectMatrix(2, 0) = 0.0;
            defectMatrix(0, 1) = 2.02344;
            defectMatrix(1, 1) = 1.0;
            defectMatrix(2, 1) = 0.0;
            defectMatrix(0, 2) = -2.52581;
            defectMatrix(1, 2) = 0.0;
            defectMatrix(2, 2) = 1.0;
            break;
        }

        if (effects->isOpenGLCompositing()) {
            m_shader = GLShaderManager::instance()->generateShaderFromFile(GLShaderTrait::MapTexture, QString(), QStringLiteral(":/effects/colorblindnesscorrection/shaders/colorblindnesscorrection.frag"));

            if (!m_shader->isValid()) {
                qCCritical(KWIN_COLORBLINDNESS_CORRECTION) << "Failed to load the shader!";
                return;
            }

            ShaderBinder binder{m_shader.get()};
            m_shader->setUniform("intensity", m_intensity);
            m_shader->setUniform("defectMatrix", defectMatrix);
        }
#if HAVE_VULKAN
        else if (effects->isVulkanCompositing()) {
            auto *scene = dynamic_cast<WorkspaceSceneVulkan *>(Compositor::self()->scene());
            auto *renderer = scene ? static_cast<ItemRendererVulkan *>(scene->renderer()) : nullptr;
            auto *ctx = renderer ? renderer->context() : nullptr;
            if (ctx && ctx->pipelineManager()) {
                m_vkPipeline = ctx->pipelineManager()->pipeline(
                    VulkanShaderTrait::MapTexture | VulkanShaderTrait::Modulate | VulkanShaderTrait::ColorBlindnessCorrect);
            }
            // Store the defect matrix in std140 column-major layout (3 columns × 4 floats each)
            for (int col = 0; col < 3; ++col) {
                m_defectMatrix[col * 4 + 0] = defectMatrix(0, col);
                m_defectMatrix[col * 4 + 1] = defectMatrix(1, col);
                m_defectMatrix[col * 4 + 2] = defectMatrix(2, col);
                m_defectMatrix[col * 4 + 3] = 0.0f; // std140 padding
            }
        }
#endif
    }

    for (const auto windows = effects->stackingOrder(); EffectWindow *w : windows) {
        correctColor(w);
    }
    effects->addRepaintFull();

    connect(effects, &EffectsHandler::windowDeleted, this, &ColorBlindnessCorrectionEffect::slotWindowDeleted);
    connect(effects, &EffectsHandler::windowAdded, this, &ColorBlindnessCorrectionEffect::correctColor);
}

void ColorBlindnessCorrectionEffect::correctColor(KWin::EffectWindow *w)
{
    if (m_windows.contains(w)) {
        return;
    }

    redirect(w);

    if (effects->isOpenGLCompositing()) {
        if (m_mode != Greyscale) {
            setShader(w, m_shader.get());
        }
        // For GL greyscale: no custom shader; apply() sets saturation=0 on the default path.
    }
#if HAVE_VULKAN
    else if (effects->isVulkanCompositing()) {
        if (m_mode == Greyscale && m_vkGreyscalePipeline) {
            setPipeline(w, m_vkGreyscalePipeline, 1.0f, 0.0f);
        } else if (m_vkPipeline) {
            setPipeline(w, m_vkPipeline);
            setColorBlindnessParams(w, m_defectMatrix, m_intensity);
        }
    }
#endif

    m_windows.insert(w);
}

void ColorBlindnessCorrectionEffect::slotWindowDeleted(EffectWindow *w)
{
    if (auto it = m_windows.find(w); it != m_windows.end()) {
        m_windows.erase(it);
    }
}

bool ColorBlindnessCorrectionEffect::isActive() const
{
    return !m_windows.empty();
}

bool ColorBlindnessCorrectionEffect::provides(Feature f)
{
    return f == Contrast;
}

void ColorBlindnessCorrectionEffect::reconfigure(ReconfigureFlags flags)
{
    if (flags != Effect::ReconfigureAll) {
        return;
    }

    ColorBlindnessCorrectionSettings::self()->read();
    const auto newMode = static_cast<Mode>(ColorBlindnessCorrectionSettings::mode());
    const auto newIntensity = std::clamp<float>(ColorBlindnessCorrectionSettings::intensity(), 0.0f, 1.0f);
    if (m_mode == newMode && qFuzzyCompare(m_intensity, newIntensity)) {
        return;
    }

    m_mode = newMode;
    m_intensity = newIntensity;

    disconnect(effects, &EffectsHandler::windowDeleted, this, &ColorBlindnessCorrectionEffect::slotWindowDeleted);
    disconnect(effects, &EffectsHandler::windowAdded, this, &ColorBlindnessCorrectionEffect::correctColor);

    for (EffectWindow *w : m_windows) {
        unredirect(w);
    }
    m_windows.clear();

    loadData();

    m_cursorDirty = true;
    m_glCursorTexture.reset();
    m_vkCursorTexture.reset();
    effects->addRepaintFull();
}

int ColorBlindnessCorrectionEffect::requestedEffectChainPosition() const
{
    return 98;
}

void ColorBlindnessCorrectionEffect::apply(EffectWindow *window, int mask, WindowPaintData &data, WindowQuadList &quads)
{
    Q_UNUSED(window)
    Q_UNUSED(mask)
    Q_UNUSED(quads)
    if (m_mode == Greyscale) {
        data.setSaturation(0.0);
    }
}

void ColorBlindnessCorrectionEffect::rebuildCursorImage()
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

    QImage img = src->image().convertToFormat(QImage::Format_ARGB32_Premultiplied);
    m_cursorHotspot = src->hotspot().toPoint();

    for (int y = 0; y < img.height(); ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            const QRgb px = line[x];
            const int a = qAlpha(px);
            if (a == 0) {
                continue;
            }
            const float fa = a / 255.0f;
            float r = srgbToLinear(qRed(px) / (255.0f * fa));
            float g = srgbToLinear(qGreen(px) / (255.0f * fa));
            float b = srgbToLinear(qBlue(px) / (255.0f * fa));

            if (m_mode == Greyscale) {
                const float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                r = g = b = lum;
            } else {
                // srgbToLMS (GLSL column-major)
                const float L = 17.8824f * r + 43.5161f * g + 4.11935f * b;
                const float M = 3.45565f * r + 27.1554f * g + 3.86714f * b;
                const float S = 0.0299566f * r + 0.184309f * g + 1.46709f * b;
                // defectMatrix (std140 col-major: defect[row][col] = m_defectMatrix[col*4+row])
                const float lL = m_defectMatrix[0] * L + m_defectMatrix[4] * M + m_defectMatrix[8] * S;
                const float lM = m_defectMatrix[1] * L + m_defectMatrix[5] * M + m_defectMatrix[9] * S;
                const float lS = m_defectMatrix[2] * L + m_defectMatrix[6] * M + m_defectMatrix[10] * S;
                // errorMat (GLSL column-major)
                const float eR = 0.0809444479f * lL - 0.130504409f * lM + 0.116721066f * lS;
                const float eG = -0.0102485335f * lL + 0.0540193266f * lM - 0.113614708f * lS;
                const float eB = -0.000365296938f * lL - 0.00412161469f * lM + 0.693511405f * lS;
                // correction
                const float dR = (r - eR) * m_intensity;
                const float dG = (g - eG) * m_intensity;
                const float dB = (b - eB) * m_intensity;
                g += dR * 0.7f + dG;
                b += dR * 0.7f + dB;
            }

            line[x] = qRgba(
                static_cast<int>(std::clamp(linearToSrgb(r) * fa * 255.0f, 0.0f, 255.0f)),
                static_cast<int>(std::clamp(linearToSrgb(g) * fa * 255.0f, 0.0f, 255.0f)),
                static_cast<int>(std::clamp(linearToSrgb(b) * fa * 255.0f, 0.0f, 255.0f)),
                a);
        }
    }
    m_cursorImage = img;
}

#if HAVE_VULKAN
void ColorBlindnessCorrectionEffect::paintVulkanCursor(const RenderTarget &renderTarget, const RenderViewport &viewport)
{
    auto *ctx = VulkanContext::currentContext();
    if (!ctx) {
        return;
    }
    auto *vkRenderer = dynamic_cast<ItemRendererVulkan *>(effects->scene()->renderer());
    if (!vkRenderer) {
        return;
    }
    VkCommandBuffer cmd = vkRenderer->currentCommandBuffer();
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

void ColorBlindnessCorrectionEffect::paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport, int mask, const QRegion &region, Output *screen)
{
    effects->paintScreen(renderTarget, viewport, mask, region, screen);

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

#include "moc_colorblindnesscorrection.cpp"
