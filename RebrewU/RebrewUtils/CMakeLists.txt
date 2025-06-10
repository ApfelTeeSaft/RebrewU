cmake_minimum_required(VERSION 3.20)

project(RebrewUtils LANGUAGES CXX)

add_library(RebrewUtils STATIC
    file.h
    file.cpp
    disasm.h
    disasm.cpp
    rpx_image.h
    rpx_image.cpp
    wiiu_ppc.h
    ppc_context.h
)

target_compile_features(RebrewUtils PUBLIC cxx_std_20)

target_include_directories(RebrewUtils 
    PUBLIC 
        ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

find_package(ZLIB REQUIRED)
target_link_libraries(RebrewUtils 
    PUBLIC
        ZLIB::ZLIB
)

if(WIN32)
    target_compile_definitions(RebrewUtils PRIVATE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(RebrewUtils PRIVATE
        -Wall -Wextra -Wno-unused-parameter
    )
elseif(MSVC)
    target_compile_options(RebrewUtils PRIVATE
        /W4 /permissive-
    )
endif()

set_target_properties(RebrewUtils PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
)