// gx2_shader_key.h - context words the R700 shader translator reads.
//
// Besides the bytecode, fetch layout, attribute count, primitive kind and
// the texture words of each stage (SQ_TEX_RESOURCE word0 DIM bits 0-2 and
// word4 NUM_FORMAT_ALL bits 8-9 at 0xE000/0xE460 + unit*7), r700_to_glsl and the vendored
// Latte decompiler read only these register words: every raw
// contextRegisters[mm...] index, typed contextRegistersNew-> field and PS
// input table read in cemu_shader/Cafe/HW/Latte/LegacyShaderDecompiler,
// PSInput.cpp and r700_to_glsl.cpp (cf. Cemu's LatteShader.cpp base/aux
// hashes). Shader descriptors reach the translator only through these
// registers (r700_shader_registers). Ranges are inclusive. Adding a read to
// the translator requires adding it here.
#pragma once
#include <cstdint>

struct Gx2RegisterRange { uint16_t first, last; };
inline constexpr Gx2RegisterRange kGx2TranslatorRegisters[] = {
    {0xA08F, 0xA08F},  // CB_SHADER_MASK
    {0xA0E0, 0xA0FF},  // SQ_VTX_SEMANTIC_0..31
    {0xA104, 0xA104},  // SX_ALPHA_TEST_CONTROL
    {0xA185, 0xA18E},  // SPI_VS_OUT_ID_0..9
    {0xA191, 0xA1B6},  // SPI_PS_INPUT_CNTL_0..31, SPI_VS_OUT_CONFIG, SPI_PS_IN_CONTROL_0/1, SPI_INTERP_CONTROL_0, SPI_INPUT_Z
    {0xA1E8, 0xA1E8},  // CB_SHADER_CONTROL
    {0xA203, 0xA204},  // DB_SHADER_CONTROL, PA_CL_CLIP_CNTL
    {0xA206, 0xA207},  // PA_CL_VTE_CNTL, PA_CL_VS_OUT_CNTL
    {0xA214, 0xA215},  // SQ_PGM_RESOURCES_PS, SQ_PGM_EXPORTS_PS
    {0xA21A, 0xA21A},  // SQ_PGM_RESOURCES_VS
    {0xA22B, 0xA22B},  // SQ_GSVS_RING_ITEMSIZE
    {0xA232, 0xA232},  // SQ_GS_VERT_ITEMSIZE
    {0xA238, 0xA238},  // SQ_VTX_SEMANTIC_CLEAR
    {0xA29B, 0xA29B},  // VGT_GS_OUT_PRIM_TYPE
    {0xA2A1, 0xA2A1},  // VGT_PRIMITIVEID_EN
    {0xA2AC, 0xA2AC},  // VGT_STRMOUT_EN
    {0xA2B5, 0xA2B5}, {0xA2B9, 0xA2B9}, {0xA2BD, 0xA2BD}, {0xA2C1, 0xA2C1},  // VGT_STRMOUT_VTX_STRIDE_0..3
    {0xA2C8, 0xA2C8},  // VGT_STRMOUT_BUFFER_EN
    {0x2256, 0x2256},  // VGT_PRIMITIVE_TYPE
};
inline constexpr unsigned gx2_translator_register_count() {
    unsigned n = 0;
    for (const auto& r : kGx2TranslatorRegisters) n += r.last - r.first + 1u;
    return n;
}
// Bits of each listed word that the translator reads. The exact key hashes
// whole words; the canonical key (equal translations shared across exact keys)
// hashes only these bits. Every other listed word is written by
// r700_shader_registers but read by no translator path:
// - SX_ALPHA_TEST_CONTROL: only ALPHA_FUNC and ALPHA_TEST_ENABLE.
// - PA_CL_CLIP_CNTL: only DX_CLIP_SPACE_DEF.
// - PA_CL_VTE_CNTL: only the six VPORT_*_ENA bits.
// - VGT_PRIMITIVE_TYPE: only "== POINTS", keyed separately as `points`.
inline constexpr uint32_t gx2_translator_register_bits(uint32_t r) {
    switch (r) {
    case 0xA104: return 0xFu;          // SX_ALPHA_TEST_CONTROL: ALPHA_FUNC 0-2, ALPHA_TEST_ENABLE 3
    case 0xA204: return 1u << 19;      // PA_CL_CLIP_CNTL: DX_CLIP_SPACE_DEF
    case 0xA206: return 0x3Fu;         // PA_CL_VTE_CNTL: VPORT_{X,Y,Z}_{SCALE,OFFSET}_ENA
    case 0x2256: return 0;             // VGT_PRIMITIVE_TYPE
    case 0xA1B1: case 0xA1B2: case 0xA1B6:            // SPI_VS_OUT_CONFIG, (unused), SPI_INPUT_Z
    case 0xA203: case 0xA207:                          // DB_SHADER_CONTROL, PA_CL_VS_OUT_CNTL
    case 0xA214: case 0xA215: case 0xA21A:            // SQ_PGM_RESOURCES_PS, SQ_PGM_EXPORTS_PS, SQ_PGM_RESOURCES_VS
    case 0xA238: case 0xA2A1: case 0xA2C8: return 0;  // SQ_VTX_SEMANTIC_CLEAR, VGT_PRIMITIVEID_EN, VGT_STRMOUT_BUFFER_EN
    default: return ~0u;
    }
}
// Texture resource words read per used unit (stage base + unit * 7 + word).
inline constexpr uint32_t kGx2TranslatorTextureBases[] = {0xE000, 0xE460};
inline bool gx2_translator_reads_register(uint32_t r) {
    for (const auto& range : kGx2TranslatorRegisters) if (r >= range.first && r <= range.last) return true;
    for (uint32_t base : kGx2TranslatorTextureBases)
        if (r >= base && r < base + 18 * 7 && ((r - base) % 7 == 0 || (r - base) % 7 == 4)) return true;
    return false;
}
