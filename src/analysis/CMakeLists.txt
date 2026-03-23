add_library(rebrewu_analysis STATIC
    function_discovery.cpp
    cfg_builder.cpp
    jump_table.cpp
)

target_include_directories(rebrewu_analysis
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(rebrewu_analysis
    PUBLIC
        rebrewu_core
        rebrewu_ppc
        rebrewu_ir
)
