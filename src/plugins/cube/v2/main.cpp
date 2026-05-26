/*
    SPDX-FileCopyrightText: 2026 Ark Royal <awright42mk1@protonmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "cubeeffectv2.h"

namespace KWin
{

KWIN_EFFECT_FACTORY_SUPPORTED(CubeEffectV2,
                              "metadata.json.stripped",
                              return CubeEffectV2::supported();)

} // namespace KWin

#include "main.moc"
