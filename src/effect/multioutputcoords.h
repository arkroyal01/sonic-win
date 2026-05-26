/*
    SPDX-FileCopyrightText: 2026 Ark Royal <awright42mk1@protonmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QPoint>
#include <QRect>

namespace KWin
{

class Output;

/**
 * @brief Coordinate helpers for multi-monitor V2 effects.
 *
 * V2 effects keep mouse positions in `effects->virtualScreenGeometry()`
 * space — the global desktop's union rect. For per-output rendering
 * those positions need to be partitioned (which output is the cursor
 * on?) and translated (where on that output's framebuffer is the
 * cursor?). These free functions provide the conversions both
 * OverviewEffectV2 and CubeEffectV2 use; no shared state, no
 * subclassing.
 *
 * See `multioutputstate.h` for the per-output state container the
 * same effects pair these utilities with.
 */
class OutputCoords
{
public:
    /**
     * @brief Output whose geometry contains @p globalPos.
     *
     * Walks `effects->screens()` in order and returns the first
     * output whose `geometry().contains(globalPos)` is true. Returns
     * nullptr when the cursor sits between outputs (gap in the
     * arrangement) or outside any output — the effect typically
     * falls back to "no input dispatched this frame" in that case.
     */
    static Output *outputAt(const QPoint &globalPos);

    /**
     * @brief Translate global pos to output-local (origin at the
     * output's top-left).
     *
     * Returns the input unchanged when @p output is null so call
     * sites can use the result of `outputAt(pos)` directly:
     *
     * @code
     *   const QPoint local = OutputCoords::toOutputLocal(
     *       OutputCoords::outputAt(globalPos), globalPos);
     * @endcode
     */
    static QPoint toOutputLocal(const Output *output, const QPoint &globalPos);

    /**
     * @brief Translate output-local pos to global.
     *
     * Inverse of `toOutputLocal`. Returns the input unchanged when
     * @p output is null.
     */
    static QPoint toGlobal(const Output *output, const QPoint &outputLocalPos);

    /**
     * @brief Output covered by @p globalRect with the largest
     * intersection.
     *
     * For determining which output a fullscreen UI element is
     * "primarily on" when it spans multiple outputs — e.g. a
     * keyboard-focused tile during a drag. Returns nullptr if no
     * output overlaps the rect at all.
     */
    static Output *primaryOutputFor(const QRect &globalRect);
};

} // namespace KWin
