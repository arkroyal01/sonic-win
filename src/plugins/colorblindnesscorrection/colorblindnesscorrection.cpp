/*
    SPDX-FileCopyrightText: 2023 Fushan Wen <qydwhotmail@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "colorblindnesscorrection.h"

#include "config-kwin.h"
#include "effect/effecthandler.h"
#include "opengl/glshader.h"

#include "colorblindnesscorrectionconfig.h"

#if HAVE_VULKAN
#include "platformsupport/scenes/vulkan/vulkancontext.h"
#include "platformsupport/scenes/vulkan/vulkanpipeline.h"
#include "platformsupport/scenes/vulkan/vulkanpipelinemanager.h"
#endif

Q_LOGGING_CATEGORY(KWIN_COLORBLINDNESS_CORRECTION, "kwin_effect_colorblindnesscorrection", QtWarningMsg)

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
}

ColorBlindnessCorrectionEffect::~ColorBlindnessCorrectionEffect()
{
}

bool ColorBlindnessCorrectionEffect::supported()
{
    return effects->isOpenGLCompositing() || effects->isVulkanCompositing();
}

void ColorBlindnessCorrectionEffect::loadData()
{
    ensureResources();

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
    case Protanopia: // Most common, use it as fallback
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
        auto *ctx = VulkanContext::currentContext();
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
        setShader(w, m_shader.get());
    }
#if HAVE_VULKAN
    else if (effects->isVulkanCompositing() && m_vkPipeline) {
        setPipeline(w, m_vkPipeline);
        setColorBlindnessParams(w, m_defectMatrix, m_intensity);
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
}

int ColorBlindnessCorrectionEffect::requestedEffectChainPosition() const
{
    return 98;
}

} // namespace

#include "moc_colorblindnesscorrection.cpp"
