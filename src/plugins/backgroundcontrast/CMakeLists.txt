#######################################
# Effect

# Source files
set(contrast_SOURCES
    contrast.cpp
    contrastshader.cpp
    main.cpp
)
kwin_add_builtin_effect(contrast ${contrast_SOURCES})
target_link_libraries(contrast PRIVATE kwin)

find_package(Vulkan QUIET)
if(Vulkan_FOUND)
    target_link_libraries(contrast PRIVATE Vulkan::Vulkan)
endif()
