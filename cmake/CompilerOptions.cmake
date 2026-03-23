include_guard(GLOBAL)

function(rebrewu_set_compiler_options target)

    set_target_properties(${target} PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    set_target_properties(${target} PROPERTIES
        POSITION_INDEPENDENT_CODE ON
    )

    if(MSVC)
        # ── MSVC ──────────────────────────────────────────────────────────────
        target_compile_options(${target} PRIVATE
            /W4             # High diagnostic level (one below /Wall)
            /WX             # Treat warnings as errors
            /permissive-    # Strict ISO C++ conformance
            /Zc:__cplusplus # Report correct __cplusplus value
            /utf-8          # Source and execution charset = UTF-8
            /MP             # Parallel compilation (multi-processor)
            $<$<CONFIG:Debug>:/Od>
            $<$<CONFIG:Debug>:/Zi>
            $<$<CONFIG:Debug>:/RTC1>
            $<$<CONFIG:Release>:/O2>
            $<$<CONFIG:Release>:/GL>
            $<$<CONFIG:RelWithDebInfo>:/O2>
            $<$<CONFIG:RelWithDebInfo>:/Zi>
        )
        target_compile_definitions(${target} PRIVATE
            _CRT_SECURE_NO_WARNINGS
            NOMINMAX
            WIN32_LEAN_AND_MEAN
        )
        target_link_options(${target} PRIVATE
            $<$<CONFIG:Release>:/LTCG>
        )
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wcast-align
            -Wformat=2
            -Wno-unused-parameter
            -Wno-sign-compare
            $<$<CONFIG:Debug>:-g>
            $<$<CONFIG:Debug>:-O0>
            $<$<CONFIG:Debug>:-fno-omit-frame-pointer>
            $<$<CONFIG:Release>:-O3>
            $<$<CONFIG:Release>:-DNDEBUG>
            $<$<CONFIG:RelWithDebInfo>:-O2>
            $<$<CONFIG:RelWithDebInfo>:-g>
            $<$<CONFIG:RelWithDebInfo>:-DNDEBUG>
        )

        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(${target} PRIVATE
                -Wno-unused-private-field
            )
        endif()

        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(${target} PRIVATE
                -Wno-maybe-uninitialized
            )
        endif()
    endif()

    if(REBREWU_ENABLE_ASAN)
        if(MSVC)
            target_compile_options(${target} PRIVATE /fsanitize=address)
        else()
            target_compile_options(${target} PRIVATE
                -fsanitize=address
                -fno-omit-frame-pointer
            )
            target_link_options(${target} PRIVATE -fsanitize=address)
        endif()
    endif()

endfunction()

if(MSVC)
    add_compile_options(
        /W4
        /WX-            # Global: warn but don't error; tightened per target
        /permissive-
        /Zc:__cplusplus
        /utf-8
        /MP
        $<$<CONFIG:Debug>:/Od>
        $<$<CONFIG:Debug>:/Zi>
        $<$<CONFIG:Release>:/O2>
        $<$<CONFIG:RelWithDebInfo>:/O2>
        $<$<CONFIG:RelWithDebInfo>:/Zi>
    )
    add_compile_definitions(
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
else()
    add_compile_options(
        -Wall
        -Wextra
        -Wno-unused-parameter
        -Wno-sign-compare
        $<$<CONFIG:Debug>:-g>
        $<$<CONFIG:Debug>:-O0>
        $<$<CONFIG:Release>:-O3>
        $<$<CONFIG:RelWithDebInfo>:-O2>
        $<$<CONFIG:RelWithDebInfo>:-g>
    )
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_compile_options(-Wno-unused-private-field)
    endif()
endif()

if(REBREWU_ENABLE_ASAN)
    if(MSVC)
        add_compile_options(/fsanitize=address)
    else()
        add_compile_options(-fsanitize=address)
        add_compile_options(-fno-omit-frame-pointer)
        add_link_options(-fsanitize=address)
    endif()
endif()
