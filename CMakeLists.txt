cmake_minimum_required(VERSION 3.20)

project(RebrewU
    VERSION 0.1.0
    DESCRIPTION "Wii U static recompilation framework"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "Build type not set – defaulting to Release.")
    set(CMAKE_BUILD_TYPE "Release"
        CACHE STRING "Choose the type of build." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

option(REBREWU_BUILD_TESTS  "Build the RebrewU test suite"          ON)
option(REBREWU_ENABLE_ASAN  "Enable AddressSanitizer"               OFF)
option(REBREWU_INSTALL      "Generate CMake install / package rules" OFF)

include(cmake/CompilerOptions.cmake)

find_package(ZLIB REQUIRED)

if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/nlohmann/json.hpp")
    message(STATUS "nlohmann/json: using bundled header "
        "(third_party/nlohmann/json.hpp)")
    add_library(nlohmann_json INTERFACE)
    add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
    target_include_directories(nlohmann_json
        INTERFACE "${CMAKE_SOURCE_DIR}/third_party")
else()
    message(STATUS "nlohmann/json: bundled header absent – fetching v3.11.3 "
        "via FetchContent (will be cached in third_party/nlohmann/)")
    include(FetchContent)
    FetchContent_Declare(
        nlohmann_json_dl
        URL      https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
        URL_HASH SHA256=9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6
        DOWNLOAD_NO_EXTRACT TRUE
        DOWNLOAD_DIR        "${CMAKE_SOURCE_DIR}/third_party/nlohmann"
    )
    FetchContent_MakeAvailable(nlohmann_json_dl)
    add_library(nlohmann_json INTERFACE)
    add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
    target_include_directories(nlohmann_json
        INTERFACE "${CMAKE_SOURCE_DIR}/third_party")
endif()

add_subdirectory(src/ir)            # INTERFACE – headers only; no external deps
add_subdirectory(src/diagnostics)   # lightweight; no external deps
add_subdirectory(src/core)          # elf / rpx / rpl / relocation / linker
add_subdirectory(src/ppc)           # decoder + semantics; links rebrewu_ir
add_subdirectory(src/analysis)      # function_discovery / cfg / jump_table
add_subdirectory(src/codegen)       # cpp_emitter / naming
add_subdirectory(src/config)        # JSON-based config; links nlohmann_json
add_subdirectory(runtime)           # INTERFACE – runtime shim headers

add_subdirectory(src/cli)

if(REBREWU_BUILD_TESTS)
    enable_testing()

    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.7.1
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(Catch2)

    add_subdirectory(tests)
endif()

if(REBREWU_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    install(
        TARGETS
            rebrewu
            rebrewu_core
            rebrewu_ppc
            rebrewu_ir
            rebrewu_analysis
            rebrewu_codegen
            rebrewu_config
            rebrewu_diagnostics
            rebrewu_runtime
        EXPORT RebrewUTargets
        RUNTIME  DESTINATION "${CMAKE_INSTALL_BINDIR}"
        LIBRARY  DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        ARCHIVE  DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    )

    install(DIRECTORY "${CMAKE_SOURCE_DIR}/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    )

    install(EXPORT RebrewUTargets
        FILE        RebrewUTargets.cmake
        NAMESPACE   RebrewU::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/RebrewU"
    )

    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/RebrewUConfigVersion.cmake"
        VERSION       ${PROJECT_VERSION}
        COMPATIBILITY AnyNewerVersion
    )

    configure_package_config_file(
        "${CMAKE_SOURCE_DIR}/cmake/RebrewUConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/RebrewUConfig.cmake"
        INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/RebrewU"
    )

    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/RebrewUConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/RebrewUConfigVersion.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/RebrewU"
    )
endif()
