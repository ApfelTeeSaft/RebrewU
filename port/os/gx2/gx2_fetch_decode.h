#pragma once
// Decode a pre-built Latte fetch-shader program (models ship them compiled;
// no GX2InitFetchShaderEx call) into the attribute records the renderer
// uses for runtime-initialised fetch shaders. Hardware facts (R600/R700 ISA):
// CF words (LE, 8 bytes): word0 ADDR (x8 bytes), word1 COUNT bits 10-12
// (+bit 19), END_OF_PROGRAM bit 21, CF_INST bits 23-29 (VTX = 2, VTX_TC = 3).
// VTX fetch (LE, 16 bytes): w0 VTX_INST 0-4 (SEMANTIC = 1), FETCH_TYPE 5-6,
// BUFFER_ID 8-15 (0xA0 + buffer); w1 SEMANTIC_ID 0-7, DST_SEL 9-20,
// DATA_FORMAT 22-27, NUM_FORMAT_ALL 28-29, FORMAT_COMP_ALL 30; w2 OFFSET 0-15,
// ENDIAN_SWAP 16-17.
#include "r700_to_glsl.h"
#include <cstdint>
#include <vector>

// Per-instance fetches select their step rate with SRC_SEL_X (w0 bits 24-25):
// W = divisor 1, Y/Z = VGT_INSTANCE_STEP_RATE_0/1, which GX2SetFetchShader
// loads from the GX2FetchShader divisors (pass them; null means none).
// Returns false when the program is malformed or uses an unknown format.
bool gx2_decode_fetch_program(const uint8_t* program, uint32_t size, std::vector<R700FetchAttribute>& out,
                              const uint32_t* divisors = nullptr);
