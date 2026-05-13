/*
    SPDX-FileCopyrightText: 2023 Fushan Wen <qydwhotmail@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <unordered_set>

#include "config-kwin.h"
#include "effect/offscreeneffect.h"
#include "opengl/glshadermanager.h"

namespace KWin
{

#if HAVE_VULKAN
class VulkanPipeline;
#endif

/**
 * The color filter supports protanopia, deuteranopia and tritanopia.
 */
class ColorBlindnessCorrectionEffect : public OffscreenEffect
{
    Q_OBJECT

public:
    enum Mode {
        Protanopia = 0, //<Greatly reduced reds
        Deuteranopia, //<Greatly reduced greens
        Tritanopia, //<Greatly reduced blues
        Greyscale, //<Desaturate to greyscale
    };

    explicit ColorBlindnessCorrectionEffect();
    ~ColorBlindnessCorrectionEffect() override;

    bool isActive() const override;
    bool provides(Feature) override;
    void reconfigure(ReconfigureFlags flags) override;
    int requestedEffectChainPosition() const override;
    void apply(EffectWindow *window, int mask, WindowPaintData &data, WindowQuadList &quads) override;

    static bool supported();

public Q_SLOTS:
    void slotWindowDeleted(KWin::EffectWindow *w);

private Q_SLOTS:
    void correctColor(KWin::EffectWindow *w);

private:
    void loadData();

    Mode m_mode = Protanopia;
    float m_intensity = 1.0f;

    std::unordered_set<KWin::EffectWindow *> m_windows;
    std::unique_ptr<GLShader> m_shader;
#if HAVE_VULKAN
    VulkanPipeline *m_vkPipeline = nullptr; // ColorBlindnessCorrect pipeline
    VulkanPipeline *m_vkGreyscalePipeline = nullptr; // Default pipeline with saturation=0
    float m_defectMatrix[12] = {}; // std140: 3 columns × 4 floats (3 data + 1 pad each)
#endif
};

} // namespace
