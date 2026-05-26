# SPDX-FileCopyrightText: 2026 Ark Royal <awright42mk1@protonmail.com>
#
# SPDX-License-Identifier: BSD-3-Clause

# CubeEffectV2: the C++ rewrite of the QML cube effect. Lives in its
# own subdirectory so kwin_add_builtin_effect picks up its metadata.json
# (the V1 cube package metadata.json is scripted-effect shaped and
# differs from a builtin's). Sources reference the parent directory
# for the CubeEffectV2 implementation; keeping the class alongside
# the V1 plugin makes cross-referencing easier through the multi-phase
# rewrite (mirroring overview/v2/).
set(cubev2_SOURCES
    main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../cubeeffectv2.cpp
)

kwin_add_builtin_effect(cubev2 ${cubev2_SOURCES})

target_include_directories(cubev2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(cubev2 PRIVATE
    kwin

    KF6::ConfigGui
    KF6::GlobalAccel
    KF6::I18n
)
