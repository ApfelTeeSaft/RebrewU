add_library(rebrewu_codegen STATIC
    cpp_emitter.cpp
    naming.cpp
)

target_include_directories(rebrewu_codegen
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(rebrewu_codegen
    PUBLIC
        rebrewu_ir
        rebrewu_analysis
        rebrewu_diagnostics
)
