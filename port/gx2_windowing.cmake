# Require the audited static window/input library profile.
find_package(SDL2 CONFIG REQUIRED)
if(NOT TARGET SDL2::SDL2-static OR NOT EXISTS "${SDL2_DIR}/SDL2VulkanOnly.cmake")
    message(FATAL_ERROR "Build the Vulkan dependency profile with tools/build_vulkan_dependencies.py and set SDL2_DIR to its cmake directory")
endif()
set(SDL2_LIBRARIES SDL2::SDL2-static)
