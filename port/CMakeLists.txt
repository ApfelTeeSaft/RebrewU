cmake_minimum_required(VERSION 3.18)
project(GambitPort CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(SDL2 REQUIRED)
find_package(OpenGL REQUIRED)
find_package(CURL REQUIRED)
find_package(ZLIB REQUIRED)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/miniaudio.h")
    message(STATUS "miniaudio.h found — audio back-end enabled (HAVE_MINIAUDIO)")
    set(HAVE_MINIAUDIO ON)
else()
    message(STATUS "miniaudio.h not found — audio output disabled (silent stubs)")
    set(HAVE_MINIAUDIO OFF)
endif()

add_library(gambit_runtime STATIC
    runtime/rebrewu_runtime.cpp
    os/gambit_plt_auto.cpp
    os/coreinit/coreinit_memory.cpp
    os/coreinit/coreinit_threads.cpp
    os/coreinit/coreinit_sync.cpp
    os/coreinit/coreinit_misc.cpp
    os/coreinit/coreinit_fs.cpp
    os/gx2/gx2_context.cpp
    os/gx2/gx2_draw.cpp
    os/gx2/gx2_render.cpp
    os/gx2/r700_to_glsl.cpp
    os/snd_core/snd_core.cpp
    os/vpad/vpad.cpp
    os/padscore/padscore.cpp
    os/stubs/stubs.cpp
    os/nsysnet/nsysnet.cpp
    loader/gambit_loader.cpp
)

target_include_directories(gambit_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime
    ${SDL2_INCLUDE_DIRS}
)

target_compile_definitions(gambit_runtime PUBLIC HAVE_SDL2)

if(HAVE_MINIAUDIO)
    target_compile_definitions(gambit_runtime PUBLIC HAVE_MINIAUDIO)
    target_include_directories(gambit_runtime PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../third_party
    )
endif()

target_link_libraries(gambit_runtime PUBLIC
    ${SDL2_LIBRARIES}
    OpenGL::GL
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

file(GLOB GAMBIT_PARTS "${CMAKE_CURRENT_SOURCE_DIR}/../Gambit/Gambit_part*.cpp")

add_executable(Gambit
    main.cpp
    ${GAMBIT_PARTS}
    ../Gambit/Gambit_data.cpp
    ../Gambit/Gambit_register.cpp
)

target_include_directories(Gambit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime
    ${SDL2_INCLUDE_DIRS}
)

target_link_libraries(Gambit PRIVATE gambit_runtime)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(Gambit PRIVATE -O0 -fno-strict-aliasing)
    target_compile_options(gambit_runtime PRIVATE -O2)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        target_link_options(Gambit PRIVATE -s)
    endif()
endif()

set_target_properties(Gambit PROPERTIES UNITY_BUILD OFF)
