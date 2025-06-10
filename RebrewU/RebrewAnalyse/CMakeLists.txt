cmake_minimum_required(VERSION 3.20)

project(RebrewAnalyse LANGUAGES CXX)

add_executable(RebrewAnalyse
    main.cpp
    function.cpp
    function.h
    rpx_analyzer.cpp
    rpx_analyzer.h
)

add_library(LibRebrewAnalyse STATIC
    function.cpp
    function.h
    rpx_analyzer.cpp
    rpx_analyzer.h
)

target_compile_features(RebrewAnalyse PUBLIC cxx_std_20)
target_compile_features(LibRebrewAnalyse PUBLIC cxx_std_20)

target_include_directories(RebrewAnalyse 
    PRIVATE 
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/RebrewUtils
)

target_include_directories(LibRebrewAnalyse 
    PUBLIC 
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/RebrewUtils
)

target_link_libraries(RebrewAnalyse 
    PRIVATE 
        RebrewUtils
        fmt::fmt
)

target_link_libraries(LibRebrewAnalyse 
    PUBLIC 
        RebrewUtils
)

if(WIN32)
    target_compile_definitions(RebrewAnalyse PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
    target_compile_definitions(LibRebrewAnalyse PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(RebrewAnalyse PRIVATE
        -Wall -Wextra -Wno-unused-parameter
    )
    target_compile_options(LibRebrewAnalyse PRIVATE
        -Wall -Wextra -Wno-unused-parameter
    )
elseif(MSVC)
    target_compile_options(RebrewAnalyse PRIVATE
        /W4 /permissive-
    )
    target_compile_options(LibRebrewAnalyse PRIVATE
        /W4 /permissive-
    )
endif()

set_target_properties(RebrewAnalyse PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

set_target_properties(LibRebrewAnalyse PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
)