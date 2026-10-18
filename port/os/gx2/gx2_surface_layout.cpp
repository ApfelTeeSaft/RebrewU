#include "gx2_surface_layout.h"
#include <algorithm>
#include <bit>

namespace {
// Latte (R700-class) addressing configuration as programmed for GX2.
constexpr uint32_t kPipes = 2, kBanks = 4, kPipeInterleave = 256, kSplitSize = 2048;
constexpr uint32_t kResDepth = 4, kResScan = 8;           // GX2 resource-use flags
enum : uint32_t {
    LinearGeneral = 0, LinearAligned = 1, Tiled1DThin1 = 2, Tiled1DThick = 3,
    Tiled2DThin1 = 4, Tiled2DThin2 = 5, Tiled2DThin4 = 6, Tiled2DThick = 7,
    LinearSpecial = 16,
};
enum : uint32_t { Dim1D, Dim2D, Dim3D, DimCube, Dim1DArray, Dim2DArray };

uint32_t next_pow2(uint32_t v) { return std::bit_ceil(std::max(v, 1u)); }
uint64_t align_up(uint64_t v, uint32_t a) {
    return std::has_single_bit(a) ? (v + a - 1) & ~uint64_t(a - 1) : (v + a - 1) / a * a;
}
uint32_t thickness(uint32_t tile) { return tile == Tiled1DThick || tile == Tiled2DThick ? 4u : 1u; }
bool macro_tiled(uint32_t tile) { return tile >= Tiled2DThin1 && tile <= 15; }

// Bits per element (BCn: per 4x4 block) for the supported hardware formats.
// Bits per element by hardware format (Cemu Latte::GetFormatBits). The 96-bit
// formats 0x2F/0x30 are left at 0: this AddrLib port does not model them.
uint32_t format_bits(uint32_t hw) {
    static constexpr uint8_t bits[0x40] = {
        0x00,0x08,0x08,0x00,0x00,0x10,0x10,0x10, 0x10,0x10,0x10,0x10,0x10,0x20,0x20,0x20,
        0x20,0x20,0x00,0x20,0x00,0x00,0x20,0x00, 0x00,0x20,0x20,0x20,0x40,0x40,0x40,0x40,
        0x40,0x00,0x80,0x80,0x00,0x00,0x00,0x10, 0x10,0x20,0x20,0x20,0x00,0x00,0x00,0x00,
        0x00,0x40,0x80,0x80,0x40,0x80,0x00,0x00, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
    return hw < 0x40 ? bits[hw] : 0u;
}
bool is_bcn(uint32_t hw) { return hw >= 0x31 && hw <= 0x35; }

struct Level {
    uint32_t pitch{}, height{}, depth{}, hw_tile{}, base_align{}, pitch_align{}, height_align{};
    uint64_t size{};
};
struct Flags { bool depth{}, display{}, cube{}; };

uint32_t mip_level_tile_mode(uint32_t base, uint32_t bpp, uint32_t level, uint32_t width,
                             uint32_t height, uint32_t slices, bool depth, bool no_recursion) {
    uint32_t mode = base;
    const uint32_t bytes_per_sample = ((bpp << 6) + 7) >> 3;
    const uint32_t samples = thickness(base) > 1 ? 4u : 1u;
    uint32_t tile_slices = 1;
    if (bytes_per_sample && kSplitSize / bytes_per_sample)
        tile_slices = std::max(1u, samples / (kSplitSize / bytes_per_sample));
    if (base == Tiled1DThick && depth) mode = Tiled1DThin1;
    if (base == Tiled2DThick && (tile_slices > 1 || depth)) mode = Tiled2DThin1;
    if (no_recursion || !level) return mode;
    const uint32_t w = next_pow2(width), h = next_pow2(height), s = next_pow2(slices);
    const uint32_t micro_bytes = (bpp * (thickness(mode) << 6) + 7) >> 3;
    const uint32_t width_factor = micro_bytes >= kPipeInterleave ? 1u : kPipeInterleave / micro_bytes;
    uint32_t macro_w = 8 * kBanks, macro_h = 8 * kPipes;
    if (mode == Tiled2DThin2) { macro_w >>= 1; macro_h *= 2; }
    if (mode == Tiled2DThin4) { macro_w >>= 2; macro_h *= 4; }
    if (macro_tiled(mode) && (w < width_factor * macro_w || h < macro_h))
        mode = mode == Tiled2DThick ? Tiled1DThick : Tiled1DThin1;
    if (mode == Tiled1DThick && s < 4) mode = Tiled1DThin1;
    if (mode == Tiled2DThick && s < 4) mode = Tiled2DThin1;
    return mip_level_tile_mode(mode, bpp, level, w, h, s, depth, true);
}

void pad_dimensions(uint32_t tile, uint32_t pad_dims, bool cube, uint64_t& pitch, uint32_t pitch_align,
                    uint64_t& height, uint32_t height_align, uint64_t& slices, uint32_t slice_align) {
    if (!pad_dims) pad_dims = 3;
    pitch = align_up(pitch, pitch_align);
    if (pad_dims > 1) height = align_up(height, height_align);
    if (pad_dims > 2 || thickness(tile) > 1) {
        if (cube) slices = next_pow2(uint32_t(slices));
        if (thickness(tile) > 1) slices = align_up(slices, slice_align);
    }
}

// Mip levels are padded to powers of two; cube faces keep their count.
void pad_mip_level(uint32_t level, bool cube, uint64_t& pitch, uint64_t& height, uint64_t& slices,
                   uint32_t& pad_dims) {
    if (!level) return;
    pitch = next_pow2(uint32_t(pitch));
    height = next_pow2(uint32_t(height));
    if (cube) pad_dims = slices <= 1 ? 2u : 0u;
    else slices = next_pow2(uint32_t(slices));
}

Level linear(uint32_t tile, uint32_t bpp, uint32_t pitch, uint32_t height, uint32_t slices,
             uint32_t level, uint32_t pad_dims, Flags flags) {
    Level out;
    out.base_align = tile == LinearAligned ? kPipeInterleave : 1u;
    out.pitch_align = tile == LinearAligned ? std::max(64u, 8u * kPipeInterleave / bpp) : 1u;
    out.height_align = 1;
    if (flags.display) out.pitch_align = uint32_t(align_up(out.pitch_align, 32));
    uint64_t p = pitch, h = height, s = slices;
    pad_mip_level(level, flags.cube, p, h, s, pad_dims);
    pad_dimensions(tile, pad_dims, flags.cube, p, out.pitch_align, h, out.height_align, s, 1);
    out.pitch = uint32_t(p); out.height = uint32_t(h); out.depth = uint32_t(s);
    out.size = (h * p * s * bpp + 7) / 8;
    out.hw_tile = tile;
    return out;
}

Level micro(uint32_t tile, uint32_t bpp, uint32_t pitch, uint32_t height, uint32_t slices,
            uint32_t level, uint32_t pad_dims, Flags flags) {
    Level out;
    uint64_t p = pitch, h = height, s = slices;
    pad_mip_level(level, flags.cube, p, h, s, pad_dims);
    if (level && tile == Tiled1DThick && s < 4) tile = Tiled1DThin1;
    out.base_align = kPipeInterleave;
    out.pitch_align = std::max(8u, kPipeInterleave / bpp / thickness(tile));
    out.height_align = 8;
    if (flags.display) out.pitch_align = uint32_t(align_up(out.pitch_align, 32));
    pad_dimensions(tile, pad_dims, flags.cube, p, out.pitch_align, h, out.height_align, s, thickness(tile));
    out.pitch = uint32_t(p); out.height = uint32_t(h); out.depth = uint32_t(s);
    out.size = (h * p * s * bpp + 7) / 8;
    out.hw_tile = tile;
    return out;
}

void macro_alignments(uint32_t tile, uint32_t bpp, uint32_t& base, uint32_t& pitch_align, uint32_t& height_align) {
    const uint32_t aspect = tile == Tiled2DThin2 ? 2u : tile == Tiled2DThin4 ? 4u : 1u;
    const uint32_t thick = thickness(tile);
    const uint32_t macro_w = 8 * kBanks / aspect, macro_h = aspect * 8 * kPipes;
    pitch_align = std::max(macro_w, macro_w * (kPipeInterleave / bpp / (8 * thick)));
    height_align = macro_h;
    const uint32_t macro_bytes = (bpp * macro_h * macro_w + 7) >> 3;
    base = thick == 1 ? std::max(macro_bytes, (height_align * bpp * pitch_align + 7) >> 3)
                      : std::max(kPipeInterleave, (4 * height_align * bpp * pitch_align + 7) >> 3);
    const uint32_t micro_bytes = (thick * (bpp << 6) + 7) >> 3;
    base /= micro_bytes < kSplitSize ? 1u : micro_bytes / kSplitSize;
}

Level macro(uint32_t tile, uint32_t base_tile, uint32_t bpp, uint32_t pitch, uint32_t height,
            uint32_t slices, uint32_t level, uint32_t pad_dims, Flags flags) {
    uint64_t p = pitch, h = height, s = slices;
    uint32_t hw_tile = tile, slice_align = thickness(tile);
    if (level) {
        p = next_pow2(pitch); h = next_pow2(height); s = next_pow2(slices);
        if (flags.cube) { s = slices; pad_dims = slices <= 1 ? 2u : 0u; }
        if (hw_tile == Tiled2DThick && s < 4) { hw_tile = Tiled2DThin1; slice_align = 1; }
    }
    Level out;
    const uint32_t pitch_factor = std::max((kPipeInterleave >> 3) / bpp, 1u);
    if (tile != base_tile && level && base_tile == Tiled2DThick && tile != Tiled2DThick) {
        macro_alignments(base_tile, bpp, out.base_align, out.pitch_align, out.height_align);
        if (p < uint64_t(out.pitch_align) * pitch_factor || h < out.height_align)
            return micro(Tiled1DThin1, bpp, pitch, height, slices, level, pad_dims, flags);
    }
    // AddrLib applies the display pitch adjustment before assigning the
    // macro alignment, so it has no effect here (unlike micro/linear).
    macro_alignments(tile, bpp, out.base_align, out.pitch_align, out.height_align);
    pad_dimensions(tile, pad_dims, flags.cube, p, out.pitch_align, h, out.height_align, s, slice_align);
    out.pitch = uint32_t(p); out.height = uint32_t(h); out.depth = uint32_t(s);
    out.size = (h * p * s * bpp + 7) / 8;
    out.hw_tile = hw_tile;
    return out;
}

std::optional<Level> level_info(const RbrewGx2SurfaceDesc& d, uint32_t tile, uint32_t level) {
    const uint32_t hw = d.format & 0x3F, bpp = format_bits(hw);
    if (!bpp) return std::nullopt;
    if (tile == LinearSpecial) {
        const uint32_t block = is_bcn(hw) ? 4u : 1u;
        Level out;
        out.pitch = std::max(1u, ((d.width >> level) + block - 1) / block);
        uint32_t h = 1, s = 1;
        if (d.dim == Dim2D || d.dim == Dim3D || d.dim == DimCube || d.dim == Dim2DArray) h = std::max(d.height >> level, 1u);
        if (d.dim == Dim3D) s = std::max(d.depth >> level, 1u);
        if (d.dim == DimCube) s = std::max(d.depth, 6u);
        if (d.dim == Dim1DArray || d.dim == Dim2DArray) s = d.depth;
        out.height = std::max(1u, (h + block - 1) / block);
        out.depth = s;
        out.size = uint64_t(bpp) * s * out.height * out.pitch >> 3;
        out.base_align = out.pitch_align = out.height_align = 1;
        out.hw_tile = tile;
        return out;
    }
    uint32_t width = std::max(d.width >> level, 1u), height = 1, slices = 1;
    Flags flags{(d.use & kResDepth) != 0, (d.use & kResScan) != 0, d.dim == DimCube};
    switch (d.dim) {
    case Dim1D: break;
    case Dim2D: height = std::max(d.height >> level, 1u); break;
    case Dim3D: height = std::max(d.height >> level, 1u); slices = std::max(d.depth >> level, 1u); break;
    case DimCube: height = std::max(d.height >> level, 1u); slices = std::max(d.depth, 6u); break;
    case Dim1DArray: slices = d.depth; break;
    case Dim2DArray: height = std::max(d.height >> level, 1u); slices = d.depth; break;
    default: return std::nullopt;
    }
    if (is_bcn(hw)) {
        if (!level) { width = uint32_t(align_up(width, 4)); height = uint32_t(align_up(height, 4)); }
        else { width = next_pow2(width); height = next_pow2(height); }
        width = std::max(width / 4, 1u); height = std::max(height / 4, 1u);
    }
    const uint32_t pad_dims = flags.cube && !level ? 2u : 0u;
    const uint32_t mode = mip_level_tile_mode(tile, bpp, level, width, height, slices, flags.depth, false);
    if (mode <= LinearAligned) return linear(mode, bpp, width, height, slices, level, pad_dims, flags);
    if (mode <= Tiled1DThick) return micro(mode, bpp, width, height, slices, level, pad_dims, flags);
    return macro(mode, tile, bpp, width, height, slices, level, pad_dims, flags);
}

uint32_t level_count(uint32_t resolution) { return resolution ? 32u - uint32_t(std::countl_zero(resolution)) : 0u; }
}

std::optional<RbrewGx2SurfaceLayout> rbrew_gx2_calc_surface_layout(const RbrewGx2SurfaceDesc& d) {
    const uint32_t tile_in = d.tile_mode;
    if (d.aa || d.dim > Dim2DArray || !format_bits(d.format & 0x3F)) return std::nullopt;
    if (!(tile_in <= Tiled2DThick || tile_in == LinearSpecial)) return std::nullopt;
    RbrewGx2SurfaceLayout out;
    uint32_t tile = tile_in;
    bool change_tile = false;
    if (tile == LinearGeneral) {
        if (d.dim != Dim1D || (d.use & kResDepth)) {
            tile = d.dim != Dim3D || (d.use & 2u) ? Tiled2DThin1 : Tiled2DThick;
            change_tile = true;
        } else {
            tile = LinearAligned;
        }
    }
    uint32_t last_tile = tile;
    uint32_t levels = d.levels ? d.levels : 1u;
    if (levels > 1) {
        uint32_t limit = std::max(level_count(d.width), level_count(d.height));
        if (d.dim == Dim3D) limit = std::max(limit, level_count(d.depth));
        levels = std::min(levels, limit);
    } else {
        levels = 1;
    }
    if (levels > 14) return std::nullopt;
    uint32_t swizzle = macro_tiled(tile) ? (d.swizzle & 0xFF00FFFFu) | 0xD0000u : d.swizzle & 0xFF00FFFFu;
    uint32_t first_mip = 0, prev_size = 0;
    for (uint32_t level = 0; level < levels; ++level) {
        auto info = level_info(d, tile, level);
        if (!info || info->size > UINT32_MAX) return std::nullopt;
        if (level) {
            uint64_t pad = 0;
            if (macro_tiled(last_tile) && !macro_tiled(info->hw_tile)) {
                swizzle = (swizzle & 0xFF00FFFFu) | (level << 16);
                last_tile = info->hw_tile;
                if (level > 1) pad = swizzle & 0xFFFF;
            }
            pad += (info->base_align - prev_size % info->base_align) % info->base_align;
            const uint64_t offset = pad + prev_size + (level > 1 ? out.mip_offsets[level - 2] : 0u);
            if (offset > UINT32_MAX) return std::nullopt;
            if (level == 1) first_mip = uint32_t(offset);
            else out.mip_offsets[level - 1] = uint32_t(offset);
        } else {
            if (change_tile) {
                if (tile != info->hw_tile) {
                    tile = info->hw_tile;
                    info = level_info(d, tile, 0);
                    if (!info) return std::nullopt;
                    if (!macro_tiled(tile)) swizzle &= 0xFF00FFFFu;
                    last_tile = tile;
                }
                if (d.width < info->pitch_align && d.height < info->height_align) {
                    tile = tile == Tiled2DThick ? Tiled1DThick : Tiled1DThin1;
                    info = level_info(d, tile, 0);
                    if (!info) return std::nullopt;
                    swizzle &= 0xFF00FFFFu;
                    last_tile = tile;
                }
            }
            out.image_size = uint32_t(info->size);
            out.alignment = info->base_align;
            out.pitch = info->pitch;
            out.padded_height = info->height;
        }
        prev_size = uint32_t(info->size);
        out.level_pitch[level]=info->pitch;out.level_height[level]=info->height;
        out.level_tile[level]=info->hw_tile;out.level_size[level]=uint32_t(info->size);
    }
    // mip_offsets[level-1] holds offsets relative to the first mip; the
    // first mip's offset from the image base is stored in slot 0.
    const uint64_t mip_size = levels > 1 ? uint64_t(prev_size) + (levels > 2 ? out.mip_offsets[levels - 2] : 0u) : 0u;
    if (mip_size > UINT32_MAX) return std::nullopt;
    out.mip_size = uint32_t(mip_size);
    out.mip_offsets[0] = first_mip;
    out.levels = levels;
    out.tile_mode = tile;
    out.swizzle = swizzle;
    return out;
}
