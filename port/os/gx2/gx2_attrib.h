// gx2_attrib.h — GX2AttribFormat constants and GPU type mapping
//
// Provides the mapping from the GX2 vertex attribute format encoding
// (as stored in GX2AttribStream.format) to GPU type info used by
// glVertexAttribPointer.  Also used to infer the per-component byte
// size needed for endian-swap decisions.
#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
// GX2AttribFormat constants
// ---------------------------------------------------------------------------

static constexpr uint32_t GX2_ATTRIB_FORMAT_UNORM_8             = 0x00000000u;
static constexpr uint32_t GX2_ATTRIB_FORMAT_UNORM_8_8           = 0x00000004u;
static constexpr uint32_t GX2_ATTRIB_FORMAT_UNORM_8_8_8_8       = 0x0000000Au;
static constexpr uint32_t GX2_ATTRIB_FORMAT_UNORM_10_10_10_2    = 0x0000000Bu;
static constexpr uint32_t GX2_ATTRIB_FORMAT_UINT_8              = 0x00000100u;
static constexpr uint32_t GX2_ATTRIB_FORMAT_UINT_8_8            = 0x00000104u;
static constexpr uint32_t GX2_ATTRIB_FORMAT_UINT_8_8_8_8        = 0x0000010Au;
static constexpr uint32_t GX2_ATTRIB_FORMAT_SNORM_8             = 0x00000200u;
static constexpr uint32_t GX2_ATTRIB_FORMAT_SNORM_8_8           = 0x00000204u;
static constexpr uint32_t GX2_ATTRIB_FORMAT_SNORM_8_8_8_8       = 0x0000020Au;
static constexpr uint32_t GX2_ATTRIB_FORMAT_SNORM_10_10_10_2    = 0x0000020Bu;
static constexpr uint32_t GX2_ATTRIB_FORMAT_SINT_8              = 0x00000300u;
static constexpr uint32_t GX2_ATTRIB_FORMAT_SINT_8_8            = 0x00000304u;
static constexpr uint32_t GX2_ATTRIB_FORMAT_SINT_8_8_8_8        = 0x0000030Au;
static constexpr uint32_t GX2_ATTRIB_FORMAT_FLOAT_16            = 0x00000803u;
static constexpr uint32_t GX2_ATTRIB_FORMAT_FLOAT_16_16         = 0x00000808u;
static constexpr uint32_t GX2_ATTRIB_FORMAT_FLOAT_16_16_16_16   = 0x0000080Du;
static constexpr uint32_t GX2_ATTRIB_FORMAT_FLOAT_32            = 0x00000806u;
static constexpr uint32_t GX2_ATTRIB_FORMAT_FLOAT_32_32         = 0x00000813u;
static constexpr uint32_t GX2_ATTRIB_FORMAT_FLOAT_32_32_32      = 0x00000817u;
static constexpr uint32_t GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32   = 0x0000081Bu;

// GX2AttribStream.endianSwap values
static constexpr uint32_t GX2_ENDIAN_SWAP_DEFAULT = 0; // infer from format
static constexpr uint32_t GX2_ENDIAN_SWAP_8_IN_16 = 1; // swap bytes in 16-bit words
static constexpr uint32_t GX2_ENDIAN_SWAP_8_IN_32 = 2; // swap bytes in 32-bit words
static constexpr uint32_t GX2_ENDIAN_SWAP_NONE    = 3;

// GX2IndexType values
// NOTE: avoid plain U16 / U32 suffixes — Windows SDK headers (pulled in by
// windows.h) may #define U16 / U32 as integer constants,
// which causes MSVC C2059 when the preprocessor expands them inside a
// 'static constexpr uint32_t <name> = ...' declaration.
static constexpr uint32_t GX2_INDEX_TYPE_U16_LE = 0;   // little-endian uint16 (rare)
static constexpr uint32_t GX2_INDEX_TYPE_U32_LE = 1;   // little-endian uint32 (rare)
static constexpr uint32_t GX2_INDEX_TYPE_U16_BE = 4;   // big-endian  uint16 (Wii U native)
static constexpr uint32_t GX2_INDEX_TYPE_U32_BE = 9;   // big-endian  uint32 (Wii U native)

