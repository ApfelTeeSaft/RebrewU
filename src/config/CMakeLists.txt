add_library(rebrewu_config STATIC
    config.cpp
)

target_include_directories(rebrewu_config
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(rebrewu_config
    PRIVATE
        nlohmann_json
    PUBLIC
        rebrewu_diagnostics
)
