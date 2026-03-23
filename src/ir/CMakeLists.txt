add_library(rebrewu_ir STATIC
    ir_builder.cpp
)

target_include_directories(rebrewu_ir
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(rebrewu_ir
    PUBLIC
        rebrewu_diagnostics
)
