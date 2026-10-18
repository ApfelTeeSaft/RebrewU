add_executable(rebrewu
    main.cpp
    cli_options.cpp
    audit.cpp
)

target_include_directories(rebrewu
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(rebrewu
    PRIVATE
        rebrewu_core
        rebrewu_ppc
        rebrewu_ir
        rebrewu_analysis
        rebrewu_codegen
        nlohmann_json
        rebrewu_config
        rebrewu_diagnostics
)

install(TARGETS rebrewu RUNTIME DESTINATION bin)
