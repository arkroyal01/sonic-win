/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2007 Rivo Laks <rivolaks@hot.ee>
    SPDX-FileCopyrightText: 2008 Lucas Murray <lmurray@undefinedfire.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "config-kwin.h"
#include "effect/offscreeneffect.h"

#include <QImage>
#include <QPoint>

namespace KWin
{

class GLShader;
class GLTexture;
#if HAVE_VULKAN
class VulkanBuffer;
class VulkanPipeline;
class VulkanTexture;
#endif

/**
 * Inverts desktop's colors
 */
class InvertEffect : public OffscreenEffect
{
    Q_OBJECT
public:
    InvertEffect();
    ~InvertEffect() override;

    bool isActive() const override;
    bool provides(Feature) override;
    int requestedEffectChainPosition() const override;
    void paintScreen(const RenderTarget &, const RenderViewport &, int, const QRegion &, Output *) override;

    static bool supported();

public Q_SLOTS:
    void toggleScreenInversion();
    void toggleWindow();

    void slotWindowAdded(KWin::EffectWindow *w);
    void slotWindowClosed(KWin::EffectWindow *w);

protected:
    bool loadData();

private:
    bool isInvertable(EffectWindow *window) const;
    void invert(EffectWindow *window);
    void uninvert(EffectWindow *window);
    void rebuildCursorImage();
#if HAVE_VULKAN
    void paintVulkanCursor(const RenderTarget &, const RenderViewport &);
#endif

    bool m_inited;
    bool m_valid;
    std::unique_ptr<GLShader> m_shader;
    bool m_allWindows;
    QList<EffectWindow *> m_windows;

    // Software cursor (hardware cursor hidden when m_allWindows is true)
    QImage m_cursorImage;
    QPoint m_cursorHotspot;
    bool m_cursorDirty = true;
    std::unique_ptr<GLTexture> m_glCursorTexture;
#if HAVE_VULKAN
    VulkanPipeline *m_vkPipeline = nullptr;
    VulkanPipeline *m_vkCursorPipeline = nullptr;
    std::unique_ptr<VulkanTexture> m_vkCursorTexture;
#endif
};

inline int InvertEffect::requestedEffectChainPosition() const
{
    return 99;
}

} // namespace
