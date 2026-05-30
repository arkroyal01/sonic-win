/*
    SPDX-FileCopyrightText: 2021 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "overvieweffect.h"

#include "effect/effecthandler.h"

namespace KWin
{

// Mutually exclusive with the C++ rewrite (see ./v2/main.cpp): OverviewEffectV2
// takes over only when it actually loads — i.e. KWIN_OVERVIEW_V2 != 0 AND the
// Vulkan backend is active (V2 is Vulkan-only). This QML-backed plugin loads
// otherwise: when the user opts out (KWIN_OVERVIEW_V2=0) or on the OpenGL
// backend, so the GL path keeps a working overview without both effects
// fighting over Meta+W.
KWIN_EFFECT_FACTORY_SUPPORTED(OverviewEffect,
                              "metadata.json.stripped",
                              return (qEnvironmentVariable("KWIN_OVERVIEW_V2", QStringLiteral("1")).toInt() == 0
                                      || !(effects && effects->isVulkanCompositing()))
                              && OverviewEffect::supported();)

} // namespace KWin

#include "main.moc"
