// gambit_loader.cpp
//
// Provides gambit_data_init() for the case where the generated Gambit_data.cpp
// was produced by an older version of the emitter that did not include the
// loader function.  When the emitter is re-run it will emit
// "void Gambit_data_init(uint8_t* arena)" directly inside Gambit_data.cpp,
// at which point this file should be removed from the build.
//
// If Gambit_data.cpp already defines Gambit_data_init this translation unit
// will produce a duplicate-symbol link error — remove it from CMakeLists.txt.

#include <stdint.h>

// Weak default definition: does nothing.
// The strong definition in the generated Gambit_data.cpp (once the emitter is
// updated) will take precedence at link time.
#ifdef _MSC_VER
// MSVC: provide a named default and redirect the public symbol to it via
// /alternatename so a strong definition in another TU wins.
void gambit_data_init_default(uint8_t* arena) { (void)arena; }
#pragma comment(linker, "/alternatename:gambit_data_init=gambit_data_init_default")
#else
__attribute__((weak))
void gambit_data_init(uint8_t* arena) {
    (void)arena;
    // No-op: data sections not loaded.
    // Re-run the recompiler to regenerate Gambit_data.cpp with the loader.
}
#endif
