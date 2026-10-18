add_library(cemu_shader STATIC
    PSInput.cpp
    Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.cpp
    Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerAnalyzer.cpp
    Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerRegisterDataTypeTracker.cpp
    Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerEmitGLSL.cpp
    Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerEmitGLSLAttrDecoder.cpp
    Cafe/HW/Latte/Core/LatteGSCopyShaderParser.cpp
    Cafe/HW/Latte/LatteAddrLib/LatteAddrLib_Coord.cpp
    Cafe/HW/Latte/LatteAddrLib/LatteAddrLib_Tiling.cpp)
target_include_directories(cemu_shader PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(MSVC)
    target_compile_options(cemu_shader PRIVATE /FIStandalone.h /utf-8 /W0)
else()
    target_compile_options(cemu_shader PRIVATE -include Standalone.h)
endif()
