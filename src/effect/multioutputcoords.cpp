/*
    SPDX-FileCopyrightText: 2026 Ark Royal <awright42mk1@protonmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "effect/multioutputcoords.h"

#include "core/output.h"
#include "effect/effecthandler.h"

namespace KWin
{

Output *OutputCoords::outputAt(const QPoint &globalPos)
{
    if (!effects) {
        return nullptr;
    }
    for (Output *output : effects->screens()) {
        if (output && output->geometry().contains(globalPos)) {
            return output;
        }
    }
    return nullptr;
}

QPoint OutputCoords::toOutputLocal(const Output *output, const QPoint &globalPos)
{
    if (!output) {
        return globalPos;
    }
    return globalPos - output->geometry().topLeft();
}

QPoint OutputCoords::toGlobal(const Output *output, const QPoint &outputLocalPos)
{
    if (!output) {
        return outputLocalPos;
    }
    return outputLocalPos + output->geometry().topLeft();
}

Output *OutputCoords::primaryOutputFor(const QRect &globalRect)
{
    if (!effects || globalRect.isEmpty()) {
        return nullptr;
    }
    Output *best = nullptr;
    int bestArea = 0;
    for (Output *output : effects->screens()) {
        if (!output) {
            continue;
        }
        const QRect overlap = output->geometry().intersected(globalRect);
        const int area = overlap.width() * overlap.height();
        if (area > bestArea) {
            bestArea = area;
            best = output;
        }
    }
    return best;
}

} // namespace KWin
