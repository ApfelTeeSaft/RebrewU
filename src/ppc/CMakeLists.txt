add_library(rebrewu_ppc STATIC
    decoder/ppc_decode.cpp
    semantics/ppc_semantics.cpp
)

target_include_directories(rebrewu_ppc
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(rebrewu_ppc
    PUBLIC
        rebrewu_ir
        rebrewu_diagnostics
)
