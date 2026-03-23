list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

function(add_rebrewu_test name)
    add_executable(${name} ${name}.cpp)
    target_include_directories(${name}
        PRIVATE
            ${CMAKE_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/include
    )
    target_link_libraries(${name}
        PRIVATE
            Catch2::Catch2WithMain
            rebrewu_core
            rebrewu_ppc
            rebrewu_ir
            rebrewu_analysis
            rebrewu_codegen
            rebrewu_config
            rebrewu_diagnostics
    )
    catch_discover_tests(${name})
endfunction()

add_rebrewu_test(test_elf_parse)
add_rebrewu_test(test_ppc_decode)
add_rebrewu_test(test_ir)
add_rebrewu_test(test_analysis)
add_rebrewu_test(test_codegen)
