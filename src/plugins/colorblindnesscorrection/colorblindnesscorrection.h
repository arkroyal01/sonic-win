/*
    SPDX-FileCopyrightText: 2023 Fushan Wen <qydwhotmail@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <unordered_set>

#include "config-kwin.h"
#include "effect/offscreeneffect.h"
#include "opengl/glshadermanager.h"

#include <QImage>
#include <QPoint>

#if HAVE_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace KWin
{

class GLTexture;
#if HAVE_VULKAN
class VulkanBuffer;
class VulkanPipeline;
class VulkanTexture;
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
    void paintScreen(const RenderTarget &, const RenderViewport &, int, const QRegion &, Output *) override;

    static bool supported();

public Q_SLOTS:
    void slotWindowDeleted(KWin::EffectWindow *w);

private Q_SLOTS:
    void correctColor(KWin::EffectWindow *w);

private:
    void loadData();
    void rebuildCursorImage();
#if HAVE_VULKAN
    void paintVulkanCursor(const RenderTarget &, const RenderViewport &);
    // Registers/unregisters the fullscreen color-correction post-pass with the
    // Vulkan renderer. Whole-screen always, so it can wrap overview and other
    // QuickSceneEffect overlays which terminate the effect chain.
    void updateVulkanPostPass();
    // Fullscreen post-pass callback: applies the colorblindness correction (or
    // greyscale) to the captured scene and draws the corrected cursor on top.
    void colorBlindnessVulkanPostPass(VkCommandBuffer cmd, VulkanTexture *sceneCapture,
                                      const RenderTarget &, const RenderViewport &);
#endif

    Mode m_mode = Protanopia;
    float m_intensity = 1.0f;

    std::unordered_set<KWin::EffectWindow *> m_windows;
    std::unique_ptr<GLShader> m_shader;

    QImage m_cursorImage;
    QPoint m_cursorHotspot;
    bool m_cursorDirty = true;
    std::unique_ptr<GLTexture> m_glCursorTexture;
#if HAVE_VULKAN
    VulkanPipeline *m_vkPipeline = nullptr;
    VulkanPipeline *m_vkGreyscalePipeline = nullptr;
    VulkanPipeline *m_vkCursorPipeline = nullptr;
    float m_defectMatrix[12] = {};
    std::unique_ptr<VulkanTexture> m_vkCursorTexture;
    // Fullscreen color-correction post-pass registration id (0 = not registered).
    int m_vkPostPassId = 0;
#endif
};

} // namespace
