/*
    SPDX-FileCopyrightText: 2021 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "overvieweffect.h"

namespace KWin
{

// Mutually exclusive with the C++ rewrite (see ./v2/main.cpp): when
// the user opts into OverviewEffectV2 via KWIN_OVERVIEW_V2=1, this
// QML-backed plugin refuses to load so both effects don't fight over
// the same global shortcut.
KWIN_EFFECT_FACTORY_SUPPORTED(OverviewEffect,
                              "metadata.json.stripped",
                              return qEnvironmentVariableIntValue("KWIN_OVERVIEW_V2") == 0
                              && OverviewEffect::supported();)

} // namespace KWin

#include "main.moc"
