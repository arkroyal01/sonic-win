/*
    SPDX-FileCopyrightText: 2026 Ark Royal <awright42mk1@protonmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "effect/effect.h"

#include <QVariantAnimation>

namespace KWin
{

/**
 * @brief Rewrite of OverviewEffect that doesn't use Qt Quick.
 *
 * The existing OverviewEffect inherits from QuickSceneEffect; its UI
 * lives in `qml/main.qml` etc. and Qt's QML/Quick stack holds VRAM for
 * per-thumbnail QSGTextures, layer FBOs, glyph atlases, and scene-graph
 * batches in a per-QQuickWindow pool kwin cannot directly free. Across
 * a session, those allocations accumulate to ~750 MB per
 * [[project_kwin_frame_drop_drift_backlog]].
 *
 * OverviewEffectV2 paints everything via kwin's compositor renderer
 * (`ItemRendererVulkan`) and allocates all GPU resources through VMA.
 * Trade: re-implement layout, animation, input handling, and (later
 * phases) text rendering and drag-drop manually.
 *
 * Phase 1 (this file): subclass + minimal lifecycle. Activates a
 * translucent overlay with a slide-in alpha animation; deactivates on
 * Esc. No window thumbnails yet (phase 2) and no plugin registration
 * yet (phase 1b — caller currently can only construct this class
 * directly, not load it as a kwin effect).
 *
 * See the design plan for the
 * multi-phase plan.
 */
class OverviewEffectV2 : public Effect
{
    Q_OBJECT

public:
    OverviewEffectV2();
    ~OverviewEffectV2() override;

    /// Activate the effect: starts the slide-in animation.
    void activate();
    /// Deactivate: animates slide-out then releases per-activation state.
    void deactivate();

    // Effect API
    bool isActive() const override;
    int requestedEffectChainPosition() const override;
    void prePaintScreen(ScreenPrePaintData &data,
                        std::chrono::milliseconds presentTime) override;
    void paintScreen(const RenderTarget &renderTarget,
                     const RenderViewport &viewport, int mask,
                     const QRegion &region, Output *screen) override;
    void postPaintScreen() override;
    void grabbedKeyboardEvent(QKeyEvent *event) override;

private:
    /// State machine for the slide-in/out animation. Drives
    /// `m_activationFactor` (0 = hidden, 1 = fully shown) over
    /// `m_animationDuration` ms. We use QVariantAnimation rather than
    /// QML's `NumberAnimation` because the entire point of V2 is to
    /// not depend on the Qt Quick infrastructure.
    QVariantAnimation m_animation;
    qreal m_activationFactor = 0.0;
    int m_animationDuration = 400;

    /// True while the effect is in the active phase (animating in,
    /// fully shown, or animating out). Cleared at the end of the
    /// slide-out animation; until then, paintScreen draws.
    bool m_visible = false;
};

} // namespace KWin
