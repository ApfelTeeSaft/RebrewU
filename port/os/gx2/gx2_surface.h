// gx2_surface.h — GX2 structure field offsets and enum constants
//
// All GX2Surface/ColorBuffer/DepthBuffer/Texture fields are big-endian
// uint32 unless noted. Use rbrew_read32(mem, base + OFFSET) to access them.
#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
// GX2Surface field offsets (size: 0x9C bytes)
// ---------------------------------------------------------------------------
#define GX2SURF_DIM           0x00u  // GX2SurfaceDim
#define GX2SURF_WIDTH         0x04u
#define GX2SURF_HEIGHT        0x08u
#define GX2SURF_DEPTH         0x0Cu
#define GX2SURF_MIP_LEVELS    0x10u
#define GX2SURF_FORMAT        0x14u  // GX2SurfaceFormat
#define GX2SURF_AA            0x18u
#define GX2SURF_USE           0x1Cu
#define GX2SURF_IMAGE_SIZE    0x20u
#define GX2SURF_IMAGE         0x24u  // guest ptr to pixel data
#define GX2SURF_MIPMAP_SIZE   0x28u
#define GX2SURF_MIPMAPS       0x2Cu  // guest ptr to mip data
#define GX2SURF_TILE_MODE     0x30u
#define GX2SURF_SWIZZLE       0x34u
#define GX2SURF_ALIGNMENT     0x38u
#define GX2SURF_PITCH         0x3Cu
#define GX2SURF_SIZEOF        0x9Cu

// ---------------------------------------------------------------------------
// GX2ColorBuffer field offsets (size: 0xC4 bytes)
// ---------------------------------------------------------------------------
#define GX2CB_SURFACE         0x00u  // GX2Surface embedded
#define GX2CB_VIEW_MIP        0x9Cu
#define GX2CB_VIEW_FIRST_SLICE 0xA0u
#define GX2CB_VIEW_NUM_SLICES  0xA4u
#define GX2CB_AA_BUFFER       0xA8u
#define GX2CB_AA_SIZE         0xACu
// regs[5] at 0xB0
#define GX2CB_SIZEOF          0xC4u

// ---------------------------------------------------------------------------
// GX2DepthBuffer field offsets (size: 0xC8 bytes)
// ---------------------------------------------------------------------------
#define GX2DB_SURFACE         0x00u  // GX2Surface embedded
#define GX2DB_VIEW_MIP        0x9Cu
#define GX2DB_VIEW_FIRST_SLICE 0xA0u
#define GX2DB_VIEW_NUM_SLICES  0xA4u
#define GX2DB_HIZ_PTR         0xA8u
#define GX2DB_HIZ_SIZE        0xACu
#define GX2DB_DEPTH_CLEAR     0xB0u  // f32 big-endian
#define GX2DB_STENCIL_CLEAR   0xB4u  // uint8
// regs[7] at 0xB8
#define GX2DB_SIZEOF          0xC8u

// ---------------------------------------------------------------------------
// GX2Texture field offsets (size: 0xC4 bytes)
// ---------------------------------------------------------------------------
#define GX2TEX_SURFACE        0x00u  // GX2Surface embedded
#define GX2TEX_VIEW_MIP       0x9Cu
#define GX2TEX_VIEW_NUM_MIPS  0xA0u
#define GX2TEX_VIEW_FIRST_SLICE 0xA4u
#define GX2TEX_VIEW_NUM_SLICES 0xA8u
#define GX2TEX_COMP_MAP       0xACu  // component swizzle
// regs[5] at 0xB0
#define GX2TEX_SIZEOF         0xC4u

// ---------------------------------------------------------------------------
// GX2SurfaceFormat enum (subset — value stored at GX2SURF_FORMAT)
// ---------------------------------------------------------------------------
#define GX2FMT_UNORM_R8               0x0001u
#define GX2FMT_UNORM_R8_G8            0x0007u
#define GX2FMT_UNORM_R5_G6_B5         0x0008u
#define GX2FMT_UNORM_R5_G5_B5_A1      0x000Au
#define GX2FMT_UNORM_R4_G4_B4_A4      0x000Bu
#define GX2FMT_UNORM_R8_G8_B8_A8      0x001Au  // most common colour buffer
#define GX2FMT_SRGB_R8_G8_B8_A8       0x041Au  // sRGB colour buffer
#define GX2FMT_UNORM_BC1              0x0031u  // DXT1
#define GX2FMT_UNORM_BC2              0x0032u  // DXT3
#define GX2FMT_UNORM_BC3              0x0033u  // DXT5
#define GX2FMT_UNORM_BC4              0x0034u
#define GX2FMT_UNORM_BC5              0x0035u
#define GX2FMT_FLOAT_D32              0x0806u  // depth only
#define GX2FMT_FLOAT_D24_S8           0x0811u  // depth + stencil (most common)
#define GX2FMT_FLOAT_R11_G11_B10      0x0816u
#define GX2FMT_FLOAT_R16              0x080Fu
#define GX2FMT_FLOAT_R16_G16          0x081Fu
#define GX2FMT_FLOAT_R16_G16_B16_A16  0x0821u
#define GX2FMT_FLOAT_R32              0x080Eu
#define GX2FMT_FLOAT_R32_G32          0x0822u
#define GX2FMT_FLOAT_R32_G32_B32_A32  0x0823u

