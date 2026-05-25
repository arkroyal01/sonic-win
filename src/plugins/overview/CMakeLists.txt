# SPDX-FileCopyrightText: 2021 Vlad Zahorodnii <vlad.zahorodnii@kde.org>
#
# SPDX-License-Identifier: BSD-3-Clause

if (KWIN_BUILD_KCMS)
    add_subdirectory(kcm)
endif()

set(overview_SOURCES
    main.cpp
    overvieweffect.cpp
)

kconfig_add_kcfg_files(overview_SOURCES
    overviewconfig.kcfgc
)

kwin_add_builtin_effect(overview ${overview_SOURCES})

qt6_add_qml_module(overview
    NO_PLUGIN
    URI overview
    RESOURCE_PREFIX /
    QML_FILES
        qml/DesktopBar.qml
        qml/DesktopView.qml
        qml/main.qml
)

target_link_libraries(overview PRIVATE
    kwin

    KF6::ConfigGui
    KF6::GlobalAccel
    KF6::I18n

    Qt::Quick
)

# C++ overview rewrite, opt-in via KWIN_OVERVIEW_V2=1. Builds a sibling
# plugin (overviewv2) that takes over the same shortcut when the env var
# is set. See ./v2/CMakeLists.txt and the volcanic-branch plan.
add_subdirectory(v2)
