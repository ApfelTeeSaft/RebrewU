// r700_to_glsl.h — AMD R700 / Wii U Latte GPU shader bytecode → GLSL 410 core
//
// Entry point: r700_to_glsl().  Returns a heap-allocated GLSL source string;
// caller must free() the result.  Returns nullptr on fatal parse error.
#pragma once
#include <stdint.h>

struct R700TranslateInput {
    const uint8_t* data;       // big-endian R700 bytecode
    uint32_t       size;       // byte size of bytecode
    bool           is_pixel;   // false = vertex shader, true = pixel shader
    uint32_t       num_attribs;// number of vertex attribute slots (vertex shader only)
};

// Translate R700 bytecode to GLSL 410 core source.
// Returns heap-allocated string (caller must free()).
// Returns nullptr if data/size is invalid.
char* r700_to_glsl(const R700TranslateInput& in);
