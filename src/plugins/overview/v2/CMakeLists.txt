# SPDX-FileCopyrightText: 2026 Ark Royal <awright42mk1@protonmail.com>
#
# SPDX-License-Identifier: BSD-3-Clause

# OverviewEffectV2: the C++ replacement for the QML overview effect.
# Lives in this subdirectory so the kwin_add_builtin_effect macro
# strips its own metadata.json (which differs from the V1 plugin's
# name + plugin ID). Sources reference the parent dir for the
# OverviewEffectV2 implementation — keeping the class file alongside
# its V1 sibling makes cross-referencing easier during the multi-phase
# rewrite (see the design plan).
set(overviewv2_SOURCES
    main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../overvieweffectv2.cpp
)

kwin_add_builtin_effect(overviewv2 ${overviewv2_SOURCES})

# Parent dir for the OverviewEffectV2 header.
target_include_directories(overviewv2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(overviewv2 PRIVATE
    kwin

    KF6::ConfigGui
    KF6::GlobalAccel
    KF6::I18n
)
