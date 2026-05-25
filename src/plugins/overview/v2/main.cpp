/*
    SPDX-FileCopyrightText: 2026 Ark Royal <awright42mk1@protonmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "overvieweffectv2.h"

namespace KWin
{

KWIN_EFFECT_FACTORY_SUPPORTED(OverviewEffectV2,
                              "metadata.json.stripped",
                              return OverviewEffectV2::supported();)

} // namespace KWin

#include "main.moc"
