cmake_minimum_required(VERSION 3.18)
project(GambitPort CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(${CMAKE_CURRENT_SOURCE_DIR}/gx2_windowing.cmake)
if(DEFINED RBREW_GRAPHICS_BACKEND AND NOT RBREW_GRAPHICS_BACKEND STREQUAL "Vulkan")
    message(FATAL_ERROR "Gambit requires the Vulkan backend. No alternate graphics backend is available.")
endif()
include(${CMAKE_CURRENT_SOURCE_DIR}/gx2_graphics_backend.cmake)
find_package(CURL REQUIRED)
find_package(ZLIB REQUIRED)
add_subdirectory(third_party/cemu_shader)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/miniaudio.h")
    message(STATUS "miniaudio.h found — audio back-end enabled (HAVE_MINIAUDIO)")
    set(HAVE_MINIAUDIO ON)
else()
    message(STATUS "miniaudio.h not found — audio output disabled (silent stubs)")
    set(HAVE_MINIAUDIO OFF)
endif()

add_library(gambit_runtime STATIC
    runtime/rebrewu_runtime.cpp
    runtime/rebrewu_paired.cpp
    os/gambit_plt_auto.cpp
    os/coreinit/coreinit_memory.cpp
    os/coreinit/coreinit_threads.cpp
    os/coreinit/coreinit_sync.cpp
    os/coreinit/coreinit_misc.cpp
    os/coreinit/coreinit_shared_data.cpp
    os/coreinit/coreinit_dynload.cpp
    os/coreinit/coreinit_overlay.cpp
    os/coreinit/coreinit_system_settings.cpp
    os/coreinit/coreinit_user_config.cpp
    os/erreula/erreula.cpp
    os/nn_act/nn_act.cpp
    os/sysapp/sysapp_title.cpp
    os/coreinit/coreinit_fs.cpp
    os/gx2/gx2_context.cpp
    os/gx2/gx2_draw.cpp
    os/gx2/gx2_surface_layout.cpp
    os/gx2/r700_to_glsl.cpp
    os/gx2/gx2_shader_program.cpp
    os/gx2/gx2_pm4_state.cpp
    os/gx2/gx2_display_list.cpp
    os/gx2/gx2_detile.cpp
    os/gx2/gx2_write_watch.cpp
    os/gx2/gx2_fetch_decode.cpp
    os/snd_core/snd_core.cpp
    os/vpad/vpad.cpp
    os/padscore/padscore.cpp
    os/stubs/stubs.cpp
    os/nsysnet/nsysnet.cpp
    loader/gambit_loader.cpp
    loader/native_console_profile.cpp
)

target_include_directories(gambit_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime
    ${SDL2_INCLUDE_DIRS}
)

target_compile_definitions(gambit_runtime PUBLIC HAVE_SDL2)
# Gameplay builds contain no console/file diagnostics or profiler hooks.
# RBREW_DIAGNOSTIC_RUNTIME keeps them for a separately linked, one-off
# diagnostic copy (capture/trace); never for the delivered game.
option(RBREW_DIAGNOSTIC_RUNTIME "Keep diagnostics in the runtime for a one-off diagnostic copy" OFF)
if(NOT RBREW_DIAGNOSTIC_RUNTIME)
    target_compile_definitions(gambit_runtime PRIVATE RBREW_DISABLE_LOGGING)
    target_compile_definitions(cemu_shader PRIVATE RBREW_DISABLE_LOGGING)
    set_property(SOURCE main.cpp APPEND PROPERTY COMPILE_DEFINITIONS RBREW_DISABLE_LOGGING)
endif()
rbrew_gx2_backend(gambit_runtime PUBLIC)

if(HAVE_MINIAUDIO)
    target_compile_definitions(gambit_runtime PUBLIC HAVE_MINIAUDIO)
    target_include_directories(gambit_runtime PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../third_party
    )
endif()

target_link_libraries(gambit_runtime PUBLIC
    cemu_shader
    ${SDL2_LIBRARIES}

    CURL::libcurl
    ZLIB::ZLIB
)

if(WIN32)
    target_link_libraries(gambit_runtime PUBLIC ws2_32 winmm)
    if(HAVE_MINIAUDIO)
        target_link_libraries(gambit_runtime PUBLIC ole32 ksuser)
    endif()
endif()

if(UNIX AND NOT APPLE AND HAVE_MINIAUDIO)
    find_library(ALSA_LIB asound)
    if(ALSA_LIB)
        target_link_libraries(gambit_runtime PUBLIC ${ALSA_LIB})
    endif()
    find_library(PTHREAD_LIB pthread)
    if(PTHREAD_LIB)
        target_link_libraries(gambit_runtime PUBLIC ${PTHREAD_LIB})
    endif()
endif()

set(GAMBIT_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../Gambit" CACHE PATH
    "Directory containing RebrewU-generated Gambit C++")
set(RBREW_GAME_DEFAULT OFF)
if(EXISTS "${GAMBIT_GENERATED_DIR}/Gambit_data.cpp")
    set(RBREW_GAME_DEFAULT ON)
endif()
option(RBREW_BUILD_GAME "Build a private, locally generated game" ${RBREW_GAME_DEFAULT})
if(RBREW_BUILD_GAME)
file(GLOB GAMBIT_PARTS "${GAMBIT_GENERATED_DIR}/Gambit_part*.cpp")

add_executable(Gambit
    main.cpp
    ${GAMBIT_PARTS}
    ${GAMBIT_GENERATED_DIR}/Gambit_data.cpp
    ${GAMBIT_GENERATED_DIR}/Gambit_register.cpp
)

target_include_directories(Gambit PRIVATE
    ${GAMBIT_GENERATED_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime
    ${SDL2_INCLUDE_DIRS}
)

target_link_libraries(Gambit PRIVATE gambit_runtime)
set_target_properties(Gambit PROPERTIES OUTPUT_NAME Splatoon)
endif()
if(WIN32)
    enable_language(RC)
    if(TARGET Gambit)
        target_sources(Gambit PRIVATE resources/splatoon.rc)
        target_include_directories(Gambit PRIVATE resources)
        if(MSVC)
            # splatoon.rc already embeds the application manifest as ID 1.
            # An automatically generated manifest would duplicate that resource.
            target_link_options(Gambit PRIVATE /MANIFEST:NO)
        endif()
    endif()
    add_library(splport_host SHARED EXCLUDE_FROM_ALL main.cpp runtime/splport_host.def)
    target_compile_definitions(splport_host PRIVATE RBREW_DISTRIBUTABLE_RUNTIME RBREW_DISABLE_LOGGING)
    target_link_libraries(splport_host PRIVATE gambit_runtime)
    if(MSVC)
        target_compile_options(splport_host PRIVATE /utf-8)
    endif()
    add_executable(splport_check_runtime EXCLUDE_FROM_ALL tools/check_prerequisites.cpp)
    target_link_libraries(splport_check_runtime PRIVATE Vulkan::Headers volk::volk)
endif()
include(${CMAKE_CURRENT_SOURCE_DIR}/runtime/host_process.cmake)
if(TARGET Gambit)
    rbrew_windowed_executable(Gambit)
    if(NOT RBREW_DIAGNOSTIC_RUNTIME)
        target_compile_definitions(Gambit PRIVATE RBREW_DISABLE_LOGGING)
    endif()
endif()

set(RBREW_TITLE_METADATA "" CACHE FILEPATH "Owned title meta.xml used for native MCP region profile")
if(RBREW_TITLE_METADATA)
    if(NOT EXISTS "${RBREW_TITLE_METADATA}")
        message(FATAL_ERROR "Configured RBREW_TITLE_METADATA does not exist")
    endif()
    set_source_files_properties(main.cpp PROPERTIES
        COMPILE_DEFINITIONS RBREW_TITLE_METADATA_PATH="${RBREW_TITLE_METADATA}")
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    if(TARGET Gambit)
        target_compile_options(Gambit PRIVATE "$<$<CONFIG:Debug>:-O0>" -fno-strict-aliasing)
    endif()
    target_compile_options(gambit_runtime PRIVATE -O2)
    if(TARGET Gambit AND CMAKE_BUILD_TYPE STREQUAL "Release")
        target_link_options(Gambit PRIVATE -s)
    endif()
endif()

if(TARGET Gambit)
    set_target_properties(Gambit PROPERTIES UNITY_BUILD OFF)
endif()

if(MSVC)
    # Keep large generated functions unoptimized in diagnostic builds, but
    # leave Release free to use its configuration's /O2 settings.
    if(TARGET Gambit)
        target_compile_options(Gambit PRIVATE "$<$<CONFIG:Debug>:/Od>" /bigobj /MP4)
    endif()
    target_compile_options(gambit_runtime PRIVATE /utf-8)
endif()

option(RBREW_ENABLE_COVERAGE "Record original guest execution to RBREW_COVERAGE_FILE" OFF)
if(RBREW_ENABLE_COVERAGE)
    target_compile_definitions(gambit_runtime PUBLIC RBREW_ENABLE_COVERAGE)
endif()

option(RBREW_ENABLE_PPC_FALLBACK "Enable conservative hybrid prototype (runtime opt-in too)" OFF)
if(RBREW_ENABLE_PPC_FALLBACK)
    if(NOT TARGET Gambit)
        message(FATAL_ERROR "Hybrid diagnostics require a private generated game")
    endif()
    if(NOT EXISTS "${GAMBIT_GENERATED_DIR}/Gambit_ppc_image.cpp" OR
       NOT EXISTS "${GAMBIT_GENERATED_DIR}/Gambit_ppc_image.bin")
        message(FATAL_ERROR "Hybrid prototype requires regeneration with --emit-ppc-image")
    endif()
    target_sources(gambit_runtime PRIVATE runtime/rebrewu_ppc_interpreter.cpp
        runtime/rebrewu_ppc_hybrid.cpp)
    target_sources(Gambit PRIVATE "${GAMBIT_GENERATED_DIR}/Gambit_ppc_image.cpp")
    target_compile_definitions(gambit_runtime PUBLIC RBREW_ENABLE_PPC_FALLBACK)
    target_compile_definitions(Gambit PRIVATE GAMBIT_PPC_IMAGE_PATH="${GAMBIT_GENERATED_DIR}/Gambit_ppc_image.bin")
endif()

# Offline validation of the actual silent gameplay runtime; no guest code is linked.
# The developer test sources are not part of the public tree.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../tests/gx2_replay.cpp")
    add_executable(gx2_replay_quiet EXCLUDE_FROM_ALL ../tests/gx2_replay.cpp)
    target_include_directories(gx2_replay_quiet PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/os/gx2)
    target_link_libraries(gx2_replay_quiet PRIVATE gambit_runtime)
    target_compile_definitions(gx2_replay_quiet PRIVATE RBREW_DISABLE_LOGGING)
endif()
# The cache-wide release /MAP names Gambit.map; give this tool its own map so
# building it never replaces the game's map (the last /MAP option wins).
if(MSVC AND TARGET gx2_replay_quiet)
    target_link_options(gx2_replay_quiet PRIVATE "/MAP:$<TARGET_FILE_DIR:gx2_replay_quiet>/gx2_replay_quiet.map")
endif()
