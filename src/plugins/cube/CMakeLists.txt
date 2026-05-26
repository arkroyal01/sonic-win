add_subdirectory(kcm)
kwin_add_scripted_effect(cube package)

# C++ cube rewrite, opt-in via KWIN_CUBE_V2=1. Builds a sibling
# builtin plugin (cubev2) that takes over the same Meta+C shortcut
# when the env var is set. See ./v2/CMakeLists.txt and
# cubeeffectv2.h for the multi-phase plan. Mirrors the overview/v2
# pattern.
add_subdirectory(v2)
