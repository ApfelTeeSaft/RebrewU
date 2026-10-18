// r700_translation_reads.h - which per-unit/per-parameter context words the
// last r700_to_glsl call on this thread read.
//
// The translator reads SPI_VS_OUT_ID_0 + i/4 only for exported VS
// parameters i (shader->outputParameterMask) and the SQ_TEX_RESOURCE words
// only for texture units the program samples (output->textureUnitMask); both
// sets come from the program's own instructions. The Vulkan program key uses
// them to hash only words that were read (gx2_render_vk_draw.cpp).
#pragma once
#include <cstdint>

struct R700TranslationReads {
    bool valid = false;               // set by a successful translation
    uint32_t vs_parameter_mask = 0;   // bit i: SPI_VS_OUT_ID_0 + i/4 read
    uint32_t texture_unit_mask = 0;   // bit t: unit t's texture words read
};
// The last translation on the calling thread (valid=false after a failure).
R700TranslationReads r700_last_translation_reads();
