add_library(rebrewu_runtime INTERFACE)

target_include_directories(rebrewu_runtime
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
