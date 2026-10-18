#pragma once
#include <array>
#include <cstdint>
#include <optional>

// GX2CalcSurfaceSizeAndAlignment for single-sample surfaces, following the
// R600/Latte AddrLib rules used by GX2 (Cemu GX2_Surface / LatteAddrLib):
// per-level dimensions (BCn power-of-two padding), mip-level tile-mode
// degradation, linear/micro/macro alignments and mip offsets.
// Unsupported inputs return nullopt so callers fail explicitly instead of
// publishing an undersized allocation: multisampling, unknown formats,
// bank-swapped/3D tile modes, TM_32_SPECIAL and more than 14 levels.
struct RbrewGx2SurfaceDesc {
    uint32_t dim{}, width{}, height{}, depth{}, levels{}, format{}, aa{}, use{}, tile_mode{}, swizzle{};
};
struct RbrewGx2SurfaceLayout {
    uint32_t levels{}, image_size{}, mip_size{}, tile_mode{}, swizzle{}, alignment{}, pitch{};
    std::array<uint32_t, 13> mip_offsets{};
    uint32_t padded_height{};
    std::array<uint32_t,14> level_pitch{},level_height{},level_tile{},level_size{};
};
std::optional<RbrewGx2SurfaceLayout> rbrew_gx2_calc_surface_layout(const RbrewGx2SurfaceDesc& desc);
