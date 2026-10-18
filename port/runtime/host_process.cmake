# A GUI PE image starts without allocating a terminal. Keep the normal C/C++
# runtime initialization and main(argc, argv), including static constructors.
function(rbrew_windowed_executable target)
    if(WIN32 AND MSVC)
        set_target_properties(${target} PROPERTIES WIN32_EXECUTABLE TRUE)
        target_link_options(${target} PRIVATE /ENTRY:mainCRTStartup)
    endif()
endfunction()
