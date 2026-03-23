// gx2_attrib.h — GX2AttribFormat constants and GL type mapping
//
// Provides the mapping from the GX2 vertex attribute format encoding
// (as stored in GX2AttribStream.format) to GL type info used by
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
// SDL_opengl.h → windows.h) may #define U16 / U32 as integer constants,
// which causes MSVC C2059 when the preprocessor expands them inside a
// 'static constexpr uint32_t <name> = ...' declaration.
static constexpr uint32_t GX2_INDEX_TYPE_U16_LE = 0;   // little-endian uint16 (rare)
static constexpr uint32_t GX2_INDEX_TYPE_U32_LE = 1;   // little-endian uint32 (rare)
static constexpr uint32_t GX2_INDEX_TYPE_U16_BE = 4;   // big-endian  uint16 (Wii U native)
static constexpr uint32_t GX2_INDEX_TYPE_U32_BE = 9;   // big-endian  uint32 (Wii U native)

// ---------------------------------------------------------------------------
// GL type info for a GX2AttribFormat
// ---------------------------------------------------------------------------
struct GX2AttribGL {
    unsigned int gl_type;    // GLenum — using unsigned int to avoid including GL headers
    int          components; // number of components (1-4)
    unsigned char normalized;// GL_TRUE or GL_FALSE
    uint32_t component_bytes;// bytes per individual component (used for endian-swap)
};

// Returns GL vertex attribute type description for a GX2AttribFormat.
// gl_type uses standard OpenGL enum values so no GL header is needed here.
static inline GX2AttribGL gx2_attrib_format_to_gl(uint32_t fmt) {
    // GL type constants (no GL header needed)
    static constexpr unsigned int GL_BYTE_T              = 0x1400;
    static constexpr unsigned int GL_UNSIGNED_BYTE_T     = 0x1401;
    static constexpr unsigned int GL_FLOAT_T             = 0x1406;
    static constexpr unsigned int GL_HALF_FLOAT_T        = 0x140B;
    static constexpr unsigned int GL_INT_2_10_10_10_REV_T= 0x8D9F;
    static constexpr unsigned int GL_UINT_2_10_10_10_REV_T=0x8368;

    switch (fmt) {
    // 8-bit unsigned normalized
    case GX2_ATTRIB_FORMAT_UNORM_8:           return { GL_UNSIGNED_BYTE_T, 1, 1, 1 };
    case GX2_ATTRIB_FORMAT_UNORM_8_8:         return { GL_UNSIGNED_BYTE_T, 2, 1, 1 };
    case GX2_ATTRIB_FORMAT_UNORM_8_8_8_8:     return { GL_UNSIGNED_BYTE_T, 4, 1, 1 };
    // 10_10_10_2 packed
    case GX2_ATTRIB_FORMAT_UNORM_10_10_10_2:  return { GL_UINT_2_10_10_10_REV_T, 4, 1, 4 };
    case GX2_ATTRIB_FORMAT_SNORM_10_10_10_2:  return { GL_INT_2_10_10_10_REV_T,  4, 1, 4 };
    // 8-bit unsigned integer
    case GX2_ATTRIB_FORMAT_UINT_8:            return { GL_UNSIGNED_BYTE_T, 1, 0, 1 };
    case GX2_ATTRIB_FORMAT_UINT_8_8:          return { GL_UNSIGNED_BYTE_T, 2, 0, 1 };
    case GX2_ATTRIB_FORMAT_UINT_8_8_8_8:      return { GL_UNSIGNED_BYTE_T, 4, 0, 1 };
    // 8-bit signed normalized
    case GX2_ATTRIB_FORMAT_SNORM_8:           return { GL_BYTE_T, 1, 1, 1 };
    case GX2_ATTRIB_FORMAT_SNORM_8_8:         return { GL_BYTE_T, 2, 1, 1 };
    case GX2_ATTRIB_FORMAT_SNORM_8_8_8_8:     return { GL_BYTE_T, 4, 1, 1 };
    // 8-bit signed integer
    case GX2_ATTRIB_FORMAT_SINT_8:            return { GL_BYTE_T, 1, 0, 1 };
    case GX2_ATTRIB_FORMAT_SINT_8_8:          return { GL_BYTE_T, 2, 0, 1 };
    case GX2_ATTRIB_FORMAT_SINT_8_8_8_8:      return { GL_BYTE_T, 4, 0, 1 };
    // 16-bit float
    case GX2_ATTRIB_FORMAT_FLOAT_16:          return { GL_HALF_FLOAT_T, 1, 0, 2 };
    case GX2_ATTRIB_FORMAT_FLOAT_16_16:       return { GL_HALF_FLOAT_T, 2, 0, 2 };
    case GX2_ATTRIB_FORMAT_FLOAT_16_16_16_16: return { GL_HALF_FLOAT_T, 4, 0, 2 };
    // 32-bit float
    case GX2_ATTRIB_FORMAT_FLOAT_32:          return { GL_FLOAT_T, 1, 0, 4 };
    case GX2_ATTRIB_FORMAT_FLOAT_32_32:       return { GL_FLOAT_T, 2, 0, 4 };
    case GX2_ATTRIB_FORMAT_FLOAT_32_32_32:    return { GL_FLOAT_T, 3, 0, 4 };
    case GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32: return { GL_FLOAT_T, 4, 0, 4 };
    default:
        // Unknown — treat as 4-component float
        return { GL_FLOAT_T, 4, 0, 4 };
    }
}
