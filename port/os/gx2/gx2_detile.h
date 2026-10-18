#pragma once
// Convert mip level 0 of a tiled Latte surface into a linear image for an
// native graphics upload (renderer plan step 3). Addressing comes from Cemu's
// LatteAddrLib (third_party/cemu_shader, MPL-2.0); this file is ours.
#include <cstddef>
#include <cstdint>

struct Gx2DetileDesc {
    uint32_t width = 0, height = 0;   // in elements (4x4 blocks for BCn)
    uint32_t pitch = 0;               // in elements (GX2Surface.pitch)
    uint32_t bits_per_element = 0;    // 32 for RGBA8, 64 for BC1, 128 for BC2/3
    uint32_t tile_mode = 0;           // GX2Surface.tileMode (Latte E_HWTILEMODE)
    uint32_t swizzle = 0;             // GX2Surface.swizzle
    bool is_depth = false;
    uint32_t slice = 0;              // array layer / cube face
    uint32_t surface_height = 0;     // padded allocation height; 0 uses height
};

// out receives width*height elements, rows packed. Returns false (and
// writes nothing meaningful) when the description is unsupported or the
// source would be read out of bounds.
bool gx2_detile_mip0(const Gx2DetileDesc& desc, const uint8_t* tiled, size_t tiled_size, uint8_t* out);

// Inverse transfer. Only logical elements are changed; padding and other
// slices survive. Invalid bounds are rejected before changing the destination.
bool gx2_retile_mip0(const Gx2DetileDesc& desc, const uint8_t* linear,
                    uint8_t* tiled, size_t tiled_size);
