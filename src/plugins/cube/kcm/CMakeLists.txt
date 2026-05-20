set(kwin_cube_config_SOURCES cubeeffectkcm.cpp)
ki18n_wrap_ui(kwin_cube_config_SOURCES cubeeffectkcm.ui)

kwin_add_effect_config(kwin_cube_config ${kwin_cube_config_SOURCES})
target_sources(kwin_cube_config PRIVATE resources.qrc)
target_link_libraries(kwin_cube_config
    KF6::ConfigCore
    KF6::ConfigGui
    KF6::ConfigWidgets
    KF6::CoreAddons
    KF6::GlobalAccel
    KF6::I18n
    KF6::KCMUtils
    KF6::XmlGui
)
