cmake_minimum_required(VERSION 3.20)

project(RebrewRecomp LANGUAGES CXX)

add_executable(RebrewRecomp
    main.cpp
    pch.h
    pch.cpp
    recompiler.cpp
    recompiler.h
    recompiler_config.cpp
    recompiler_config.h
    test_recompiler.cpp
    test_recompiler.h
)

target_compile_features(RebrewRecomp PUBLIC cxx_std_20)

target_precompile_headers(RebrewRecomp PRIVATE pch.h)

target_include_directories(RebrewRecomp 
    PRIVATE 
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/RebrewUtils
        ${CMAKE_SOURCE_DIR}/RebrewAnalyse
)

target_link_libraries(RebrewRecomp 
    PRIVATE 
        LibRebrewAnalyse
        RebrewUtils
        fmt::fmt
        tomlplusplus::tomlplusplus
)

find_path(XXHASH_INCLUDE_DIR xxhash.h)
find_library(XXHASH_LIBRARY xxhash)

if(XXHASH_INCLUDE_DIR AND XXHASH_LIBRARY)
    target_include_directories(RebrewRecomp PRIVATE ${XXHASH_INCLUDE_DIR})
    target_link_libraries(RebrewRecomp PRIVATE ${XXHASH_LIBRARY})
else()
    target_link_libraries(RebrewRecomp PRIVATE xxHash::xxhash)
endif()

if(WIN32)
    target_compile_definitions(RebrewRecomp PRIVATE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(RebrewRecomp PRIVATE
        -Wall -Wextra 
        -Wno-switch 
        -Wno-unused-variable 
        -Wno-null-arithmetic
    )
    
    if(NOT APPLE)
        target_compile_definitions(RebrewRecomp PRIVATE REBREW_RECOMP_USE_ALIAS)
    endif()
elseif(MSVC)
    target_compile_options(RebrewRecomp PRIVATE
        /W4 /permissive-
        /wd4065
        /wd4101
    )
endif()

option(REBREW_EMBED_CONFIG_PATH "Embed configuration file path" OFF)
option(REBREW_EMBED_HEADER_PATH "Embed header file path" OFF)

if(REBREW_EMBED_CONFIG_PATH)
    target_compile_definitions(RebrewRecomp PRIVATE 
        REBREW_RECOMP_CONFIG_FILE_PATH="${REBREW_EMBED_CONFIG_PATH}")
endif()

if(REBREW_EMBED_HEADER_PATH)
    target_compile_definitions(RebrewRecomp PRIVATE 
        REBREW_RECOMP_HEADER_FILE_PATH="${REBREW_EMBED_HEADER_PATH}")
endif()

set_target_properties(RebrewRecomp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

configure_file(
    ${CMAKE_SOURCE_DIR}/examples/sample_config.toml
    ${CMAKE_BINARY_DIR}/bin/sample_config.toml
    COPYONLY
)