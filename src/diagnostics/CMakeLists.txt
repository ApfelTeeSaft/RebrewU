add_library(rebrewu_diagnostics STATIC
    diagnostics.cpp
)

target_include_directories(rebrewu_diagnostics
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