// ---------------------------------------------------------------------------
// GX2SurfaceDim enum
// ---------------------------------------------------------------------------
#define GX2_SURFACE_DIM_TEXTURE_2D    0x01u
#define GX2_SURFACE_DIM_TEXTURE_CUBE  0x08u

// ---------------------------------------------------------------------------
// GX2TileMode enum
// ---------------------------------------------------------------------------
#define GX2_TILE_MODE_DEFAULT         0x00u
#define GX2_TILE_MODE_LINEAR_ALIGNED  0x01u
#define GX2_TILE_MODE_TILED_1D_THIN1  0x02u
#define GX2_TILE_MODE_TILED_2D_THIN1  0x04u

// ---------------------------------------------------------------------------
// GX2PrimitiveMode enum (for GX2DrawEx)
// ---------------------------------------------------------------------------
#define GX2_PRIMITIVE_MODE_POINTS         0x01u
#define GX2_PRIMITIVE_MODE_LINES          0x02u
#define GX2_PRIMITIVE_MODE_LINE_STRIP     0x03u
#define GX2_PRIMITIVE_MODE_TRIANGLES      0x04u
#define GX2_PRIMITIVE_MODE_TRIANGLE_FAN   0x05u
#define GX2_PRIMITIVE_MODE_TRIANGLE_STRIP 0x06u
#define GX2_PRIMITIVE_MODE_QUADS          0x13u
#define GX2_PRIMITIVE_MODE_QUAD_STRIP     0x14u

// GX2IndexType
#define GX2_INDEX_TYPE_U16  0u
#define GX2_INDEX_TYPE_U32  1u

// ---------------------------------------------------------------------------
// GX2CompareFunction enum (depth/stencil test)
// ---------------------------------------------------------------------------
#define GX2_COMPARE_FUNC_NEVER    0u
#define GX2_COMPARE_FUNC_LESS     1u
#define GX2_COMPARE_FUNC_EQUAL    2u
#define GX2_COMPARE_FUNC_LEQUAL   3u
#define GX2_COMPARE_FUNC_GREATER  4u
#define GX2_COMPARE_FUNC_NOT_EQUAL 5u
#define GX2_COMPARE_FUNC_GEQUAL   6u
#define GX2_COMPARE_FUNC_ALWAYS   7u

// ---------------------------------------------------------------------------
// GX2BlendMode enum (blend factor)
// ---------------------------------------------------------------------------
#define GX2_BLEND_MODE_ZERO                0u
#define GX2_BLEND_MODE_ONE                 1u
#define GX2_BLEND_MODE_SRC_COLOR           2u
#define GX2_BLEND_MODE_INV_SRC_COLOR       3u
#define GX2_BLEND_MODE_SRC_ALPHA           4u
#define GX2_BLEND_MODE_INV_SRC_ALPHA       5u
#define GX2_BLEND_MODE_DST_ALPHA           6u
#define GX2_BLEND_MODE_INV_DST_ALPHA       7u
#define GX2_BLEND_MODE_DST_COLOR           8u
#define GX2_BLEND_MODE_INV_DST_COLOR       9u
#define GX2_BLEND_MODE_SRC_ALPHA_SAT       10u
#define GX2_BLEND_MODE_BOTH_SRC_ALPHA      11u
#define GX2_BLEND_MODE_INV_BOTH_SRC_ALPHA  12u
#define GX2_BLEND_MODE_CONSTANT_ALPHA      13u
#define GX2_BLEND_MODE_INV_CONSTANT_ALPHA  14u
#define GX2_BLEND_MODE_SRC1_COLOR          15u
#define GX2_BLEND_MODE_INV_SRC1_COLOR      16u
#define GX2_BLEND_MODE_SRC1_ALPHA          17u
#define GX2_BLEND_MODE_INV_SRC1_ALPHA      18u
#define GX2_BLEND_MODE_CONSTANT_COLOR      19u
#define GX2_BLEND_MODE_INV_CONSTANT_COLOR  20u

// GX2BlendCombineMode
#define GX2_BLEND_COMBINE_MODE_ADD              0u
#define GX2_BLEND_COMBINE_MODE_DST_MINUS_SRC    1u
#define GX2_BLEND_COMBINE_MODE_SRC_MINUS_DST    2u
#define GX2_BLEND_COMBINE_MODE_MIN              3u
#define GX2_BLEND_COMBINE_MODE_MAX              4u

// ---------------------------------------------------------------------------
// GX2FrontFaceMode
// ---------------------------------------------------------------------------
#define GX2_FRONT_FACE_CCW  0u
#define GX2_FRONT_FACE_CW   1u

// ---------------------------------------------------------------------------
// GX2ClearFlags for GX2ClearDepthStencilEx
// ---------------------------------------------------------------------------
#define GX2_CLEAR_FLAGS_DEPTH    0x01u
#define GX2_CLEAR_FLAGS_STENCIL  0x02u
