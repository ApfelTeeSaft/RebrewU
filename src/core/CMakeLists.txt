add_library(rebrewu_core STATIC
    elf/elf_reader.cpp
    rpx/rpx_loader.cpp
    rpl/rpl_loader.cpp
    relocation/reloc_processor.cpp
    linker/module_graph.cpp
    linker/linker.cpp
)

target_include_directories(rebrewu_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(rebrewu_core
    PUBLIC
        rebrewu_diagnostics
        ZLIB::ZLIB
    PRIVATE
        nlohmann_json
)
