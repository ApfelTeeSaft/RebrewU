#include "../../runtime/rbrew_diagnostics.h"
#include "gx2_texture_diagnostics.h"
// gx2_render_vk.cpp - Vulkan back-end for GX2 (render targets, textures,
// samplers, raster state, surface copies, scan buffers, presentation).
//
// Operation for operation equivalent of the native graphics back-end gx2_render.cpp;
// comments name the GPU behaviour being reproduced. Draw submission, programs,
// pipelines and queries are in gx2_render_vk_draw.cpp; draw capture and
// replay in gx2_render_vk_capture.cpp.
#include "gx2_render.h"
#include "gx2_pm4_state.h"
#include "gx2_detile.h"
#include "gx2_surface.h"
#include "gx2_surface_layout.h"
#include "r700_to_glsl.h"
#include "gx2_shader_program.h"
#include "gx2_perf.h"
#include "gx2_write_watch.h"
#include "gx2_native_context.h"
#include "vk/gx2_render_vk_state.h"
#include "vk/gx2_vk_shader.h"
#include "gx2_host_memory.h"
#include "../os_common.h"
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_vulkan.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <ranges>
#include <type_traits>
#include <utility>

using namespace gx2r;

size_t gx2r_vertex_cache_entries();   // gx2_render_vk_draw.cpp
size_t gx2r_native_query_count();
std::pair<uint64_t, uint64_t> gx2_dl_recording_counts();   // gx2_display_list.cpp
std::array<uint64_t, 9> gx2_dl_maintenance_counts();

namespace gx2r {
State& state() { static State s; return s; }
Counters& counters() { static Counters c; return c; }
}  // namespace gx2r

namespace {
State& S = state();
Counters& C = counters();

template<class Map> auto image_views(const Map& map, uint32_t image) {
    auto& cache = []() -> auto& {
        if constexpr (std::is_same_v<typename Map::mapped_type, ColorTarget>) return S.color_ranges;
        else return S.depth_ranges;
    }();
    if (cache.map_size != map.size()) {
        cache.ranges.clear();
        cache.map_size = map.size();
    }
    auto [it, inserted] = cache.ranges.try_emplace(image);
    if (inserted) {
        ++C.image_view_searches;
        ColorImageKey first{}; first[0] = image;
        const auto begin = map.lower_bound(first);
        auto end = begin;
        while (end != map.end() && end->first[0] == image) ++end;
        it->second = {begin, end};
    }
    // Includes empty ranges: most guest textures never have native targets.
    // Keep map order for overlapping historical mip/slice views.
    return std::ranges::subrange(it->second.first, it->second.second);
}

// 64-bit content hash with four independent lanes (same as gx2_render.cpp).
struct ProgramKeyHash {
    uint64_t lane[4] = {0x60EA27EEADC0B5D6ull, 0xC2B2AE3D27D4EB4Full, 0x0ull, 0x61C8864E7A143579ull};
    static uint64_t rotl(uint64_t v, int r) { return (v << r) | (v >> (64 - r)); }
    static uint64_t round(uint64_t acc, uint64_t input) { acc += input * 0xC2B2AE3D27D4EB4Full; acc = rotl(acc, 31); return acc * 0x9E3779B185EBCA87ull; }
    void add(const void* data, size_t bytes) {
        const auto* p = static_cast<const uint8_t*>(data);
        lane[0] = round(lane[0], bytes);
        size_t i = 0;
        for (; i + 32 <= bytes; i += 32) {
            uint64_t w[4]; std::memcpy(w, p + i, 32);
            lane[0] = round(lane[0], w[0]); lane[1] = round(lane[1], w[1]); lane[2] = round(lane[2], w[2]); lane[3] = round(lane[3], w[3]);
        }
        unsigned l = 1;
        for (; i + 8 <= bytes; i += 8, l = (l + 1) & 3) { uint64_t w; std::memcpy(&w, p + i, 8); lane[l] = round(lane[l], w); }
        if (i < bytes) { uint64_t w = 0; std::memcpy(&w, p + i, bytes - i); lane[l] = round(lane[l], w); }
    }
    uint64_t finish() const {
        uint64_t h = rotl(lane[0], 1) + rotl(lane[1], 7) + rotl(lane[2], 12) + rotl(lane[3], 18);
        h ^= h >> 33; h *= 0xC2B2AE3D27D4EB4Full; h ^= h >> 29; h *= 0x165667B19E3779F9ull; h ^= h >> 32;
        return h;
    }
};

// RBREW_PERF detail (same report as the GPU back-end).
struct PerfFlush { uint64_t calls = 0, marked = 0; };
struct PerfHashed { uint64_t calls = 0, bytes = 0; uint32_t format = 0, width = 0, height = 0, dim = 0; };
std::mutex s_perf_detail_mutex;
std::map<std::pair<uint32_t, uint32_t>, PerfFlush> s_perf_flushes;
std::map<uint32_t, PerfHashed> s_perf_hashed;
template<class Map, class Less> auto perf_top(Map& map, Less less, size_t count) {
    std::vector<std::pair<typename Map::key_type, typename Map::mapped_type>> rows(map.begin(), map.end());
    std::sort(rows.begin(), rows.end(), less);
    if (rows.size() > count) rows.resize(count);
    map.clear();
    return rows;
}
#ifndef RBREW_DISABLE_LOGGING
// [perf-mem]: what accumulates over a session (never-freed caches, display
// lists, memory), plus per-frame alias probes and display-list maintenance.
// Written with fprintf: it must reach the silent game's performance log.
void perf_memory_report(FILE* f, unsigned frames) {
    // Cumulative counters, reported as deltas since the previous report.
    static std::array<uint64_t, 9> last{};
    static uint64_t last_probes = 0;
    const auto [lists_recorded, lists_resident] = gx2_dl_recording_counts();
    const auto now = gx2_dl_maintenance_counts();
    auto delta = [&](size_t i) { return (unsigned long long)(now[i] - last[i]); };
    const Gx2HostMemory m = gx2_host_memory();
    const auto gpu_memory = gx2vk::memory_usage();
    std::fprintf(f, "[perf-mem] private %llu MB ws %llu MB gpu %llu MB (mapped %llu MB) gpu_host %llu MB | lists %llu/%llu (%llu MB) reused %llu swept %llu (%llu sweeps, %.1f ms%s)"
                    " evict %llu (%.2f ms) restore %llu | cb %zu depth %zu tex %zu views %zu probes/frame %.0f | queries %zu vbufs %zu\n",
                 (unsigned long long)(m.private_bytes >> 20), (unsigned long long)(m.working_set >> 20),
                 (unsigned long long)(gpu_memory.device_local >> 20),
                 (unsigned long long)(gpu_memory.host_visible_device_local >> 20),
                 (unsigned long long)(gpu_memory.host_only >> 20), (unsigned long long)lists_resident,
                 (unsigned long long)lists_recorded, (unsigned long long)(now[8] >> 20), delta(3), delta(5), delta(4),
                 (now[6] - last[6]) / 1e6, now[7] != last[7] ? ", inconclusive" : "", delta(0), (now[2] - last[2]) / 1e6, delta(1),
                 S.cb_map.size(), S.depth_map.size(), S.tex_map.size(), S.color_views.size() + S.depth_views.size(),
                 double(C.alias_probes - last_probes) / frames, gx2r_native_query_count(), gx2r_vertex_cache_entries());
    last = now; last_probes = C.alias_probes;
}
void perf_detail_report(FILE* f, unsigned frames) {
    perf_memory_report(f, frames);
    std::lock_guard lock(s_perf_detail_mutex);
    RBREW_LOGF(f, "[perf-flush]");
    for (const auto& [range, flush] : perf_top(s_perf_flushes, [](const auto& a, const auto& b) { return a.second.marked > b.second.marked; }, 8))
        RBREW_LOGF(f, " %08X+%X:%.1f/%.1f", range.first, range.second, double(flush.calls) / frames, double(flush.marked) / frames);
    RBREW_LOGF(f, "\n[perf-hashed]");
    for (const auto& [image, h] : perf_top(s_perf_hashed, [](const auto& a, const auto& b) { return a.second.bytes > b.second.bytes; }, 10))
        RBREW_LOGF(f, " %08X:%.0fKiB/%.1f fmt=%X %ux%u d%u", image, h.bytes / 1024.0 / frames, double(h.calls) / frames, h.format, h.width, h.height, h.dim);
    const auto checks=gx2_write_watch_stats();
    RBREW_LOGF(f, " [vk] submissions=%llu barriers=%llu instances=%llu pipelines=%llu write_checks=%llu write_queries=%llu write_reused=%llu\n",
                 (unsigned long long)gx2vk::submission_count(), (unsigned long long)gx2vk::barrier_count(),
                 (unsigned long long)C.rendering_instances, (unsigned long long)C.pipelines,
                 (unsigned long long)checks.ranges,(unsigned long long)checks.queries,(unsigned long long)checks.reused);
}
#endif

bool texture_content_hash(uint8_t* mem, const uint8_t* data, uint64_t& hash) {
    Gx2PerfScope perf_scope(kPerfTexHash);
    ++C.texture_hashes;
    const uint64_t total = uint64_t(descriptor_word(data, GX2SURF_IMAGE_SIZE)) + descriptor_word(data, GX2SURF_MIPMAP_SIZE);
    gx2_perf_bytes(kBytesTexHash, total);
    if (gx2_perf_enabled()) {
        std::lock_guard lock(s_perf_detail_mutex);
        auto& h = s_perf_hashed[descriptor_word(data, GX2SURF_IMAGE)];
        ++h.calls; h.bytes += total; h.format = descriptor_word(data, GX2SURF_FORMAT);
        h.width = descriptor_word(data, GX2SURF_WIDTH); h.height = descriptor_word(data, GX2SURF_HEIGHT); h.dim = descriptor_word(data, GX2SURF_DIM);
    }
    ProgramKeyHash hasher;
    for (unsigned offset : {GX2SURF_IMAGE_SIZE, GX2SURF_MIPMAP_SIZE}) {
        const uint32_t bytes = descriptor_word(data, offset), address = descriptor_word(data, offset + 4);
        if (!bytes) continue;
        if (!address || !rbrew_memory_accessible(mem, address, bytes)) return false;
        hasher.add(mem + address, bytes);
    }
    hash = hasher.finish();
    return true;
}
uint64_t texture_write_generation(uint8_t* mem, const uint8_t* data) {
    uint64_t newest = 0;
    for (unsigned offset : {GX2SURF_IMAGE_SIZE, GX2SURF_MIPMAP_SIZE}) {
        const uint32_t bytes = descriptor_word(data, offset), address = descriptor_word(data, offset + 4);
        if (!bytes) continue;
        if (!address || !rbrew_memory_accessible(mem, address, bytes)) return kGx2WriteUntracked;
        const uint64_t written = gx2_write_generation(mem, address, bytes);
        if (written == kGx2WriteUntracked) return written;
        newest = std::max(newest, written);
    }
    return newest;
}

uint32_t gl_target_index(uint32_t target) {
    switch (target) {
    case wire::TEXTURE_2D_ARRAY: return 1;
    case wire::TEXTURE_3D: return 2;
    case wire::TEXTURE_CUBE_MAP_ARRAY: return 3;
    default: return 0;
    }
}
void bind_unit(uint32_t unit, TexObject* object) {
    if (unit < S.units.size() && object) S.units[unit][gl_target_index(object->target)] = object;
}

// Default parameters the GPU back-end sets on its texture objects.
TexParams render_target_params(bool integer) {
    TexParams p;
    p.min_filter = p.mag_filter = integer ? wire::NEAREST : wire::LINEAR;
    p.wrap_s = p.wrap_t = wire::CLAMP_TO_EDGE;
    p.max_level = 0;
    return p;
}

VkClearColorValue zero_color() { return VkClearColorValue{}; }
}  // namespace

// ---------------------------------------------------------------------------
// Formats (gx2_wire_format)
// ---------------------------------------------------------------------------
Format gx2r::gx2_vk_format(uint32_t f) {
    Format r;
    auto set = [&](VkFormat format, uint32_t bpp, uint32_t internal, uint32_t wire_format, uint32_t wire_type) {
        r.format = r.attachment = format; r.bpp = bpp; r.wire_internal = internal; r.wire_format = wire_format; r.wire_type = wire_type;
    };
    auto compressed = [&](VkFormat format, uint32_t block, uint32_t internal, uint32_t wire_format) {
        r.format = r.attachment = format; r.bpp = 0; r.compressed = true; r.block = block; r.wire_internal = internal; r.wire_format = wire_format;
        r.wire_type = wire::UNSIGNED_BYTE;
    };
    switch (f) {
    case GX2FMT_UNORM_R8: set(VK_FORMAT_R8_UNORM, 1, 0x8229, wire::RED, wire::UNSIGNED_BYTE); break;
    case 0x0005u: set(VK_FORMAT_R16_UNORM, 2, 0x822A, wire::RED, 0x1403); break;
    case GX2FMT_UNORM_R8_G8: set(VK_FORMAT_R8G8_UNORM, 2, 0x822B, wire::RG, wire::UNSIGNED_BYTE); break;
    case GX2FMT_UNORM_R8_G8_B8_A8: set(VK_FORMAT_R8G8B8A8_UNORM, 4, wire::RGBA8, wire::RGBA, wire::UNSIGNED_BYTE); break;
    case 0x0122u: set(VK_FORMAT_R32G32B32A32_UINT, 16, 0x8D70, wire::RGBA_INTEGER, wire::UNSIGNED_INT); r.integer = true; break;
    case 0x011Fu: set(VK_FORMAT_R16G16B16A16_UINT, 8, 0x8D76, wire::RGBA_INTEGER, 0x1403); r.integer = true; break;
    case 0x0322u: set(VK_FORMAT_R32G32B32A32_SINT, 16, 0x8D82, wire::RGBA_INTEGER, wire::INT); r.integer = true; break;
    case GX2FMT_SRGB_R8_G8_B8_A8:
        // GPU renders into SRGB8_ALPHA8 with FRAMEBUFFER_SRGB disabled (raw
        // writes) and decodes when sampling: SRGB image, UNORM attachment view.
        set(VK_FORMAT_R8G8B8A8_SRGB, 4, 0x8C43, wire::RGBA, wire::UNSIGNED_BYTE);
        r.attachment = VK_FORMAT_R8G8B8A8_UNORM; r.srgb = true; break;
    case GX2FMT_UNORM_R5_G6_B5: set(VK_FORMAT_R5G6B5_UNORM_PACK16, 2, wire::RGB, wire::RGB, 0x8363); break;
    case GX2FMT_UNORM_BC1: compressed(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8, 0x83F1, wire::RGBA); break;
    case GX2FMT_UNORM_BC2: compressed(VK_FORMAT_BC2_UNORM_BLOCK, 16, 0x83F2, wire::RGBA); break;
    case GX2FMT_UNORM_BC3: compressed(VK_FORMAT_BC3_UNORM_BLOCK, 16, 0x83F3, wire::RGBA); break;
    case 0x0431u: compressed(VK_FORMAT_BC1_RGBA_SRGB_BLOCK, 8, 0x8C4D, wire::RGBA); break;
    case 0x0432u: compressed(VK_FORMAT_BC2_SRGB_BLOCK, 16, 0x8C4E, wire::RGBA); break;
    case 0x0433u: compressed(VK_FORMAT_BC3_SRGB_BLOCK, 16, 0x8C4F, wire::RGBA); break;
    case GX2FMT_UNORM_BC4: compressed(VK_FORMAT_BC4_UNORM_BLOCK, 8, 0x8DBB, wire::RED); break;
    case 0x0234u: compressed(VK_FORMAT_BC4_SNORM_BLOCK, 8, 0x8DBC, wire::RED); break;
    case GX2FMT_UNORM_BC5: compressed(VK_FORMAT_BC5_UNORM_BLOCK, 16, 0x8DBD, wire::RG); break;
    case 0x0235u: compressed(VK_FORMAT_BC5_SNORM_BLOCK, 16, 0x8DBE, wire::RG); break;
    case 0x0019u: set(VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, 0x8059, wire::RGBA, 0x8368); break;
    // 0x0419 = 0x0019 with the GX2 sRGB flag (bit 10). Vulkan has no sRGB
    // A2B10G10R10 format, so storage stays UNORM: renders write raw values
    // and the scan/present paths apply the transfer when the scan format
    // declares sRGB, as the GPU back-end does with FRAMEBUFFER_SRGB.
    case 0x0419u: set(VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, 0x8059, wire::RGBA, 0x8368); break;
    case GX2FMT_FLOAT_D24_S8:
        set(VK_FORMAT_D24_UNORM_S8_UINT, 4, 0x88F0, wire::DEPTH_STENCIL, 0x84FA); r.depth = true;
        if (gx2vk::ready() && !gx2vk::caps().d24s8) r.format = r.attachment = VK_FORMAT_D32_SFLOAT_S8_UINT;
        break;
    case GX2FMT_FLOAT_R11_G11_B10: set(VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4, 0x8C3A, wire::RGB, 0x8C3B); break;
    case GX2FMT_FLOAT_R16: set(VK_FORMAT_R16_SFLOAT, 2, 0x822D, wire::RED, wire::HALF_FLOAT); break;
    case GX2FMT_FLOAT_R16_G16: set(VK_FORMAT_R16G16_SFLOAT, 4, 0x822F, wire::RG, wire::HALF_FLOAT); break;
    case GX2FMT_FLOAT_R16_G16_B16_A16: set(VK_FORMAT_R16G16B16A16_SFLOAT, 8, 0x881A, wire::RGBA, wire::HALF_FLOAT); break;
    case GX2FMT_FLOAT_R32: set(VK_FORMAT_R32_SFLOAT, 4, 0x822E, wire::RED, wire::FLOAT); break;
    case GX2FMT_FLOAT_R32_G32: set(VK_FORMAT_R32G32_SFLOAT, 8, 0x8230, wire::RG, wire::FLOAT); break;
    case GX2FMT_FLOAT_R32_G32_B32_A32: set(VK_FORMAT_R32G32B32A32_SFLOAT, 16, wire::RGBA32F, wire::RGBA, wire::FLOAT); break;
    default: {
        static std::set<uint32_t> s_seen; static std::mutex s_seen_mutex;
        std::lock_guard lock(s_seen_mutex);
        if (s_seen.insert(f).second) RBREW_LOGF(stderr, "[gx2_render] unknown GX2SurfaceFormat 0x%04X, using RGBA8\n", f);
        set(VK_FORMAT_R8G8B8A8_UNORM, 4, wire::RGBA8, wire::RGBA, wire::UNSIGNED_BYTE);
        break;
    }
    }
    return r;
}
uint32_t gx2_render_format_block_bytes(uint32_t gx2_fmt) { return gx2_vk_format(gx2_fmt).block; }

ColorImageKey gx2r::color_image_key(uint32_t addr, uint8_t* mem, const uint8_t* descriptor) {
    const auto* data = descriptor ? descriptor : mem + addr;
    return {descriptor_word(data, GX2SURF_IMAGE), descriptor_word(data, GX2SURF_FORMAT),
            descriptor_word(data, GX2SURF_WIDTH), descriptor_word(data, GX2SURF_HEIGHT),
            descriptor_word(data, GX2SURF_PITCH), descriptor_word(data, GX2CB_VIEW_MIP),
            descriptor_word(data, GX2CB_VIEW_FIRST_SLICE), descriptor_word(data, GX2CB_VIEW_NUM_SLICES),
            descriptor_word(data, GX2SURF_AA)};
}
TextureKey gx2r::texture_image_key(uint8_t* mem, const uint8_t* data) {
    TextureLayout layout{};
    for (unsigned i = 0; i < layout.size(); ++i) layout[i] = descriptor_word(data, i * 4);
    return {reinterpret_cast<uintptr_t>(mem), layout};
}

uint32_t gx2r::gx2_compare_to_gl(uint32_t f) {
    static const uint32_t tbl[] = {wire::NEVER, wire::LESS, wire::EQUAL, wire::LEQUAL, wire::GREATER, wire::NOTEQUAL, wire::GEQUAL, wire::ALWAYS};
    return f < 8 ? tbl[f] : wire::ALWAYS;
}
VkCompareOp gx2r::gl_compare_to_vk(uint32_t f) {
    switch (f) {
    case wire::NEVER: return VK_COMPARE_OP_NEVER; case wire::LESS: return VK_COMPARE_OP_LESS;
    case wire::EQUAL: return VK_COMPARE_OP_EQUAL; case wire::LEQUAL: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case wire::GREATER: return VK_COMPARE_OP_GREATER; case wire::NOTEQUAL: return VK_COMPARE_OP_NOT_EQUAL;
    case wire::GEQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL; default: return VK_COMPARE_OP_ALWAYS;
    }
}
VkFilter gx2r::gl_filter(uint32_t f) {
    return (f == wire::LINEAR || f == wire::LINEAR_MIPMAP_NEAREST || f == wire::LINEAR_MIPMAP_LINEAR) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
}

static uint32_t gx2_blend_factor(uint32_t m) {
    switch (m) {
    case 0: return wire::ZERO; case 1: return wire::ONE; case 2: return wire::SRC_COLOR; case 3: return wire::ONE_MINUS_SRC_COLOR;
    case 4: return wire::SRC_ALPHA; case 5: return wire::ONE_MINUS_SRC_ALPHA; case 6: return wire::DST_ALPHA; case 7: return wire::ONE_MINUS_DST_ALPHA;
    case 8: return wire::DST_COLOR; case 9: return wire::ONE_MINUS_DST_COLOR; case 10: return wire::SRC_ALPHA_SATURATE;
    case 13: return wire::CONSTANT_ALPHA; case 14: return wire::ONE_MINUS_CONSTANT_ALPHA;
    case 19: return wire::CONSTANT_COLOR; case 20: return wire::ONE_MINUS_CONSTANT_COLOR;
    default: return wire::ONE;
    }
}
static uint32_t gx2_blend_equation(uint32_t c) {
    switch (c) {
    case 1: return wire::FUNC_SUBTRACT; case 2: return wire::FUNC_REVERSE_SUBTRACT; case 3: return wire::MIN; case 4: return wire::MAX;
    default: return wire::FUNC_ADD;
    }
}

// ---------------------------------------------------------------------------
// Image helpers (outside a rendering instance)
// ---------------------------------------------------------------------------
void gx2r::upload_image_level(gx2vk::Image& image, uint32_t level, uint32_t first_layer, uint32_t layers,
                              uint32_t width, uint32_t height, uint32_t depth, const uint8_t* data, size_t bytes,
                              VkImageAspectFlags aspect) {
    if (!image.image || !data || !bytes) return;
    end_rendering();
    auto span = gx2vk::upload(bytes, 16);
    if (!span.data) {
        TXDIAG(txdiag::event("upload_failed","\"image\":%llu,\"mip\":%u,\"bytes\":%zu",txdiag::handle(image.image),level,bytes));
        return;
    }
    TXDIAG(txdiag::upload(image,level,first_layer,layers,data,bytes,txdiag::handle(span.buffer),span.offset));
    std::memcpy(span.data, data, bytes);
    VkCommandBuffer cb = gx2vk::cmd();
    gx2vk::barrier_if_dirty();
    VkBufferImageCopy region{};
    region.bufferOffset = span.offset;
    region.imageSubresource = {aspect ? aspect : (image.aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : image.aspect),
                               level, image.desc.type == VK_IMAGE_TYPE_3D ? 0u : first_layer, image.desc.type == VK_IMAGE_TYPE_3D ? 1u : layers};
    region.imageExtent = {width, height, image.desc.type == VK_IMAGE_TYPE_3D ? std::max(1u, depth) : 1u};
    vkCmdCopyBufferToImage(cb, span.buffer, image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    gx2vk::note_gpu_write(image);
}
bool gx2r::shadow_log() {
    static const bool on = [] {
        const char* v = rbrew_diagnostic_env("RBREW_SHADOW_LOG");
        return v && *v && *v != '0';
    }();
    return on;
}

void gx2r::clear_image_color(gx2vk::Image& image, const VkClearColorValue& value) {
    if (!image.image) return;
    end_rendering();
    VkCommandBuffer cb = gx2vk::cmd();
    gx2vk::barrier_if_dirty();
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    vkCmdClearColorImage(cb, image.image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
    gx2vk::note_gpu_write(image);
}
void gx2r::clear_image_depth(gx2vk::Image& image, float depth, uint32_t stencil) {
    if (!image.image) return;
    end_rendering();
    VkCommandBuffer cb = gx2vk::cmd();
    gx2vk::barrier_if_dirty();
    const VkClearDepthStencilValue value{depth, stencil};
    const VkImageSubresourceRange range{image.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    vkCmdClearDepthStencilImage(cb, image.image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
    gx2vk::note_gpu_write(image);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void gx2_render_init() {
    TXDIAG(++txdiag::journal().epoch;txdiag::event("renderer_init"));
    // gx2.rpl FUN_02007718 (GX2 initialization): viewport transform enabled,
    // W input in floating point. The translator uses this state at exports.
    S.regs[0xA206] = 0x43F;
#ifndef RBREW_DISABLE_LOGGING
    if (gx2_perf_enabled()) g_gx2_perf_detail = perf_detail_report;
#endif
    if (!gx2vk::ready()) {
        if (!gx2vk::init(static_cast<SDL_Window*>(gx2_native_window()))) {
            // No silent fallback to another API: the Vulkan build only renders with Vulkan.
            RBREW_LOGF(stderr, "[gx2_render] FATAL: Vulkan initialization failed; this build has no other graphics backend\n");
            std::fflush(stderr);
            std::exit(3);
        }
    }
    if (const char* dir = std::getenv("RBREW_VK_CACHE_DIR")) gx2vk::shader_set_cache_dir(dir);
    gx2vk::set_submit_hooks(before_submit, nullptr);
    gx2vk::set_leave_rendering_hook(end_rendering);
    S.sampler_max_anisotropy = gx2vk::caps().anisotropy ? gx2vk::caps().max_anisotropy : 1.0f;
    // Default GPU state set by gx2_render.cpp's init: depth test on, LEQUAL,
    // back-face culling, CCW, no blend, no scissor.
    S.raster = RasterState{};
    S.raster.depth_test = true;
    S.raster.depth_write = true;
    S.raster.depth_func = wire::LEQUAL;
    S.raster.cull = true;
    S.raster.cull_face = wire::BACK;
    S.raster.front_face = wire::CCW;
    // GPU's initial viewport is the default framebuffer: the window drawable.
    if (auto* window = static_cast<SDL_Window*>(gx2_native_window())) {
        int w = 0, h = 0;
        SDL_Vulkan_GetDrawableSize(window, &w, &h);
        S.raster.viewport[2] = w; S.raster.viewport[3] = h;
    }
    RBREW_LOGF(stderr, "[gx2_render] Vulkan rendering back-end initialised (%s)\n", gx2vk::caps().device_name.c_str());
}

// GX2SetSpecialState(ClearColorAsDepth = 8): Gambit's GX2UTClearRectOp
// (02C6A200) clears an R32F color buffer by binding a depth buffer over the
// same surface and drawing the clear value as depth (func ALWAYS). Latte's DB
// writes those bytes into the color surface; natively the depth and color
// views are separate images, so publish the depth writes into the aliased
// color view (same surface key) when the state ends.
namespace {
bool s_clear_color_as_depth = false;
std::vector<ColorImageKey> s_color_as_depth_keys;
gx2vk::Buffer s_alias_staging;
}

static void copy_depth_to_color_alias(DepthTarget& depth, ColorTarget& color) {
    const VkFormat df = depth.format, cf = color.image.desc.format;
    const uint32_t bpp = df == VK_FORMAT_D32_SFLOAT && cf == VK_FORMAT_R32_SFLOAT ? 4 :
                         df == VK_FORMAT_D16_UNORM && cf == VK_FORMAT_R16_UNORM ? 2 : 0;
    if (!bpp || depth.width != color.image.desc.width || depth.height != color.image.desc.height) return;
    const VkDeviceSize bytes = VkDeviceSize(depth.width) * depth.height * bpp;
    if (s_alias_staging.size < bytes) {
        if (s_alias_staging.buffer) gx2vk::destroy_buffer(s_alias_staging);
        s_alias_staging = gx2vk::create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                               gx2vk::MemoryUse::DeviceLocal);
        if (!s_alias_staging.buffer) return;
    }
    end_rendering();
    const auto cb = gx2vk::cmd();
    gx2vk::barrier_if_dirty();
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    region.imageExtent = {depth.width, depth.height, 1};
    vkCmdCopyImageToBuffer(cb, depth.image.image, VK_IMAGE_LAYOUT_GENERAL, s_alias_staging.buffer, 1, &region);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vkCmdCopyBufferToImage(cb, s_alias_staging.buffer, color.image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    gx2vk::note_gpu_write(color.image);
    if (shadow_log())
        RBREW_LOGF(stderr, "[shadow] ClearColorAsDepth: depth %u -> color %u (%ux%u)\n", depth.id, color.id, depth.width, depth.height);
}

void gx2_render_set_special_state(uint32_t state, bool enable) {
    if (state != 8 || !gx2vk::ready()) return;
    if (enable) { s_clear_color_as_depth = true; s_color_as_depth_keys.clear(); return; }
    if (!s_clear_color_as_depth) return;
    s_clear_color_as_depth = false;
    for (const auto& key : s_color_as_depth_keys) {
        const auto depth = S.depth_map.find(key);
        const auto color = S.cb_map.find(key);
        if (depth != S.depth_map.end() && color != S.cb_map.end()) copy_depth_to_color_alias(depth->second, color->second);
    }
    s_color_as_depth_keys.clear();
}

void gx2_render_shutdown() {
    TXDIAG(txdiag::event("renderer_shutdown"));
    if (!gx2vk::ready()) return;
    end_rendering();
    gx2vk::finish();
    TXDIAG(txdiag::drain_probes(true));
    s_clear_color_as_depth = false; s_color_as_depth_keys.clear();
    if (s_alias_staging.buffer) gx2vk::destroy_buffer(s_alias_staging);
    s_alias_staging = {};
    shutdown_draw_state();
    extern void gx2r_shutdown_capture_state();
    gx2r_shutdown_capture_state();
    for (auto& [key, color] : S.cb_map) gx2vk::destroy_image(color.image);
    for (auto& [key, depth] : S.depth_map) gx2vk::destroy_image(depth.image);
    for (auto& [key, texture] : S.tex_map) {
        TXDIAG(txdiag::retire("texture_entry",txdiag::handle(&texture),gx2vk::recording_serial());
            txdiag::destroyed("texture_entry",txdiag::handle(&texture),txdiag::resource_id("texture_entry",txdiag::handle(&texture)),gx2vk::known_completed_serial()));
        gx2vk::destroy_image(texture.image);
    }
    extern void gx2r_shutdown_scan_images();
    gx2r_shutdown_scan_images();
    S = State{};
    gx2vk::shader_shutdown();
    gx2vk::shutdown();
}

// ---------------------------------------------------------------------------
// Framebuffer composition
// ---------------------------------------------------------------------------
static void apply_channel_masks() {
    for (unsigned i = 0; i < 8; ++i) S.raster.color_mask[i] = uint8_t(S.color_write ? (S.target_masks >> (4 * i)) & 15u : 0);
}
uint64_t gx2_render_framebuffer_bind_count() { return C.framebuffer_binds; }

void gx2r::bind_target_framebuffer() {
    ++C.framebuffer_binds;
    // Depth-only passes have their own dimensions (e.g. shadow maps) and
    // must not be clipped to the last color target still in command state.
    if (!S.target_masks && S.depth_target) {
        Framebuffer fb;
        fb.depth = S.depth_target; fb.depth_only = true; fb.valid = true;
        S.framebuffer = fb;
        S.current_fbo_id = S.depth_target->id;
        note_framebuffer_changed();
        return;
    }
    auto* base = S.color_targets[0];
    if (!base) return;   // GPU keeps the previous framebuffer bound
    Framebuffer fb;
    fb.valid = true;
    for (unsigned i = 0; i < 8; ++i) {
        auto* target = S.color_targets[i];
        // Disabled stale targets must not constrain the new pass dimensions.
        const bool attached = target && (S.target_masks & (15u << (4 * i)));
        fb.colors[i] = attached ? target : nullptr;
    }
    // Cafe retains the depth binding while depth/stencil testing is disabled,
    // but it is not an active render target (Cemu LatteMRT::GetActiveDepthBufferMask).
    if (S.depth_target && (S.raster.depth_test || S.raster.stencil_test)) fb.depth = S.depth_target;
    S.framebuffer = fb;
    S.current_fbo_id = base->id;
    note_framebuffer_changed();
}

void gx2_render_set_target_channel_masks(uint32_t masks) {
    S.regs[0xA08E] = masks;
    S.target_masks = masks; apply_channel_masks(); bind_target_framebuffer();
}

static bool color_format_renderable(VkFormat format) {
    return gx2vk::format_supports(format, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
}

void gx2_render_set_color_buffer(uint32_t cb_addr, uint8_t* mem, const uint8_t* descriptor, uint32_t target) {
    static unsigned s_set_cb_logs;
    if (s_set_cb_logs < 20) { ++s_set_cb_logs; RBREW_LOGF(stderr, "[gx2_render] GX2SetColorBuffer cb=0x%08X\n", cb_addr); }
    if (!gx2vk::ready() || target >= 8) return;
    if (!cb_addr) { S.color_targets[target] = nullptr; bind_target_framebuffer(); return; }
    const auto* data = descriptor ? descriptor : mem + cb_addr;
    auto key = color_image_key(cb_addr, mem, data);
    auto it = S.cb_map.find(key);
    if (it == S.cb_map.end()) {
        uint32_t w = descriptor_word(data, GX2SURF_WIDTH), h = descriptor_word(data, GX2SURF_HEIGHT);
        const uint32_t fmt = descriptor_word(data, GX2SURF_FORMAT);
        if (w == 0) w = 1920;
        if (h == 0) h = 1080;
        w = std::max(1u, w >> std::min(key[5], 31u)); h = std::max(1u, h >> std::min(key[5], 31u));
        ColorTarget e;
        e.width = w; e.height = h; e.gx2_format = fmt; e.fmt = gx2_vk_format(fmt);
        e.id = S.next_object_id++;
        gx2vk::ImageDesc desc;
        desc.format = e.fmt.format;
        desc.width = w; desc.height = h;
        desc.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        const bool renderable = !e.fmt.compressed && !e.fmt.depth && color_format_renderable(e.fmt.attachment);
        if (renderable) desc.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        // sRGB attachment views and the 32-bit-class views (glTextureView)
        // reinterpret the image format.
        if (e.fmt.srgb || fmt == 0x122u || fmt == 0x322u || fmt == 0x823u) desc.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        if (!gx2vk::create_image(e.image, desc)) return;
        // GPU zero-initializes new storage on this driver; keep it deterministic.
        if (!e.fmt.compressed && !e.fmt.depth) clear_image_color(e.image, zero_color());
        e.tex.image = &e.image;
        e.tex.target = wire::TEXTURE_2D;
        e.tex.view_format = e.fmt.format;
        e.tex.levels = 1; e.tex.layers = 1;
        e.tex.integer = e.fmt.integer;
        e.tex.compressed = e.fmt.compressed;
        // A CBEntry is one selected mip/slice, stored at level zero: max level 0.
        e.tex.params = render_target_params(e.fmt.integer);
        e.tex.id = e.id;
        it = S.cb_map.emplace(key, std::move(e)).first;
        it->second.tex.image = &it->second.image;
        RBREW_LOGF(stderr, "[gx2_render] created FBO %u for CB 0x%08X (%ux%u fmt=0x%04X)\n", it->second.id, cb_addr, w, h, fmt);
        // A surface used both as a depth buffer and as a color target is how
        // games render depth data into a color-format shadow map.
        if (shadow_log() && S.depth_map.count(key))
            RBREW_LOGF(stderr, "[shadow] color target shares a depth address: cb=0x%08X %ux%u fmt=%X depthfmt=%X\n", cb_addr, w, h, fmt,
                         S.depth_map.find(key)->second.gx2_format);
    }
    S.color_targets[target] = &it->second;
    TXDIAG(txdiag::target(mem,data,it->second.image,it->second.id,"color"));
    bind_target_framebuffer();
}

void gx2_render_set_depth_buffer(uint32_t db_addr, uint8_t* mem, const uint8_t* descriptor) {
    if (!gx2vk::ready()) return;
    if (g_trace_active) RBREW_LOGF(stderr, "[ftrace] render set depth db=%08X descriptor=%p\n", db_addr, (const void*)descriptor);
    if (!db_addr) { S.depth_target = nullptr; bind_target_framebuffer(); return; }
    const auto* data = descriptor ? descriptor : mem + db_addr;
    const auto key = color_image_key(db_addr, mem, data);
    auto [it, inserted] = S.depth_map.try_emplace(key);
    auto& e = it->second;
    if (inserted) {
        const uint32_t mip = key[5];
        e.width = std::max(1u, key[2] >> std::min(mip, 31u)); e.height = std::max(1u, key[3] >> std::min(mip, 31u));
        const bool float_stencil = key[1] == 0x81Cu;
        const bool stencil = (key[1] & 63u) == 0x11u || float_stencil;
        e.stencil = stencil;
        e.gx2_format = key[1];
        e.format = float_stencil ? VK_FORMAT_D32_SFLOAT_S8_UINT :
                   stencil ? (gx2vk::caps().d24s8 ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT) :
                   key[1] == 0x80Eu ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_D16_UNORM;
        e.id = S.next_object_id++;
        gx2vk::ImageDesc desc;
        desc.format = e.format;
        desc.width = e.width; desc.height = e.height;
        desc.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!gx2vk::create_image(e.image, desc)) { S.depth_map.erase(it); return; }
        clear_image_depth(e.image, 0.0f, 0);
        e.tex.image = &e.image;
        e.tex.target = wire::TEXTURE_2D;
        e.tex.view_format = e.format;
        e.tex.levels = 1; e.tex.layers = 1; e.tex.depth = true;
        // Each native depth view owns one selected mip; NEAREST, max level 0.
        e.tex.params = render_target_params(true);
        e.tex.id = e.id;
        RBREW_LOGF(stderr, "[gx2_render] created depth texture %u for image %08X (%ux%u fmt=%X)\n", e.id, key[0], e.width, e.height, key[1]);
        // The GPU back-end binds the new depth-only FBO while creating it; with
        // no color target bound it stays the current framebuffer (clears
        // reach the new depth image).
        Framebuffer fb;
        fb.depth = &e; fb.depth_only = true; fb.valid = true;
        S.framebuffer = fb;
        S.current_fbo_id = e.id;
        note_framebuffer_changed();
    }
    S.depth_target = &e; bind_target_framebuffer();
    TXDIAG(txdiag::target(mem,data,e.image,e.id,"depth"));
    if (s_clear_color_as_depth && std::find(s_color_as_depth_keys.begin(), s_color_as_depth_keys.end(), key) == s_color_as_depth_keys.end())
        s_color_as_depth_keys.push_back(key);
    static uint32_t s_last_bound = UINT32_MAX;
    if (shadow_log() && s_last_bound != key[0]) {
    s_last_bound = key[0];
    RBREW_LOGF(stderr, "[shadow] bind depth db=%08X %ux%u gx2fmt=%X vkfmt=%d stencil=%d\n", db_addr, e.width, e.height, e.gx2_format,
                 int(e.format), int(e.stencil));
    }
}

// ---------------------------------------------------------------------------
// Clears: GX2 clears ignore the draw colour mask; GPU clears the bound
// framebuffer's attachments within the scissor box.
// ---------------------------------------------------------------------------
// Channel widths of the linear UNORM attachment formats (0 = not UNORM).
// The Vulkan float -> UNORM conversion of clear values may round halfway
// cases down (NVIDIA: 0.5 -> 127); the GPU back-end produced the nearest
// value (128), so clears pass the exactly representable nearest value.
static void unorm_bits(VkFormat f, uint32_t bits[4]) {
    auto set = [&](uint32_t r, uint32_t g, uint32_t b, uint32_t a) { bits[0] = r; bits[1] = g; bits[2] = b; bits[3] = a; };
    set(0, 0, 0, 0);
    switch (f) {
    case VK_FORMAT_R8_UNORM: set(8, 0, 0, 0); break;
    case VK_FORMAT_R8G8_UNORM: set(8, 8, 0, 0); break;
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_A8B8G8R8_UNORM_PACK32: set(8, 8, 8, 8); break;
    case VK_FORMAT_R16_UNORM: set(16, 0, 0, 0); break;
    case VK_FORMAT_R16G16_UNORM: set(16, 16, 0, 0); break;
    case VK_FORMAT_R16G16B16A16_UNORM: set(16, 16, 16, 16); break;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_A2R10G10B10_UNORM_PACK32: set(10, 10, 10, 2); break;
    case VK_FORMAT_R5G6B5_UNORM_PACK16: case VK_FORMAT_B5G6R5_UNORM_PACK16: set(5, 6, 5, 0); break;
    case VK_FORMAT_R5G5B5A1_UNORM_PACK16: case VK_FORMAT_A1R5G5B5_UNORM_PACK16: case VK_FORMAT_B5G5R5A1_UNORM_PACK16: set(5, 5, 5, 1); break;
    case VK_FORMAT_R4G4B4A4_UNORM_PACK16: case VK_FORMAT_B4G4R4A4_UNORM_PACK16: set(4, 4, 4, 4); break;
    default: break;
    }
}
static float quantize_unorm(float v, uint32_t bits) {
    if (!bits || !(v == v)) return v;
    const double max = double((1ull << bits) - 1);
    return float(std::floor(std::clamp(double(v), 0.0, 1.0) * max + 0.5) / max);
}

static void clear_bound(bool color, bool depth, bool stencil) {
    if (g_trace_active)
        RBREW_LOGF(stderr,"[ftrace] clear bound fbo=%u color=%u depth=%u stencil=%u rgba=%g,%g,%g,%g z=%g masks=%08X scissor=%d,%d,%d,%d\n",
            S.current_fbo_id,unsigned(color),unsigned(depth),unsigned(stencil),S.raster.clear_color[0],S.raster.clear_color[1],
            S.raster.clear_color[2],S.raster.clear_color[3],S.raster.clear_depth,S.target_masks,
            S.raster.scissor[0],S.raster.scissor[1],S.raster.scissor[2],S.raster.scissor[3]);
    if (!S.framebuffer.valid || !ensure_rendering()) return;
    const VkRect2D area = render_area();
    VkClearRect rect{};
    rect.layerCount = 1;
    rect.rect = area;
    if (S.raster.scissor_test) {
        const int32_t x0 = std::max(S.raster.scissor[0], 0), y0 = std::max(S.raster.scissor[1], 0);
        const int32_t x1 = std::min<int32_t>(S.raster.scissor[0] + S.raster.scissor[2], int32_t(area.extent.width));
        const int32_t y1 = std::min<int32_t>(S.raster.scissor[1] + S.raster.scissor[3], int32_t(area.extent.height));
        if (x1 <= x0 || y1 <= y0) return;
        rect.rect = {{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
    }
    std::vector<VkClearAttachment> clears;
    if (color) {
        for (unsigned i = 0; i < 8; ++i) {
            auto* target = S.framebuffer.colors[i];
            if (!target) continue;
            VkClearAttachment a{VK_IMAGE_ASPECT_COLOR_BIT, i, {}};
            if (target->fmt.integer) {
                for (unsigned c = 0; c < 4; ++c) {
                    if (target->gx2_format == 0x322u) a.clearValue.color.int32[c] = int32_t(S.raster.clear_color[c]);
                    else a.clearValue.color.uint32[c] = uint32_t(std::max(0.0f, S.raster.clear_color[c]));
                }
            } else {
                uint32_t bits[4];
                unorm_bits(target->fmt.attachment, bits);
                for (unsigned c = 0; c < 4; ++c) a.clearValue.color.float32[c] = quantize_unorm(S.raster.clear_color[c], bits[c]);
            }
            clears.push_back(a);
        }
    }
    if (auto* d = S.framebuffer.depth) {
        VkImageAspectFlags aspect = 0;
        if (depth) aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
        if (stencil && d->stencil) aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
        if (aspect) {
            VkClearAttachment a{aspect, 0, {}};
            a.clearValue.depthStencil = {float(std::clamp(S.raster.clear_depth, 0.0, 1.0)), S.raster.clear_stencil & 255};
            clears.push_back(a);
        }
    }
    if (clears.empty()) return;
    vkCmdClearAttachments(gx2vk::cmd(), uint32_t(clears.size()), clears.data(), 1, &rect);
    for (const auto& clear : clears) {
        if (clear.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) gx2vk::note_gpu_write(S.framebuffer.colors[clear.colorAttachment]->image);
        else gx2vk::note_gpu_write(S.framebuffer.depth->image);
    }
    if (shadow_log() && depth && S.framebuffer.depth)
        RBREW_LOGF(stderr, "[shadow] clear bound depth value=%g stencil=%u %ux%u gx2fmt=%X scissor=%d,%d %ux%u\n",
                     double(S.raster.clear_depth), unsigned(S.raster.clear_stencil & 255), S.framebuffer.depth->width, S.framebuffer.depth->height,
                     S.framebuffer.depth->gx2_format, rect.rect.offset.x, rect.rect.offset.y, rect.rect.extent.width, rect.rect.extent.height);
}

void gx2_render_clear_color(float r, float g, float b, float a) {
    S.raster.clear_color = {r, g, b, a};
    clear_bound(true, false, false);
}
void gx2_render_clear_depth_stencil(float depth, uint8_t stencil, uint32_t clear_mode) {
    if (clear_mode & GX2_CLEAR_FLAGS_DEPTH) S.raster.clear_depth = depth;
    if (clear_mode & GX2_CLEAR_FLAGS_STENCIL) S.raster.clear_stencil = stencil;
    if (clear_mode & (GX2_CLEAR_FLAGS_DEPTH | GX2_CLEAR_FLAGS_STENCIL))
        clear_bound(false, (clear_mode & GX2_CLEAR_FLAGS_DEPTH) != 0, (clear_mode & GX2_CLEAR_FLAGS_STENCIL) != 0);
}
void gx2_render_clear_depth_buffer(uint32_t db_addr, uint8_t* mem, float depth, uint8_t stencil, uint32_t clear_mode,
                                   const uint8_t* recorded) {
    if (!db_addr || !gx2vk::ready()) { gx2_render_clear_depth_stencil(depth, stencil, clear_mode); return; }
    if (clear_mode & GX2_CLEAR_FLAGS_DEPTH) S.raster.clear_depth = depth;
    if (clear_mode & GX2_CLEAR_FLAGS_STENCIL) S.raster.clear_stencil = stencil;
    if (!(clear_mode & (GX2_CLEAR_FLAGS_DEPTH | GX2_CLEAR_FLAGS_STENCIL))) return;
    std::array<uint8_t, GX2DB_SIZEOF> descriptor{};
    std::copy_n(recorded ? recorded : mem + db_addr, descriptor.size(), descriptor.data());
    const auto named_key = color_image_key(db_addr, mem, descriptor.data());
    const uint32_t first_slice = named_key[6];
    const uint32_t surface_layers = std::max(1u, descriptor_word(descriptor.data(), GX2SURF_DEPTH));
    if (first_slice >= surface_layers) return;
    const uint32_t slice_count = std::min(std::max(1u, named_key[7]), surface_layers - first_slice);
    if (g_trace_active)
        RBREW_LOGF(stderr,"[ftrace] clear named depth db=%08X image=%08X fmt=%X mip=%u slice=%u count=%u flags=%X z=%g stencil=%u\n",
            db_addr,named_key[0],named_key[1],named_key[5],first_slice,slice_count,clear_mode,depth,unsigned(stencil));
    const uint64_t end_slice = uint64_t(first_slice) + slice_count;
    // gx2.rpl GX2ClearDepthStencilEx iterates every slice in the named view.
    // Native depth targets store one slice per image: create the individual
    // views now so a later cascade binding inherits this clear, not zero.
    // Keep the descriptor private; clearing does not change guest bindings.
    auto* const bound = S.depth_target;
    const Framebuffer framebuffer = S.framebuffer;
    const uint32_t fbo = S.current_fbo_id;
    gx2_render_set_depth_buffer(db_addr, mem, descriptor.data());
    auto word = [&](unsigned offset, uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) descriptor[offset + i] = uint8_t(value >> (24 - i * 8));
    };
    word(GX2DB_VIEW_NUM_SLICES, 1);
    for (uint32_t slice = 0; slice < slice_count; ++slice) {
        word(GX2DB_VIEW_FIRST_SLICE, first_slice + slice);
        gx2_render_set_depth_buffer(db_addr, mem, descriptor.data());
    }
    S.depth_target = bound;
    S.framebuffer = framebuffer;
    S.current_fbo_id = fbo;
    note_framebuffer_changed();
    end_rendering();
    VkCommandBuffer cb = gx2vk::cmd();
    gx2vk::barrier_if_dirty();
    const VkClearDepthStencilValue value{float(std::clamp(double(depth), 0.0, 1.0)), stencil};
    ColorImageKey first_key{}; first_key[0] = named_key[0];
    for (auto it = S.depth_map.lower_bound(first_key); it != S.depth_map.end() && it->first[0] == named_key[0]; ++it) {
        auto& [key, target] = *it;
        if (!std::equal(key.begin(), key.begin() + 6, named_key.begin()) || key[8] != named_key[8]) continue;
        // Update existing aliases too (including legacy zero-count views),
        // while a subrange clear preserves every slice outside its selection.
        if (key[6] < first_slice || uint64_t(key[6]) + std::max(1u, key[7]) > end_slice || !target.image.image) continue;
        VkImageAspectFlags aspect = 0;
        if (clear_mode & GX2_CLEAR_FLAGS_DEPTH) aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
        if ((clear_mode & GX2_CLEAR_FLAGS_STENCIL) && target.stencil) aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
        if (!aspect) continue;
        const VkImageSubresourceRange range{aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        vkCmdClearDepthStencilImage(cb, target.image.image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
        gx2vk::note_gpu_write(target.image);
    }
    if (shadow_log())
        RBREW_LOGF(stderr, "[shadow] clear named depth db=%08X value=%g stencil=%u mode=%X firstSlice=%u slices=%u gx2fmt=%X\n",
                     db_addr, value.depth, value.stencil, clear_mode, first_slice, slice_count, named_key[1]);
}
void gx2_render_clear_buffers(float r, float g, float b, float a, float depth, uint8_t stencil) {
    S.raster.clear_color = {r, g, b, a}; S.raster.clear_depth = depth; S.raster.clear_stencil = stencil;
    clear_bound(true, true, true);
}
void gx2_render_clear_color_buffer(uint32_t cb_addr, uint8_t* mem, float r, float g, float b, float a,
                                   const uint8_t* recorded) {
    if (!cb_addr || !gx2vk::ready()) { gx2_render_clear_color(r, g, b, a); return; }
    S.raster.clear_color = {r, g, b, a};
    // GX2 clears the named color buffer's whole view, bound or not.
    auto* const bound = S.color_targets[0];
    const Framebuffer framebuffer = S.framebuffer;
    const uint32_t fbo = S.current_fbo_id;
    gx2_render_set_color_buffer(cb_addr, mem, recorded, 0);
    ColorTarget* target = S.color_targets[0];
    S.color_targets[0] = bound;
    S.framebuffer = framebuffer;
    S.current_fbo_id = fbo;
    note_framebuffer_changed();
    if (!target || !target->image.image || target->fmt.compressed) return;
    if (g_trace_active) {
        const auto* d=recorded?recorded:mem+cb_addr;
        RBREW_LOGF(stderr,"[ftrace] clear named color cb=%08X target=%u image=%08X fmt=%X size=%ux%u mip=%u slice=%u count=%u rgba=%g,%g,%g,%g\n",
            cb_addr,target->id,descriptor_word(d,GX2SURF_IMAGE),target->gx2_format,target->width,target->height,
            descriptor_word(d,GX2CB_VIEW_MIP),descriptor_word(d,GX2CB_VIEW_FIRST_SLICE),descriptor_word(d,GX2CB_VIEW_NUM_SLICES),r,g,b,a);
    }
    VkClearColorValue value{};
    if (target->fmt.integer) {
        for (unsigned c = 0; c < 4; ++c) {
            if (target->gx2_format == 0x322u) value.int32[c] = int32_t(S.raster.clear_color[c]);
            else value.uint32[c] = uint32_t(std::max(0.0f, S.raster.clear_color[c]));
        }
    } else {
        uint32_t bits[4];
        unorm_bits(target->fmt.attachment, bits);
        for (unsigned c = 0; c < 4; ++c) value.float32[c] = quantize_unorm(S.raster.clear_color[c], bits[c]);
    }
    clear_image_color(target->image, value);
}

// ---------------------------------------------------------------------------
// Raster state
// ---------------------------------------------------------------------------
static void update_native_viewport() {
    auto f = [](uint32_t r) { float v; const auto word = S.regs[r]; std::memcpy(&v, &word, 4); return v; };
    float x = f(0xA110) - f(0xA10F), y = f(0xA112) + f(0xA111);
    const float w = 2 * f(0xA10F);
    float h = -2 * f(0xA111);
    // Latte always applies z = ZSCALE * (clip.z / clip.w) + ZOFFSET.
    // A 0..1 clip volume therefore uses [offset, offset+scale] as the
    // Vulkan depth range; a -1..1 volume uses [offset-scale, offset+scale].
    const bool half_z = (S.regs[0xA204] & (1u << 19)) != 0;
    const float near_z = f(0xA114) - (half_z ? 0.0f : f(0xA113));
    const float far_z = f(0xA114) + f(0xA113);
    if (g_trace_active) RBREW_LOGF(stderr, "[ftrace] render set viewport %g,%g %gx%g\n", x, y, w, h);
    const bool inverted = h < 0;
    if (inverted) { y += h; h = -h; }
    S.raster.clip_origin = inverted ? wire::LOWER_LEFT : wire::UPPER_LEFT;
    // glViewport truncates to integers and rejects negative sizes.
    const int32_t vw = int32_t(w), vh = int32_t(h);
    if (vw >= 0 && vh >= 0) { S.raster.viewport[0] = int32_t(x); S.raster.viewport[1] = int32_t(y); S.raster.viewport[2] = vw; S.raster.viewport[3] = vh; }
    // glDepthRange clamps to [0, 1].
    S.raster.depth_range[0] = std::clamp(double(near_z), 0.0, 1.0);
    S.raster.depth_range[1] = std::clamp(double(far_z), 0.0, 1.0);
}

void gx2_render_set_viewport(float x, float y, float w, float h, float near_z, float far_z) {
    auto bits = [](float f) { uint32_t v; std::memcpy(&v, &f, 4); return v; };
    // GX2SetViewport packs these words independently of clip-control mode.
    S.regs[0xA10F] = bits(w * 0.5f); S.regs[0xA110] = bits(x + w * 0.5f);
    S.regs[0xA111] = bits(-h * 0.5f); S.regs[0xA112] = bits(y + h * 0.5f);
    S.regs[0xA113] = bits((far_z - near_z) * 0.5f); S.regs[0xA114] = bits((far_z + near_z) * 0.5f);
    update_native_viewport();
}

void gx2_render_set_scissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    auto clamp = [](uint32_t v) { return uint32_t(std::clamp(int32_t(v), 0, 8192)); };
    const uint32_t right = clamp(x + w), bottom = clamp(y + h); x = clamp(x); y = clamp(y);
    S.regs[0xA090] = 0x80000000 | x | (y << 16); S.regs[0xA091] = right | (bottom << 16);
    S.raster.scissor_test = true;
    S.raster.scissor[0] = int32_t(x); S.raster.scissor[1] = int32_t(y);
    S.raster.scissor[2] = int32_t(right >= x ? right - x : 0); S.raster.scissor[3] = int32_t(bottom >= y ? bottom - y : 0);
}

void gx2_render_set_depth_stencil_control(bool depth_test, bool depth_write, uint32_t gx2_cmp) {
    const bool attachments_changed = S.raster.depth_test != depth_test || S.raster.stencil_test;
    S.raster.stencil_test = false;
    S.regs[0xA200] = (uint32_t(depth_test) << 1) | (uint32_t(depth_write) << 2) | ((gx2_cmp & 7) << 4);
    S.raster.depth_test = depth_test;
    if (depth_test) S.raster.depth_func = gx2_compare_to_gl(gx2_cmp);
    S.raster.depth_write = depth_write;
    // Material draws often change the comparison/write mask without changing
    // active targets; only enable transitions reattach the depth image.
    if (attachments_changed) bind_target_framebuffer();
}

void gx2_render_set_polygon_control(uint32_t front_face, bool cull_front, bool cull_back) {
    S.raster.front_face = front_face == GX2_FRONT_FACE_CW ? wire::CW : wire::CCW;
    if (cull_front && cull_back) { S.raster.cull = true; S.raster.cull_face = wire::FRONT_AND_BACK; }
    else if (cull_back) { S.raster.cull = true; S.raster.cull_face = wire::BACK; }
    else if (cull_front) { S.raster.cull = true; S.raster.cull_face = wire::FRONT; }
    else S.raster.cull = false;
}

void gx2r::apply_native_raster_registers(const Gx2RegisterWrite* writes, size_t count) {
    bool mask = false, blend = false, alpha = false, point = false, restart = false, viewport = false, scissor = false,
         stencil = false, polygon = false, offset = false;
    for (size_t i = 0; i < count; ++i) {
        const uint32_t r = writes[i].index;
        mask |= r == 0xA08E; blend |= r >= 0xA105 && r <= 0xA108; alpha |= r == 0xA351; point |= r == 0xA280;
        restart |= r == 0xA103 || r == 0xA2A5; viewport |= r == 0xA204 || (r >= 0xA10F && r <= 0xA114); scissor |= r == 0xA090 || r == 0xA091;
        stencil |= r == 0xA200 || r == 0xA10C || r == 0xA10D; polygon |= r == 0xA205; offset |= r >= 0xA37F && r <= 0xA383;
    }
    auto f = [](uint32_t r) { float v; const uint32_t w = S.regs[r]; std::memcpy(&v, &w, 4); return v; };
    if (mask) gx2_render_set_target_channel_masks(S.regs[0xA08E]);
    if (blend) S.raster.blend_color = {f(0xA105), f(0xA106), f(0xA107), f(0xA108)};
    if (alpha) S.raster.alpha_to_coverage = (S.regs[0xA351] & 1) != 0;
    if (point) S.raster.point_size = std::max(0.125f, float(S.regs[0xA280] >> 16) / 8);
    if (restart) { S.raster.restart_index = S.regs[0xA103]; S.raster.primitive_restart = (S.regs[0xA2A5] & 1) != 0; }
    if (viewport) update_native_viewport();
    if (scissor) {
        const uint32_t tl = S.regs[0xA090], br = S.regs[0xA091];
        const uint32_t x = tl & 0x3FFF, y = (tl >> 16) & 0x3FFF, right = br & 0x3FFF, bottom = (br >> 16) & 0x3FFF;
        S.raster.scissor_test = true;
        S.raster.scissor[0] = int32_t(x); S.raster.scissor[1] = int32_t(y);
        S.raster.scissor[2] = int32_t(right >= x ? right - x : 0); S.raster.scissor[3] = int32_t(bottom >= y ? bottom - y : 0);
    }
    if (stencil) {
        const uint32_t w = S.regs[0xA200]; const bool active = S.raster.depth_test || S.raster.stencil_test;
        S.raster.depth_test = (w & 2) != 0; S.raster.depth_write = (w & 4) != 0; S.raster.depth_func = gx2_compare_to_gl((w >> 4) & 7);
        S.raster.stencil_test = (w & 1) != 0;
        static const uint32_t ops[] = {wire::KEEP, wire::ZERO, wire::REPLACE, wire::INCR, wire::DECR, wire::INVERT, wire::INCR_WRAP, wire::DECR_WRAP};
        for (unsigned face = 0; face < 2; ++face) {
            const bool back = face && (w & 128); const uint32_t maskword = S.regs[0xA10C + unsigned(back)];
            auto& s = face ? S.raster.back : S.raster.front;
            s.func = gx2_compare_to_gl((w >> (back ? 20 : 8)) & 7); s.ref = maskword & 255; s.mask = (maskword >> 8) & 255;
            s.fail = ops[(w >> (back ? 23 : 11)) & 7]; s.zfail = ops[(w >> (back ? 29 : 17)) & 7]; s.zpass = ops[(w >> (back ? 26 : 14)) & 7];
            s.write_mask = (maskword >> 16) & 255;
        }
        if (active != (S.raster.depth_test || S.raster.stencil_test)) bind_target_framebuffer();
    }
    if (polygon) {
        const uint32_t w = S.regs[0xA205]; gx2_render_set_polygon_control((w >> 2) & 1, (w & 1) != 0, (w & 2) != 0);
        S.raster.polygon_offset_fill = (w & 0x1800) != 0;
        S.raster.polygon_offset_line = S.raster.polygon_offset_point = (w & 0x2000) != 0;
    }
    if (offset || polygon) {
        // glPolygonOffsetClamp(factor, units, clamp)
        S.raster.offset_factor = f(0xA380) / 16; S.raster.offset_units = f(0xA381); S.raster.offset_clamp = f(0xA37F);
    }
}

void gx2_render_set_blend_control(bool blend_enable, uint32_t src_color, uint32_t dst_color, uint32_t color_combine,
                                  uint32_t src_alpha, uint32_t dst_alpha, uint32_t alpha_combine) {
    if (blend_enable) {
        S.raster.blend = true;
        S.raster.blend_src_rgb = gx2_blend_factor(src_color); S.raster.blend_dst_rgb = gx2_blend_factor(dst_color);
        S.raster.blend_src_alpha = gx2_blend_factor(src_alpha); S.raster.blend_dst_alpha = gx2_blend_factor(dst_alpha);
        S.raster.blend_eq_rgb = gx2_blend_equation(color_combine); S.raster.blend_eq_alpha = gx2_blend_equation(alpha_combine);
    } else {
        S.raster.blend = false;
    }
}

void gx2_render_set_color_write(bool enable) {
    S.color_write = enable;
    apply_channel_masks();
}

// ---------------------------------------------------------------------------
// Samplers (apply_native_sampler): GPU sampler object parameters per slot
// ---------------------------------------------------------------------------
static void apply_native_sampler(unsigned sampler) {
    auto& p = S.samplers[sampler].params;
    const unsigned stage = sampler / 18, slot = sampler % 18;
    const uint32_t w0 = S.regs[0xF000 + sampler * 3], w1 = S.regs[0xF001 + sampler * 3];
    // Same mapping as the GPU back-end (clamp-to-edge for half-border 4,
    // mirror-clamp-to-border for 5 and 7; see the Vulkan mapping at bind).
    static const uint32_t wraps[] = {wire::REPEAT, wire::MIRRORED_REPEAT, wire::CLAMP_TO_EDGE, wire::MIRROR_CLAMP_TO_EDGE,
                                     wire::CLAMP_TO_EDGE, wire::MIRROR_CLAMP_TO_BORDER, wire::CLAMP_TO_BORDER, wire::MIRROR_CLAMP_TO_BORDER};
    p.wrap_s = wraps[w0 & 7]; p.wrap_t = wraps[(w0 >> 3) & 7]; p.wrap_r = wraps[(w0 >> 6) & 7];
    const bool linear_min = ((w0 >> 12) & 3) != 0, linear_mag = ((w0 >> 9) & 3) != 0;
    const unsigned mip = (w0 >> 17) & 3;
    p.mag_filter = linear_mag ? wire::LINEAR : wire::NEAREST;
    p.min_filter = mip == 0 ? (linear_min ? wire::LINEAR : wire::NEAREST) :
                   mip == 1 ? (linear_min ? wire::LINEAR_MIPMAP_NEAREST : wire::NEAREST_MIPMAP_NEAREST) :
                              (linear_min ? wire::LINEAR_MIPMAP_LINEAR : wire::NEAREST_MIPMAP_LINEAR);
    p.min_lod = float(w1 & 1023) / 64.0f; p.max_lod = float((w1 >> 10) & 1023) / 64.0f;
    p.lod_bias = float(int32_t(w1) >> 20) / 64.0f;
    p.anisotropy = S.sampler_max_anisotropy > 1 ? std::min(S.sampler_max_anisotropy, float(1u << std::min((w0 >> 19) & 7, 4u))) : 1.0f;
    p.border = {};
    switch ((w0 >> 22) & 3) {
    case 1: p.border[3] = 1; break;
    case 2: p.border.fill(1); break;
    case 3: for (unsigned i = 0; i < 4; ++i) { uint32_t w = S.regs[0xA400 + stage * 0x80 + slot * 4 + i]; std::memcpy(&p.border[i], &w, 4); } break;
    }
    p.compare_func = gx2_compare_to_gl((w0 >> 26) & 7);
    p.compare_mode = S.sampler_compare[sampler] ? 1 : 0;
    p.base_level = 0; p.max_level = 1000;
}

void gx2_render_set_sampler_comparison(uint32_t native_slot, bool compare) {
    if (native_slot / 32 >= 3 || native_slot % 32 >= 18) return;
    const unsigned sampler = (native_slot / 32) * 18 + native_slot % 32;
    if (S.sampler_compare[sampler] != compare) {
        S.sampler_compare[sampler] = compare;
        if (S.samplers[sampler].valid) apply_native_sampler(sampler);
    }
}

void gx2_render_apply_registers(const Gx2RegisterWrite* w, size_t n) {
    std::array<bool, 54> samplers{};
    for (size_t i = 0; i < n; ++i) {
        if (w[i].index >= S.regs.size()) continue;
        S.regs[w[i].index] = w[i].value;
        if (w[i].index == 0xA216) S.vs_from_regs = true;
        if (w[i].index == 0xA210) S.ps_from_regs = true;
        if (w[i].index >= 0xF000 && w[i].index < 0xF000 + 54 * 3) {
            const unsigned s = (w[i].index - 0xF000) / 3; samplers[s] = true; S.samplers[s].valid = true;
        }
        for (unsigned stage = 0; stage < 3; ++stage)
            if (w[i].index >= 0xA400 + stage * 0x80 && w[i].index < 0xA400 + stage * 0x80 + 18 * 4)
                samplers[stage * 18 + (w[i].index - 0xA400 - stage * 0x80) / 4] = true;
    }
    for (unsigned s = 0; s < 54; ++s) if (samplers[s] && S.samplers[s].valid) apply_native_sampler(s);
    apply_native_raster_registers(w, n);
}
uint32_t gx2_render_register(uint32_t index) { return index < S.regs.size() ? S.regs[index] : 0; }

// Test/diagnostic view of the native sampler object bound to a unit.
bool gx2_render_vk_sampler_params(uint32_t unit, uint32_t* wrap_s, uint32_t* min_filter, uint32_t* compare) {
    if (unit / 32 >= 3 || unit % 32 >= 18) return false;
    const auto& s = S.samplers[(unit / 32) * 18 + unit % 32];
    if (!s.valid) return false;
    if (wrap_s) *wrap_s = s.params.wrap_s;
    if (min_filter) *min_filter = s.params.min_filter;
    if (compare) *compare = s.params.compare_mode;
    return true;
}

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------
// GX2Invalidate(texture bit): the CPU rewrote [addr, addr+size). Verify both
// allocations at the next bind; unchanged bytes keep their upload.
void gx2_render_invalidate_textures(uint32_t addr, uint32_t size) {
    TXDIAG(txdiag::count(txdiag::Invalidations);
        if(txdiag::sample("invalidate",uint64_t(addr)<<32|size))
            txdiag::event("invalidate","\"address\":%u,\"size\":%u",addr,size));
    Gx2PerfScope perf_scope(kPerfInvalidate);
    const uint64_t end = uint64_t(addr) + size;
    uint64_t marked = 0;
    // Same overlap test as a scan of every texture's image and mip ranges,
    // through an index sorted by range start (called ~200 times per frame).
    if (S.tex_ranges_size != S.tex_map.size()) {
        S.tex_ranges.clear();
        for (auto& [key, entry] : S.tex_map) {
            const auto& layout = key.second;
            for (unsigned offset : {GX2SURF_IMAGE_SIZE, GX2SURF_MIPMAP_SIZE}) {
                const uint32_t bytes = layout[offset / 4], image = layout[offset / 4 + 1];
                if (image) S.tex_ranges.push_back({image, uint64_t(image) + bytes, &entry});
            }
        }
        std::sort(S.tex_ranges.begin(), S.tex_ranges.end(),
                  [](const auto& a, const auto& b) { return a.start < b.start; });
        S.tex_range_max_end.resize(S.tex_ranges.size());
        uint64_t max_end = 0;
        for (size_t i = 0; i < S.tex_ranges.size(); ++i)
            S.tex_range_max_end[i] = max_end = std::max(max_end, S.tex_ranges[i].end);
        S.tex_ranges_size = S.tex_map.size();
    }
    // Ranges starting before `end`, walked down until none can reach `addr`.
    size_t i = size_t(std::partition_point(S.tex_ranges.begin(), S.tex_ranges.end(),
                                           [end](const auto& r) { return r.start < end; }) - S.tex_ranges.begin());
    while (i > 0 && S.tex_range_max_end[i - 1] > addr) {
        const auto& range = S.tex_ranges[--i];
        if (range.end > addr) {
            TXDIAG(if(txdiag::sample("cache_invalidate",range.entry->tex.id))
                txdiag::event("cache_invalidate","\"cache_id\":%u,\"address\":%u,\"size\":%u,\"previous_verify\":%s",range.entry->tex.id,addr,size,range.entry->verify?"true":"false"));
            if (!range.entry->verify) { gx2_perf_count(kPerfTexInvalidated); ++marked; }
            range.entry->verify = true;
        }
    }
    if (gx2_perf_enabled()) {
        std::lock_guard lock(s_perf_detail_mutex);
        auto& flush = s_perf_flushes[{addr, size}];
        ++flush.calls; flush.marked += marked;
    }
}

static bool texture_view_matches(const ColorImageKey& key, const uint8_t* d, bool reinterpret_format = false) {
    // Allocators reuse image addresses: a historical allocation at the same
    // address is not a view of the current image.
    return key[0] == descriptor_word(d, GX2SURF_IMAGE) &&
           (reinterpret_format || key[1] == descriptor_word(d, GX2SURF_FORMAT)) &&
           key[2] == descriptor_word(d, GX2SURF_WIDTH) && key[3] == descriptor_word(d, GX2SURF_HEIGHT) &&
           key[4] == descriptor_word(d, GX2SURF_PITCH) && key[5] == descriptor_word(d, GX2TEX_VIEW_MIP) &&
           key[6] == descriptor_word(d, GX2TEX_VIEW_FIRST_SLICE) &&
           std::max(1u, key[7]) == std::max(1u, descriptor_word(d, GX2TEX_VIEW_NUM_SLICES)) &&
           key[8] == descriptor_word(d, GX2SURF_AA);
}

// (Re)allocates a texture's storage when its shape changes (GPU respecifies
// the texture image on every upload).
static bool ensure_texture_storage(Texture& entry, uint32_t target, VkFormat format, uint32_t w, uint32_t h, uint32_t layers,
                                   uint32_t levels, bool depth_format) {
    const VkImageType type = target == wire::TEXTURE_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    const auto& d = entry.image.desc;
    if (entry.image.image && d.format == format && d.width == w && d.height == h && d.levels == levels &&
        (type == VK_IMAGE_TYPE_3D ? d.depth == layers : d.layers == layers) && d.type == type &&
        ((d.flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) != 0) == (target == wire::TEXTURE_CUBE_MAP_ARRAY))
        return true;
    if (entry.image.image) gx2vk::destroy_image(entry.image);
    gx2vk::ImageDesc desc;
    desc.type = type;
    desc.format = format;
    desc.width = w; desc.height = h;
    desc.depth = type == VK_IMAGE_TYPE_3D ? layers : 1;
    desc.layers = type == VK_IMAGE_TYPE_3D ? 1 : layers;
    desc.levels = levels;
    desc.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!gx2vk::format_is_compressed(format) && !depth_format) desc.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    if (target == wire::TEXTURE_CUBE_MAP_ARRAY) desc.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (!gx2vk::create_image(entry.image, desc)) return false;
    entry.tex.image = &entry.image;
    return true;
}

template<class Target> static bool live_copy_needed(Texture& destination, const Target& source) {
    if (destination.copied_destination != destination.image.content_generation) {
        // Storage recreation or a CPU/GPU upload may overwrite copied slices.
        destination.copied_sources.clear();
        destination.copied_destination = destination.image.content_generation;
    }
    const auto found = destination.copied_sources.find(source.id);
    return found == destination.copied_sources.end() || found->second != source.image.content_generation;
}
template<class Target> static void live_copy_recorded(Texture& destination, const Target& source) {
    TXDIAG(txdiag::count(txdiag::AliasCopies);
        if(txdiag::sample("alias_copy",uint64_t(destination.tex.id)<<32|source.id))
            txdiag::event("alias_copy","\"destination\":%llu,\"source\":%llu,\"source_content\":%llu",txdiag::resource_id("image",txdiag::handle(destination.image.image)),txdiag::resource_id("image",txdiag::handle(source.image.image)),source.image.content_generation));
    gx2vk::note_gpu_write(destination.image);
    destination.copied_destination = destination.image.content_generation;
    destination.copied_sources[source.id] = source.image.content_generation;
}

void gx2_render_upload_texture(uint32_t tex_addr, uint32_t slot, uint8_t* mem, const uint8_t* descriptor) {
    Gx2PerfScope perf_scope(kPerfTexture);
    if (!tex_addr || !gx2vk::ready() || slot >= S.bound_textures.size()) return;
    const auto* data = descriptor ? descriptor : mem + tex_addr;
    TXDIAG(txdiag::count(txdiag::Lookups));
#ifdef RBREW_TEXTURE_DIAGNOSTICS
    txdiag::TextureScope diagnostic_scope{mem,data,tex_addr,slot};
#endif
    S.bound_textures[slot].address = tex_addr;
    std::memcpy(S.bound_textures[slot].descriptor.data(), data, GX2TEX_SIZEOF);
    const uint32_t word4 = descriptor_word(data, 0x90);
    r700_texture_registers(S.regs.data(), data, slot);

    auto& cached = S.texture_entries[slot];
    if (!cached.entry || cached.mem != mem || std::memcmp(cached.layout.data(), data, cached.layout.size()) != 0) {
        ++C.texture_entry_searches;
        cached.entry = &S.tex_map[texture_image_key(mem, data)];
        TXDIAG(txdiag::count(txdiag::Misses));
        cached.mem = mem;
        std::memcpy(cached.layout.data(), data, cached.layout.size());
    }
    auto& entry = *cached.entry;
    if (!entry.tex.id) {
        entry.tex.id = S.next_object_id++;
        TXDIAG(txdiag::create("texture_entry",txdiag::handle(&entry));
            txdiag::event("cache_insert","\"cache_id\":%u,\"cache_uid\":%llu,\"address\":%u",entry.tex.id,txdiag::resource_id("texture_entry",txdiag::handle(&entry)),descriptor_word(data,GX2SURF_IMAGE)));
    } else { TXDIAG(txdiag::count(txdiag::Hits)); }
    const uint32_t w = descriptor_word(data, GX2SURF_WIDTH), h = descriptor_word(data, GX2SURF_HEIGHT);
    const uint32_t fmt = descriptor_word(data, GX2SURF_FORMAT);
    const uint32_t img_ptr = descriptor_word(data, GX2SURF_IMAGE), img_sz = descriptor_word(data, GX2SURF_IMAGE_SIZE);
    const uint32_t dim = descriptor_word(data, GX2SURF_DIM);
    const uint32_t target = dim == 3 ? wire::TEXTURE_CUBE_MAP_ARRAY : dim == 5 ? wire::TEXTURE_2D_ARRAY : dim == 2 ? wire::TEXTURE_3D : wire::TEXTURE_2D;
    const uint32_t first_slice = descriptor_word(data, GX2TEX_VIEW_FIRST_SLICE);
    const uint32_t surface_layers = std::max(1u, descriptor_word(data, GX2SURF_DEPTH));
    const uint32_t layers = target == wire::TEXTURE_2D ? 1 : dim == 2 ? surface_layers :
                            std::min(surface_layers, std::max(1u, descriptor_word(data, GX2TEX_VIEW_NUM_SLICES)));
    const uint32_t mip_levels = std::min(14u, std::max(1u, descriptor_word(data, GX2SURF_MIP_LEVELS)));
    if (w == 0 || h == 0) return;
    // GPU deletes and recreates the texture object when its target changes.
    if (entry.created && entry.tex.target != target) {
        gx2vk::destroy_image(entry.image);
        entry.created = false; entry.image_addr = 0; entry.hash_valid = false;
        entry.tex.image = nullptr; entry.tex.complete_storage = false;
    }
    entry.created = true;
    entry.tex.target = target;

    for (auto& [key, depth] : image_views(S.depth_map, img_ptr)) {
        ++C.alias_probes;
        static const bool depth_alias_probe = rbrew_diagnostic_env("RBREW_DEPTH_ALIAS_PROBE") != nullptr;  // read once: per-draw path
        if (depth_alias_probe) {
            static unsigned reports = 0;
            if (reports++ < 20) RBREW_LOGF(stderr,"[depth-alias-probe] image=%X target=%X layers=%u key=%X,%u,%u,%u,%u,%u,%u,%u texture=%X,%u,%u,%u,%u,%u,%u,%u\n",img_ptr,target,layers,key[1],key[2],key[3],key[4],key[5],key[6],key[7],key[8],fmt,w,h,descriptor_word(data,GX2SURF_PITCH),descriptor_word(data,GX2TEX_VIEW_MIP),first_slice,descriptor_word(data,GX2TEX_VIEW_NUM_SLICES),descriptor_word(data,GX2SURF_AA));
        }
        // A surface cleared through a depth alias (ClearColorAsDepth) and then
        // rendered as color: the newer of the two native views holds the bytes.
        if (const auto color = S.cb_map.find(key);
            color != S.cb_map.end() && color->second.image.content_generation > depth.image.content_generation)
            continue;
        if ((target == wire::TEXTURE_2D || (target == wire::TEXTURE_2D_ARRAY && layers == 1)) && texture_view_matches(key, data)) {
            // A one-slice array can view the live depth image directly. Its
            // surface format may also describe a color texture (e.g. R16),
            // so uploading guest RAM would lose the GPU's shadow depth.
            auto& source = const_cast<DepthTarget&>(depth);
            TexObject* sampled = &source.tex;
            if (target == wire::TEXTURE_2D_ARRAY) {
                auto& view = S.depth_views[{source.id, target, word4}].tex;
                if (!view.id) {
                    view = source.tex;
                    view.id = S.next_object_id++;
                    view.target = target;
                    view.layers = 1;
                }
                sampled = &view;
            }
            sampled->swizzle = word4;
            bind_unit(slot, sampled);
            if (g_trace_active || shadow_log())
                RBREW_LOGF(stderr, "[tex] slot %u samples depth target image %08X (gl tex %u %ux%u fmt %X mip %u)\n",
                             slot, img_ptr, depth.id, depth.width, depth.height, fmt, key[5]);
            return;
        }
    }
    if (target == wire::TEXTURE_2D_ARRAY) {
        // Cascaded shadows render one depth slice at a time, then sample the
        // allocation as an array. Those native single-slice images cannot be
        // bound as a multi-layer view, and guest RAM has none of their GPU writes.
        // Assemble the selected slices into depth storage, just as the color
        // array path below assembles live color targets. In particular GX2 R16
        // used as a depth buffer needs D16 storage for SAMPLE_C, not R16_UNORM.
        const uint32_t first_mip = descriptor_word(data, GX2TEX_VIEW_MIP);
        const uint32_t view_mips = std::max(1u, descriptor_word(data, GX2TEX_VIEW_NUM_MIPS));
        auto matches = [&](const ColorImageKey& key) {
            return key[1] == fmt && key[2] == w && key[3] == h &&
                   key[4] == descriptor_word(data, GX2SURF_PITCH) && key[8] == descriptor_word(data, GX2SURF_AA) &&
                   key[5] < mip_levels && key[5] >= first_mip && key[5] - first_mip < view_mips &&
                   key[6] >= first_slice && key[6] - first_slice < layers && std::max(1u, key[7]) == 1;
        };
        DepthTarget* source = nullptr;
        for (auto& [key, depth] : image_views(S.depth_map, img_ptr)) {
            ++C.alias_probes;
            if (matches(key)) { source = const_cast<DepthTarget*>(&depth); break; }
        }
        if (source) {
            const bool same_storage = entry.image.image && entry.image.desc.format == source->format &&
                                      entry.image.desc.width == w && entry.image.desc.height == h &&
                                      entry.image.desc.layers == layers && entry.image.desc.levels == mip_levels;
            if (!ensure_texture_storage(entry, target, source->format, w, h, layers, mip_levels, true)) return;
            if (!same_storage) clear_image_depth(entry.image, 0.0f, 0);
            entry.tex.view_format = source->format;
            entry.tex.levels = mip_levels; entry.tex.layers = layers;
            entry.tex.depth = true; entry.tex.integer = entry.tex.compressed = false;
            entry.tex.complete_storage = true; entry.tex.swizzle = word4;
            entry.tex.params = render_target_params(true);
            entry.tex.params.base_level = std::min(first_mip, mip_levels - 1);
            entry.tex.params.max_level = entry.tex.params.base_level + std::min(view_mips, mip_levels - entry.tex.params.base_level) - 1;
            bind_unit(slot, &entry.tex);
            for (auto& [key, depth] : image_views(S.depth_map, img_ptr)) {
                if (!matches(key) || depth.format != source->format) continue;
                if (!live_copy_needed(entry, depth)) continue;
                end_rendering();
                const auto cb = gx2vk::cmd();
                gx2vk::barrier_if_dirty();
                VkImageCopy region{};
                region.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
                region.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, key[5], key[6] - first_slice, 1};
                region.extent = {depth.width, depth.height, 1};
                vkCmdCopyImage(cb, depth.image.image, VK_IMAGE_LAYOUT_GENERAL, entry.image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
                live_copy_recorded(entry, depth);
                if (g_trace_active || shadow_log())
                    RBREW_LOGF(stderr, "[tex] slot %u samples depth target slice %u image %08X (tex %u %ux%ux%u fmt %X mip %u)\n",
                                 slot, key[6], img_ptr, entry.tex.id, w, h, layers, fmt, key[5]);
            }
            return;
        }
    }
    // Render-to-texture: a texture over a color buffer's image reads what the
    // GPU drew there, which lives only in the native image.
    for (auto& [key, cbc] : image_views(S.cb_map, img_ptr)) {
        auto& cb = const_cast<ColorTarget&>(cbc);
        ++C.alias_probes;
        const auto compressed = gx2_vk_format(fmt);
        if (target == wire::TEXTURE_2D && compressed.compressed && compressed.block == 16 &&
            (key[1] == 0x122u || key[1] == 0x322u) && key[0] == img_ptr && cb.width == (w + 3) / 4 && cb.height == (h + 3) / 4 &&
            key[4] == descriptor_word(data, GX2SURF_PITCH) && key[5] == descriptor_word(data, GX2TEX_VIEW_MIP) &&
            key[6] == first_slice && key[8] == descriptor_word(data, GX2SURF_AA)) {
            // Native GPU block encoders write compressed blocks through an
            // integer color target: copy its live words into the compressed
            // view (the GPU back-end reads them back and re-uploads them).
            if (ensure_texture_storage(entry, wire::TEXTURE_2D, compressed.format, w, h, 1, 1, false) && live_copy_needed(entry, cb)) {
                end_rendering();
                VkCommandBuffer cbuf = gx2vk::cmd();
                gx2vk::barrier_if_dirty();
                VkImageCopy region{};
                region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.extent = {cb.width, cb.height, 1};
                vkCmdCopyImage(cbuf, cb.image.image, VK_IMAGE_LAYOUT_GENERAL, entry.image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
                live_copy_recorded(entry, cb);
                entry.tex.view_format = compressed.format; entry.tex.levels = 1; entry.tex.layers = 1;
                entry.tex.compressed = true; entry.tex.integer = false; entry.tex.depth = false; entry.tex.complete_storage = true;
                entry.tex.params.min_filter = entry.tex.params.mag_filter = wire::LINEAR;
                entry.tex.params.wrap_s = entry.tex.params.wrap_t = wire::CLAMP_TO_EDGE;
                entry.tex.params.max_level = 0;
            }
            entry.tex.swizzle = word4;
            bind_unit(slot, &entry.tex);
            return;
        }
        if (target == wire::TEXTURE_2D && texture_view_matches(key, data) && cb.image.image) {
            cb.tex.swizzle = word4;
            bind_unit(slot, &cb.tex);
            static unsigned s_logged_aliases;
            if (s_logged_aliases++ < 8 || g_trace_active)
                RBREW_LOGF(stderr, "[tex] slot %u samples render target image %08X (gl tex %u %ux%u fmt %X)\n", slot, img_ptr, cb.id,
                             cb.width, cb.height, key[1]);
            return;
        }
        const bool source32 = key[1] == 0x122u || key[1] == 0x322u || key[1] == 0x823u;
        const bool dest32 = fmt == 0x122u || fmt == 0x322u || fmt == 0x823u;
        if (target == wire::TEXTURE_2D && source32 && dest32 && texture_view_matches(key, data, true)) {
            // glTextureView reinterpretation of the target's 128-bit texels.
            auto& view = S.color_views[{cb.id, fmt, word4}];
            const auto gf = gx2_vk_format(fmt);
            if (!view.tex.id) {
                view.tex.id = S.next_object_id++;
                view.tex.image = &cb.image;
                view.tex.target = wire::TEXTURE_2D;
                view.tex.view_format = gf.format;
                view.tex.levels = 1; view.tex.layers = 1;
                view.tex.integer = gf.integer;
                view.tex.params = render_target_params(gf.integer);
                view.tex.params.max_level = 1000;   // glTextureView with one level
            }
            view.tex.swizzle = word4;
            bind_unit(slot, &view.tex);
            return;
        }
    }
    entry.tex.swizzle = word4;
    bind_unit(slot, &entry.tex);

    bool upload = !entry.hash_valid;
    if (upload || entry.verify) {
        const uint64_t written = texture_write_generation(mem, data);
        if (!upload && written != kGx2WriteUntracked && written <= entry.written) {
            entry.verify = false;
        } else {
            uint64_t hash = 0;
            const bool valid = texture_content_hash(mem, data, hash);
            TXDIAG(if(!entry.hash_valid || !valid || hash!=entry.content_hash)
                txdiag::event("cache_verify","\"cache_id\":%u,\"previous_hash\":%llu,\"current_hash\":%llu,\"valid\":%s,\"written\":%llu",entry.tex.id,entry.content_hash,hash,valid?"true":"false",written));
            upload = upload || !valid || hash != entry.content_hash;
            entry.content_hash = hash; entry.hash_valid = valid; entry.verify = !valid;
            entry.written = written == kGx2WriteUntracked ? 0 : written;
        }
    }
    if (img_ptr != 0 && upload) {
        Gx2PerfScope upload_scope(kPerfTexUpload);
        ++C.texture_uploads;
        entry.image_addr = img_ptr; entry.image_size = img_sz;
        entry.width = w; entry.height = h; entry.layers = layers; entry.format = fmt; entry.mip_levels = mip_levels;
        const auto vf = gx2_vk_format(fmt);
        const uint8_t* pixels = rbrew_memory_accessible(mem, img_ptr, img_sz ? img_sz : 1) ? (mem + img_ptr) : nullptr;
        std::vector<uint8_t> linear;
        bool detiled = false;
        if (pixels && !vf.depth) {
            Gx2DetileDesc d;
            d.width = vf.compressed ? (w + 3) / 4 : w;
            d.height = vf.compressed ? (h + 3) / 4 : h;
            d.pitch = descriptor_word(data, GX2SURF_PITCH);
            d.bits_per_element = vf.compressed ? vf.block * 8 : vf.bpp * 8;
            d.tile_mode = descriptor_word(data, GX2SURF_TILE_MODE);
            d.swizzle = descriptor_word(data, GX2SURF_SWIZZLE);
            RbrewGx2SurfaceDesc sd{dim, w, h, surface_layers, 1, fmt, 0, descriptor_word(data, GX2SURF_USE), d.tile_mode, d.swizzle};
            if (const auto layout = rbrew_gx2_calc_surface_layout(sd)) d.surface_height = layout->padded_height;
            const size_t slice_bytes = size_t(d.width) * d.height * (d.bits_per_element / 8);
            linear.resize(slice_bytes * layers);
            detiled = !linear.empty();
            for (unsigned z = 0; z < layers && detiled; ++z) { d.slice = first_slice + z; detiled = gx2_detile_mip0(d, pixels, img_sz, linear.data() + z * slice_bytes); }
            static unsigned s_logged_uploads;
            if (s_logged_uploads < 40) {
                ++s_logged_uploads;
                size_t nonzero = 0;
                for (size_t i = 0; i < img_sz && i < (1u << 20); ++i) nonzero += mem[img_ptr + i] != 0;
                RBREW_LOGF(stderr, "[tex] upload %08X %ux%u fmt=%X tile=%u pitch=%u detiled=%d nonzero_src_bytes=%zu/%u surf_dim=%u word0=%08X\n",
                             img_ptr, w, h, fmt, d.tile_mode, d.pitch, int(detiled), nonzero, img_sz, descriptor_word(data, GX2SURF_DIM),
                             descriptor_word(data, 0x88));
            }
        }
        // GPU: compressed level 0 without CPU data is never specified; a
        // CUBE_MAP_ARRAY needs a multiple of six layer-faces.
        bool storage_ok = true;
        TXDIAG(txdiag::event("decode","\"cache_id\":%u,\"address\":%u,\"format\":%u,\"width\":%u,\"height\":%u,\"layers\":%u,\"mips\":%u,\"pixels\":%s,\"detiled\":%s",entry.tex.id,img_ptr,fmt,w,h,layers,mip_levels,pixels?"true":"false",detiled?"true":"false"));
        if (vf.compressed && !pixels) storage_ok = false;
        if (target == wire::TEXTURE_CUBE_MAP_ARRAY && (layers % 6) != 0) storage_ok = false;
        if (storage_ok && ensure_texture_storage(entry, target, vf.format, w, h, layers, mip_levels, vf.depth)) {
            entry.tex.view_format = vf.format;
            entry.tex.levels = mip_levels; entry.tex.layers = layers;
            entry.tex.integer = vf.integer; entry.tex.depth = vf.depth; entry.tex.compressed = vf.compressed;
            entry.tex.complete_storage = true;
            const uint8_t* level0 = detiled ? linear.data() : pixels;
            const uint32_t bw = vf.compressed ? (w + 3) / 4 : w, bh = vf.compressed ? (h + 3) / 4 : h;
            const size_t level0_bytes = size_t(bw) * bh * (vf.compressed ? vf.block : vf.bpp) * layers;
            if (vf.depth) {
                clear_image_depth(entry.image, 0.0f, 0);    // GPU allocates depth textures without data
            } else if (level0 && (detiled || !vf.compressed || target != wire::TEXTURE_2D || level0_bytes <= img_sz)) {
                // Undetiled CPU bytes are uploaded as GPU would (they may be short).
                if (!detiled && img_sz > 0 && img_sz < level0_bytes) {
                    std::vector<uint8_t> padded(level0_bytes);
                    std::memcpy(padded.data(), level0, img_sz);
                    upload_image_level(entry.image, 0, 0, layers, w, h, layers, padded.data(), level0_bytes, 0);
                } else {
                    // As in GPU, a zero imageSize does not cap the unpacked
                    // texel upload. Validate the actual range before reading.
                    if (detiled || rbrew_memory_accessible(mem, img_ptr, uint32_t(level0_bytes)))
                        upload_image_level(entry.image, 0, 0, layers, w, h, layers, level0, level0_bytes, 0);
                }
            } else {
                clear_image_color(entry.image, zero_color());
            }
            entry.tex.params.min_filter = entry.tex.params.mag_filter = vf.integer ? wire::NEAREST : wire::LINEAR;
            entry.tex.params.wrap_s = entry.tex.params.wrap_t = wire::CLAMP_TO_EDGE;
            if (target != wire::TEXTURE_2D) entry.tex.params.wrap_r = wire::CLAMP_TO_EDGE;
            // Mip levels use the surface calculator's per-level Latte layout.
            RbrewGx2SurfaceDesc sd{dim, w, h, surface_layers, mip_levels, fmt, 0, descriptor_word(data, GX2SURF_USE),
                                   descriptor_word(data, GX2SURF_TILE_MODE), descriptor_word(data, GX2SURF_SWIZZLE)};
            const auto layout = rbrew_gx2_calc_surface_layout(sd);
            const uint32_t mip_ptr = descriptor_word(data, GX2SURF_MIPMAPS), mip_bytes = descriptor_word(data, GX2SURF_MIPMAP_SIZE);
            for (uint32_t level = 1; level < mip_levels; ++level) {
                const auto mw = std::max(1u, w >> level), mh = std::max(1u, h >> level), ml = dim == 2 ? std::max(1u, layers >> level) : layers;
                const uint32_t offset = level == 1 ? 0 : descriptor_word(data, GX2SURF_MIP_OFFSETS + 4 * (level - 1));
                std::vector<uint8_t> decoded;
                const uint8_t* source = nullptr;
                Gx2DetileDesc d;
                d.width = vf.compressed ? (mw + 3) / 4 : mw; d.height = vf.compressed ? (mh + 3) / 4 : mh;
                d.bits_per_element = (vf.compressed ? vf.block : vf.bpp) * 8;
                const size_t plane = size_t(d.width) * d.height * (d.bits_per_element / 8);
                if (!vf.depth && layout && mip_ptr && offset < mip_bytes && rbrew_memory_accessible(mem, mip_ptr, mip_bytes)) {
                    d.pitch = layout->level_pitch[level]; d.surface_height = layout->level_height[level]; d.tile_mode = layout->level_tile[level];
                    d.swizzle = sd.swizzle;
                    decoded.resize(plane * ml); bool ok = true;
                    for (unsigned z = 0; z < ml && ok; ++z) { d.slice = first_slice + z; ok = gx2_detile_mip0(d, mem + mip_ptr + offset, mip_bytes - offset, decoded.data() + z * plane); }
                    if (ok) source = decoded.data();
                }
                if (vf.depth) continue;
                TXDIAG(if(!source)txdiag::event("mip_decode_failed","\"cache_id\":%u,\"mip\":%u,\"mip_address\":%u,\"mip_bytes\":%u,\"offset\":%u",entry.tex.id,level,mip_ptr,mip_bytes,offset));
                std::vector<uint8_t> empty;
                if (!source) { empty.resize(plane * ml); source = empty.data(); }
                upload_image_level(entry.image, level, 0, ml, mw, mh, ml, source, plane * ml, 0);
            }
            entry.tex.params.max_level = mip_levels - 1;
            if (mip_levels > 1) entry.tex.params.min_filter = vf.integer ? wire::NEAREST_MIPMAP_NEAREST : wire::LINEAR_MIPMAP_LINEAR;
        } else if (!storage_ok) {
            entry.tex.complete_storage = false;
        }
    }
    if (target == wire::TEXTURE_CUBE_MAP_ARRAY && fmt == 0x816u && rbrew_diagnostic_env("RBREW_CUBE_PROBE")) {
        static unsigned probe_reports = 0;
        if (probe_reports < 300) {
            ++probe_reports;
            RBREW_LOGF(stderr, "[cube-probe] tex image=%08X fmt=%X %ux%u dim=%u layers=%u first=%u mips=%u pitch=%u upload=%d storage=%d\n", img_ptr, fmt, w, h, dim, layers, first_slice, mip_levels, descriptor_word(data, GX2SURF_PITCH), int(upload), int(entry.image.image != VK_NULL_HANDLE));
            for (auto& [key, cbc] : image_views(S.cb_map, img_ptr))
                RBREW_LOGF(stderr, "[cube-probe]   cb key=%08X,%X,%u,%u,%u,mip%u,slice%u,n%u,aa%u gen=%llu\n", key[0], key[1], key[2], key[3], key[4], key[5], key[6], key[7], key[8], (unsigned long long)cbc.image.content_generation);
        }
    }
    if (target != wire::TEXTURE_2D && entry.image.image) {
        bool copied = false;
        for (auto& [key, cbc] : image_views(S.cb_map, img_ptr)) {
            ++C.alias_probes;
            if (key[0] != img_ptr || key[1] != fmt || key[2] != w || key[3] != h || key[4] != descriptor_word(data, GX2SURF_PITCH) ||
                key[5] >= mip_levels || key[6] < first_slice || key[6] - first_slice >= (dim == 2 ? std::max(1u, layers >> key[5]) : layers) ||
                std::max(1u, key[7]) != 1)
                continue;
            auto& cb = const_cast<ColorTarget&>(cbc);
            // Preserve map-order overwrite semantics if historical views
            // overlap: once a copy changes the destination, later views must
            // restore their bytes even when their own contents did not change.
            if (!live_copy_needed(entry, cb) && !copied) continue;
            end_rendering();
            VkCommandBuffer cbuf = gx2vk::cmd();
            gx2vk::barrier_if_dirty();
            VkImageCopy region{};
            region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            const bool is3d = entry.image.desc.type == VK_IMAGE_TYPE_3D;
            region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, key[5], is3d ? 0u : key[6] - first_slice, 1};
            region.dstOffset = {0, 0, is3d ? int32_t(key[6] - first_slice) : 0};
            region.extent = {std::min(cb.width, std::max(1u, w >> key[5])), std::min(cb.height, std::max(1u, h >> key[5])), 1};
            vkCmdCopyImage(cbuf, cb.image.image, VK_IMAGE_LAYOUT_GENERAL, entry.image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
            live_copy_recorded(entry, cb);
            copied = true;
            if (g_trace_active)
                RBREW_LOGF(stderr, "[tex] slot %u samples render target slice %u image %08X (gl tex %u %ux%ux%u fmt %X)\n", slot, key[6], img_ptr,
                             entry.tex.id, w, h, layers, fmt);
        }
    }
}

void gx2r::refresh_bound_render_texture(uint32_t unit, uint8_t* mem) {
    if (unit >= S.bound_textures.size()) return;
    const auto& bound = S.bound_textures[unit];
    if (!bound.address) return;
    const auto image = descriptor_word(bound.descriptor.data(), GX2SURF_IMAGE);
    const auto depth = image_views(S.depth_map, image);
    const auto color = image_views(S.cb_map, image);
    if (depth.begin() == depth.end() && color.begin() == color.end()) return;
    // A binding names guest storage, not a frozen copy. Shadow cascades,
    // color-correction volumes and environment faces can be rendered after
    // binding. Refresh copied views at consumption, including targets created
    // after the bind; unchanged source generations still reuse their copies.
    const auto snapshot = bound;
    gx2_render_upload_texture(snapshot.address, unit, mem, snapshot.descriptor.data());
}

void gx2_render_bind_texture(uint32_t tex_addr, uint32_t slot, uint8_t* mem, const uint8_t* descriptor) {
    Gx2PerfScope perf_scope(kPerfTexture);
    gx2_render_upload_texture(tex_addr, slot, mem, descriptor);
}
uint64_t gx2_render_texture_upload_count() { return C.texture_uploads; }
uint64_t gx2_render_texture_alias_probe_count() { return C.alias_probes; }
uint64_t gx2_render_texture_hash_count() { return C.texture_hashes; }

// ---------------------------------------------------------------------------
// Context state snapshots
// ---------------------------------------------------------------------------
void gx2_render_save_context_state(uint32_t context_state) {
    NativeContextSnapshot snapshot;
    snapshot.color_targets = S.color_targets;
    snapshot.depth_target = S.depth_target;
    snapshot.bound_textures = S.bound_textures;
    for (unsigned slot = 0; slot < 32; ++slot) snapshot.attrib_buffers[slot] = {S.attrib[slot].guest_addr, S.attrib[slot].size, S.attrib[slot].stride};
    for (unsigned stage = 0; stage < 2; ++stage) for (unsigned bank = 0; bank < 16; ++bank)
        snapshot.uniform_blocks[stage][bank] = {S.uniform_blocks[stage][bank].address, S.uniform_blocks[stage][bank].size};
    snapshot.vertex_shader = S.cur_vs; snapshot.pixel_shader = S.cur_ps; snapshot.fetch_shader = S.active_fetch;
    snapshot.vertex_shader_from_registers = S.vs_from_regs; snapshot.pixel_shader_from_registers = S.ps_from_regs;
    S.context_snapshots[context_state] = std::move(snapshot);
}

void gx2_render_restore_context_state(uint32_t context_state, uint8_t* mem) {
    TXDIAG(if(txdiag::sample("context_restore",context_state))txdiag::event("context_restore","\"context\":%u",context_state));
    const auto found = S.context_snapshots.find(context_state);
    if (found == S.context_snapshots.end()) return;
    const auto snapshot = found->second;
    if (g_trace_active) RBREW_LOGF(stderr, "[ftrace] restore context %08X depth %p -> %p color0 %p -> %p\n", context_state, (void*)S.depth_target,
                                     (void*)snapshot.depth_target, (void*)S.color_targets[0], (void*)snapshot.color_targets[0]);
    S.color_targets = snapshot.color_targets;
    S.depth_target = snapshot.depth_target;
    S.cur_vs = snapshot.vertex_shader; S.cur_ps = snapshot.pixel_shader;
    S.vs_from_regs = snapshot.vertex_shader_from_registers; S.ps_from_regs = snapshot.pixel_shader_from_registers;
    S.bound_textures = snapshot.bound_textures;
    for (unsigned slot = 0; slot < 32; ++slot) {
        auto& buffer = S.attrib[slot]; const auto& saved = snapshot.attrib_buffers[slot];
        buffer.guest_addr = saved[0]; buffer.size = saved[1]; buffer.stride = saved[2];
    }
    for (unsigned stage = 0; stage < 2; ++stage) for (unsigned bank = 0; bank < 16; ++bank) {
        auto& block = S.uniform_blocks[stage][bank]; const auto& saved = snapshot.uniform_blocks[stage][bank];
        block.address = saved[0]; block.size = saved[1];
    }
    S.active_fetch = snapshot.fetch_shader;
    for (unsigned slot = 0; slot < S.bound_textures.size(); ++slot) {
        const auto& bound = snapshot.bound_textures[slot];
        if (bound.address) gx2_render_upload_texture(bound.address, slot, mem, bound.descriptor.data());
    }
    bind_target_framebuffer();
}

// ---------------------------------------------------------------------------
// Surface copies
// ---------------------------------------------------------------------------
namespace {
struct CpuCopyLevel { Gx2DetileDesc elements; uint32_t address = 0, available = 0; };
std::optional<CpuCopyLevel> cpu_copy_level(const uint8_t* surface, uint32_t mip, uint32_t slice, uint8_t* mem) {
    RbrewGx2SurfaceDesc desc{descriptor_word(surface, 0), descriptor_word(surface, 4), descriptor_word(surface, 8),
                             descriptor_word(surface, 12), descriptor_word(surface, 16), descriptor_word(surface, 20),
                             descriptor_word(surface, 24), descriptor_word(surface, 28)};
    desc.tile_mode = descriptor_word(surface, GX2SURF_TILE_MODE);
    desc.swizzle = descriptor_word(surface, GX2SURF_SWIZZLE);
    const auto layout = rbrew_gx2_calc_surface_layout(desc);
    if (!layout || mip >= layout->levels) return std::nullopt;
    // No unknown-format RGBA8 fallback at a CPU-visible copy boundary.
    switch (desc.format) {
    case 1: case 7: case 8: case 0x19: case 0x1A: case 0x41A: case 0x122: case 0x322:
    case 0x806: case 0x810: case 0x816: case 0x820: case 0x80E: case 0x823:
    case 0x31: case 0x32: case 0x33: case 0x34: case 0x35:
    case 0x431: case 0x432: case 0x433: case 0x234: case 0x235: break;
    default: return std::nullopt;
    }
    const auto format = gx2_vk_format(desc.format);
    const uint32_t slices = desc.dim == 2 ? std::max(1u, desc.depth >> mip) : desc.dim == 3 ? std::max(6u, desc.depth) :
                            desc.dim == 4 || desc.dim == 5 ? desc.depth : 1u;
    if (slice >= slices) return std::nullopt;
    CpuCopyLevel level;
    auto& d = level.elements;
    const uint32_t block = format.compressed ? 4 : 1;
    d.width = (std::max(1u, desc.width >> mip) + block - 1) / block;
    d.height = (std::max(1u, desc.height >> mip) + block - 1) / block;
    d.pitch = mip ? layout->level_pitch[mip] : descriptor_word(surface, GX2SURF_PITCH);
    d.surface_height = layout->level_height[mip]; d.tile_mode = layout->level_tile[mip];
    d.bits_per_element = (format.compressed ? format.block : format.bpp) * 8;
    d.slice = slice; d.swizzle = desc.swizzle; d.is_depth = (desc.use & 4) != 0;
    const uint32_t base = descriptor_word(surface, mip ? GX2SURF_MIPMAPS : GX2SURF_IMAGE);
    const uint32_t size = descriptor_word(surface, mip ? GX2SURF_MIPMAP_SIZE : GX2SURF_IMAGE_SIZE);
    const uint32_t offset = mip > 1 ? descriptor_word(surface, GX2SURF_MIP_OFFSETS + 4 * (mip - 1)) : 0;
    if (!base || offset >= size || uint64_t(base) + offset > UINT32_MAX || !rbrew_memory_accessible(mem, base, size)) return std::nullopt;
    level.address = base + offset; level.available = size - offset;
    return level;
}

bool copy_cpu_surface(const uint8_t* src, uint32_t src_mip, uint32_t src_slice, const uint8_t* dst, uint32_t dst_mip,
                      uint32_t dst_slice, uint8_t* mem) {
    const auto source = cpu_copy_level(src, src_mip, src_slice, mem), destination = cpu_copy_level(dst, dst_mip, dst_slice, mem);
    if (!source || !destination) return false;
    const auto& sd = source->elements; const auto& dd = destination->elements;
    if (sd.width != dd.width || sd.height != dd.height || sd.bits_per_element != dd.bits_per_element) return false;
    const size_t bytes = size_t(sd.width) * sd.height * (sd.bits_per_element / 8);
    std::vector<uint8_t> linear;
    const uint32_t image = descriptor_word(src, GX2SURF_IMAGE), format = descriptor_word(src, GX2SURF_FORMAT);
    ColorTarget* live = nullptr;
    for (auto& [key, cb] : image_views(S.cb_map, image))
        if (key[1] == format && key[2] == descriptor_word(src, GX2SURF_WIDTH) && key[3] == descriptor_word(src, GX2SURF_HEIGHT) &&
            key[4] == descriptor_word(src, GX2SURF_PITCH) && key[5] == src_mip && key[6] == src_slice && std::max(1u, key[7]) == 1 && !key[8]) {
            live = &const_cast<ColorTarget&>(cb); break;
        }
    if (live) {
        const auto vf = gx2_vk_format(format);
        // Native images preserve color element bits; depth and multisample
        // transfers need distinct packing (unsupported, as in the GPU back-end).
        if (sd.is_depth || vf.depth || vf.compressed) return false;
        Gx2PerfScope readback(kPerfTexReadback);
        end_rendering();
        if (!gx2vk::read_image(live->image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, live->width, live->height, linear)) return false;
        if (linear.size() < bytes) return false;
        linear.resize(bytes);
    } else {
        // No GPU image owns this view: source bytes are authoritative CPU data.
        if (sd.is_depth) return false;
        linear.resize(bytes);
        if (!gx2_detile_mip0(sd, mem + source->address, source->available, linear.data())) return false;
    }
    std::vector<uint8_t> tiled(mem + destination->address, mem + destination->address + destination->available);
    if (!gx2_retile_mip0(dd, linear.data(), tiled.data(), tiled.size())) return false;
    if (dd.tile_mode <= 1 || dd.tile_mode == 16) {
        const uint32_t row_bytes = dd.width * (dd.bits_per_element / 8), pitch_bytes = dd.pitch * (dd.bits_per_element / 8);
        const uint64_t first = uint64_t(dd.slice) * dd.surface_height * pitch_bytes;
        for (uint32_t y = 0; y < dd.height; ++y) {
            const auto offset = uint32_t(first + uint64_t(y) * pitch_bytes);
            rbrew_write_memory(mem, destination->address + offset, tiled.data() + offset, row_bytes);
        }
    } else {
        rbrew_write_memory(mem, destination->address, tiled.data(), uint32_t(tiled.size()));
    }
    // Existing destination render views observe this completed CPU copy too.
    const uint32_t dst_image = descriptor_word(dst, GX2SURF_IMAGE), dst_format = descriptor_word(dst, GX2SURF_FORMAT);
    const auto vf = gx2_vk_format(dst_format);
    for (auto& [key, cbc] : image_views(S.cb_map, dst_image)) {
        if (key[1] != dst_format || key[2] != descriptor_word(dst, GX2SURF_WIDTH) || key[3] != descriptor_word(dst, GX2SURF_HEIGHT) ||
            key[4] != descriptor_word(dst, GX2SURF_PITCH) || key[5] != dst_mip || key[6] != dst_slice || std::max(1u, key[7]) != 1) continue;
        if (vf.compressed || dd.is_depth) return false;
        auto& cb = const_cast<ColorTarget&>(cbc);
        const size_t view_bytes = size_t(cb.width) * cb.height * vf.bpp;
        if (view_bytes <= linear.size()) upload_image_level(cb.image, 0, 0, 1, cb.width, cb.height, 1, linear.data(), view_bytes, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    gx2_render_invalidate_textures(destination->address, destination->available);
    return true;
}
}  // namespace

void gx2_render_copy_surface(const uint8_t* src, uint32_t src_mip, uint32_t src_slice, const uint8_t* dst, uint32_t dst_mip,
                             uint32_t dst_slice, uint8_t* mem) {
    if (!src || !dst || !gx2vk::ready() || src_mip >= std::max(1u, descriptor_word(src, 16)) || dst_mip >= std::max(1u, descriptor_word(dst, 16))) return;
    // gx2.rpl 02035628 selects a synchronous CPU transfer whenever either
    // side is LINEAR_SPECIAL: complete readback and guest publication here.
    if (descriptor_word(src, GX2SURF_TILE_MODE) == 16 || descriptor_word(dst, GX2SURF_TILE_MODE) == 16) {
        if (!copy_cpu_surface(src, src_mip, src_slice, dst, dst_mip, dst_slice, mem))
            RBREW_LOGF(stderr, "[gx2] unsupported or invalid CPU surface copy fmt=%X/%X mip=%u/%u slice=%u/%u\n",
                         descriptor_word(src, GX2SURF_FORMAT), descriptor_word(dst, GX2SURF_FORMAT), src_mip, dst_mip, src_slice, dst_slice);
        return;
    }
    const auto color_targets = S.color_targets; auto* depth_target = S.depth_target;
    auto wrapper = [](const uint8_t* surface, uint32_t mip, uint32_t slice) {
        std::array<uint8_t, GX2DB_SIZEOF> d{}; std::copy_n(surface, GX2SURF_SIZEOF, d.data());
        auto w = [&](unsigned a, uint32_t v) { for (unsigned i = 0; i < 4; ++i) d[a + i] = uint8_t(v >> (24 - i * 8)); };
        w(0x74, mip); w(0x78, slice); w(0x7C, 1); return d;
    };
    const auto sw = wrapper(src, src_mip, src_slice), dw = wrapper(dst, dst_mip, dst_slice);
    const auto sk = color_image_key(0, mem, sw.data()), dk = color_image_key(0, mem, dw.data());
    const bool src_depth = (descriptor_word(src, GX2SURF_USE) & 4) != 0, dst_depth = (descriptor_word(dst, GX2SURF_USE) & 4) != 0;
    gx2vk::Image* source_image = nullptr;
    uint32_t source_level = 0;
    DepthTarget* source_depth = nullptr;
    auto find_view = [&](auto& map) {
        for (auto& [key, e] : image_views(map, sk[0]))
            if (std::equal(key.begin(), key.begin() + 7, sk.begin()) && std::max(1u, key[7]) == 1 && key[8] == sk[8])
                return &const_cast<std::remove_const_t<std::remove_reference_t<decltype(e)>>&>(e);
        return static_cast<typename std::remove_reference_t<decltype(map)>::mapped_type*>(nullptr);
    };
    if (src_depth) { if (auto* e = find_view(S.depth_map)) { source_image = &e->image; source_depth = e; } }
    else if (auto* e = find_view(S.cb_map)) source_image = &e->image;
    Texture* temporary = nullptr;
    if (!source_image) {
        // Upload the CPU surface as a texture (unit 31 scratch, as GPU) and copy from its level.
        std::array<uint8_t, GX2TEX_SIZEOF> texture{}; std::copy_n(src, GX2SURF_SIZEOF, texture.data());
        auto w = [&](unsigned a, uint32_t v) { for (unsigned i = 0; i < 4; ++i) texture[a + i] = uint8_t(v >> (24 - i * 8)); };
        w(0x74, src_mip); w(0x78, 1); w(0x7C, src_slice); w(0x80, 1); w(0x90, 0x06880000);
        const auto bound = S.bound_textures[31];
        const auto units = S.units[31];
        gx2_render_upload_texture(1, 31, mem, texture.data());
        S.bound_textures[31] = bound; S.units[31] = units;
        auto it = S.tex_map.find(texture_image_key(mem, texture.data()));
        if (it == S.tex_map.end() || !it->second.image.image) return;
        temporary = &it->second;
        source_image = &temporary->image;
        source_level = src_mip;
    }
    ColorTarget* destination_color = nullptr;
    DepthTarget* destination_depth = nullptr;
    if (dst_depth) { gx2_render_set_depth_buffer(1, mem, dw.data()); destination_depth = S.depth_target; }
    else { gx2_render_set_color_buffer(1, mem, dw.data(), 0); destination_color = S.color_targets[0]; }
    const uint32_t width = std::min(std::max(1u, sk[2] >> src_mip), std::max(1u, dk[2] >> dst_mip));
    const uint32_t height = std::min(std::max(1u, sk[3] >> src_mip), std::max(1u, dk[3] >> dst_mip));
    if (src_depth && !dst_depth && !temporary && source_depth && destination_color) {
        // Depth->R32F is a numeric texture copy, not a CPU-image upload.
        depth_to_color_pass(*source_depth, *destination_color, width, height);
    } else if (src_depth == dst_depth && (destination_color || destination_depth)) {
        gx2vk::Image& target = destination_color ? destination_color->image : destination_depth->image;
        if (source_image->image && target.image) {
            end_rendering();
            VkCommandBuffer cb = gx2vk::cmd();
            gx2vk::barrier_if_dirty();
            const VkImageAspectFlags aspect = src_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
            const uint32_t src_w = std::max(1u, source_image->desc.width >> source_level), src_h = std::max(1u, source_image->desc.height >> source_level);
            const uint32_t cw = std::min(width, std::min(src_w, target.desc.width)), ch = std::min(height, std::min(src_h, target.desc.height));
            if (source_image->desc.format == target.desc.format || src_depth) {
                if (source_image->desc.format == target.desc.format) {
                    VkImageCopy region{};
                    region.srcSubresource = {aspect, source_level, 0, 1};
                    region.dstSubresource = {aspect, 0, 0, 1};
                    region.extent = {cw, ch, 1};
                    vkCmdCopyImage(cb, source_image->image, VK_IMAGE_LAYOUT_GENERAL, target.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
                }
                // A depth blit between different formats fails in GPU too.
            } else if (gx2vk::format_is_integer(source_image->desc.format) == gx2vk::format_is_integer(target.desc.format) &&
                       !gx2vk::format_is_compressed(target.desc.format)) {
                VkImageBlit blit{};
                blit.srcSubresource = {aspect, source_level, 0, 1};
                blit.srcOffsets[1] = {int32_t(cw), int32_t(ch), 1};
                blit.dstSubresource = {aspect, 0, 0, 1};
                blit.dstOffsets[1] = {int32_t(cw), int32_t(ch), 1};
                vkCmdBlitImage(cb, source_image->image, VK_IMAGE_LAYOUT_GENERAL, target.image, VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_NEAREST);
            }
            gx2vk::note_gpu_write(target);
        }
    }
    S.color_targets = color_targets; S.depth_target = depth_target; bind_target_framebuffer();
}

// ---------------------------------------------------------------------------
// Scan buffers and presentation
// ---------------------------------------------------------------------------
namespace {
struct EncodedScan { gx2vk::Image srgb, raw; uint32_t width = 0, height = 0; };
EncodedScan s_encoded[2];
std::string s_pending_present_capture;
gx2vk::Image s_present_raw;   // UNORM copy of an sRGB-format scan source
unsigned s_draws_requested_report = 0;
}
void gx2r_shutdown_scan_images() {
    s_pending_present_capture.clear();
    for (auto& encoded : s_encoded) {
        gx2vk::destroy_image(encoded.srgb); gx2vk::destroy_image(encoded.raw);
        encoded = {};
    }
    gx2vk::destroy_image(s_present_raw);
}
void gx2r::capture_next_present(const std::string& path) { s_pending_present_capture = path; }
void gx2_render_set_scan_format(bool gamepad, uint32_t format) {
    S.scan_formats[gamepad] = format;
    RBREW_LOGF(stderr, "[scan] %s buffer format=%X (sRGB=%u)\n", gamepad ? "DRC" : "TV", format, (format >> 10) & 1);
}

extern void gx2r_capture_frame_copy();      // gx2_render_vk_capture.cpp
extern void gx2r_trace_glitch_frames(unsigned frames);
extern void gx2r_frame_trace_scan(uint32_t cb_addr, uint32_t scan_target);
extern void gx2r_draw_report();             // gx2_render_vk_draw.cpp

void gx2_render_copy_to_scan(uint32_t cb_addr, uint32_t scan_target, uint8_t* mem, const uint8_t* descriptor) {
    if (scan_target == 1 || scan_target == 0) gx2r_capture_frame_copy();
    gx2r_frame_trace_scan(cb_addr, scan_target);
    gx2r_draw_report();
    // Formerly printed twice per frame into the unbuffered log file.
    static unsigned scan_logs = 0;
    if (scan_logs < 8) {
        ++scan_logs;
        RBREW_LOGF(stderr, "[gx2_render] GX2CopyColorBufferToScanBuffer cb=0x%08X target=%u fbo_map_size=%zu\n", cb_addr, scan_target, S.cb_map.size());
    }
    const auto scan_key = color_image_key(cb_addr, mem, descriptor);
    auto it = S.cb_map.find(scan_key);
    if (it == S.cb_map.end()) {
        static unsigned misses = 0;
        if (misses++ < 20)
            RBREW_LOGF(stderr,"[scan] missing source target=%u image=%08X fmt=%X size=%ux%u pitch=%u mip=%u slice=%u count=%u aa=%u\n",
                scan_target,scan_key[0],scan_key[1],scan_key[2],scan_key[3],scan_key[4],scan_key[5],scan_key[6],scan_key[7],scan_key[8]);
        return;
    }
    ColorTarget& scan = it->second;
    const bool gamepad = scan_target != 0 && scan_target != 1;
    if (!gamepad) S.scan_tv_source_id = scan.id;
    gx2vk::Image* image = &scan.image;
    if (S.scan_formats[gamepad] & 0x400u) {
        // GPU blits with FRAMEBUFFER_SRGB into an SRGB8_ALPHA8 texture: the
        // source is decoded if sRGB and the destination encoded. vkCmdBlitImage
        // into an SRGB image does the same; a raw copy keeps the encoded
        // bytes for the unconverted present blit.
        auto& encoded = s_encoded[gamepad];
        if (!encoded.raw.image || encoded.width != scan.width || encoded.height != scan.height) {
            gx2vk::destroy_image(encoded.srgb); gx2vk::destroy_image(encoded.raw);
            gx2vk::ImageDesc desc;
            desc.width = scan.width; desc.height = scan.height;
            desc.format = VK_FORMAT_R8G8B8A8_SRGB;
            desc.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            gx2vk::create_image(encoded.srgb, desc);
            desc.format = VK_FORMAT_R8G8B8A8_UNORM;
            gx2vk::create_image(encoded.raw, desc);
            encoded.width = scan.width; encoded.height = scan.height;
        }
        end_rendering();
        VkCommandBuffer cb = gx2vk::cmd();
        gx2vk::barrier_if_dirty();
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {int32_t(scan.width), int32_t(scan.height), 1};
        blit.dstSubresource = blit.srcSubresource;
        blit.dstOffsets[1] = blit.srcOffsets[1];
        vkCmdBlitImage(cb, scan.image.image, VK_IMAGE_LAYOUT_GENERAL, encoded.srgb.image, VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_NEAREST);
        gx2vk::note_gpu_write(encoded.srgb);
        gx2vk::full_barrier();
        VkImageCopy copy{};
        copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.extent = {scan.width, scan.height, 1};
        vkCmdCopyImage(cb, encoded.srgb.image, VK_IMAGE_LAYOUT_GENERAL, encoded.raw.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        gx2vk::note_gpu_write(encoded.raw);
        image = &encoded.raw;
    }
    const unsigned index = gamepad ? 1 : 0;
    S.scan_source[index] = &scan;
    S.scan_image[index] = image;
    S.scan_w[index] = scan.width; S.scan_h[index] = scan.height;
}

#ifndef RBREW_DISABLE_LOGGING
// Writes one BMP from a raw image readback; shared by interval dumps and glitch captures.
static bool write_frame_bmp(VkFormat format, uint32_t width, uint32_t height, const uint8_t* raw, const char* name) {
    static const char* const directory = rbrew_diagnostic_env("RBREW_FRAME_DUMP_DIR");  // read once
    if (!directory || !*directory) return false;
    // RBREW_FRAME_DUMP_SCALE=<n>: integer downsample factor for the BMP.
    static const unsigned scale = [] { const char* v = rbrew_diagnostic_env("RBREW_FRAME_DUMP_SCALE"); unsigned n = v ? unsigned(std::strtoul(v, nullptr, 10)) : 0u; return n ? n : 1u; }();
    const uint32_t outW = std::max(1u, width / scale), outH = std::max(1u, height / scale);
    const unsigned stride = (outW * 3 + 3) & ~3u;
    std::vector<uint8_t> pixels(size_t(stride) * outH);
    const uint32_t texel = gx2vk::format_block_bytes(format, VK_IMAGE_ASPECT_COLOR_BIT);
    for (uint32_t y = 0; y < outH; ++y) for (uint32_t x = 0; x < outW; ++x) {
        const uint8_t* p = raw + (size_t(y * scale) * width + x * scale) * texel;
        uint8_t r = 0, g = 0, b = 0;
        if (format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB) { r = p[0]; g = p[1]; b = p[2]; }
        else if (format == VK_FORMAT_A2B10G10R10_UNORM_PACK32) { uint32_t v; std::memcpy(&v, p, 4); r = uint8_t((v & 1023) >> 2); g = uint8_t(((v >> 10) & 1023) >> 2); b = uint8_t(((v >> 20) & 1023) >> 2); }
        uint8_t* o = pixels.data() + size_t(y) * stride + x * 3;
        o[0] = b; o[1] = g; o[2] = r;
    }
    std::array<uint8_t, 54> header{}; header[0] = 'B'; header[1] = 'M';
    auto put = [&](unsigned offset, uint32_t value) { for (unsigned i = 0; i < 4; ++i) header[offset + i] = uint8_t(value >> (i * 8)); };
    put(2, 54 + uint32_t(pixels.size())); put(10, 54); put(14, 40); put(18, outW); put(22, uint32_t(-int32_t(outH)));
    header[26] = 1; header[28] = 24; put(34, uint32_t(pixels.size()));
    char path[1024]; std::snprintf(path, sizeof(path), "%s/%s.bmp", directory, name);
    if (FILE* f = std::fopen(path, "wb")) { std::fwrite(header.data(), 1, header.size(), f); std::fwrite(pixels.data(), 1, pixels.size(), f); std::fclose(f); }
    RBREW_LOGF(stderr, "[frame] captured %s size=%ux%u scale=%u\n", name, outW, outH, scale);
    return true;
}

static void dump_frame(gx2vk::Image& image, uint32_t width, uint32_t height) {
    static const char* const directory = rbrew_diagnostic_env("RBREW_FRAME_DUMP_DIR");  // read once
    if (!directory || !*directory) return;
    static unsigned frame = 0; ++frame;
    // RBREW_FRAME_DUMP_EVERY=<n>: dump every n-th present (default 1/60/300).
    static const unsigned every = [] { const char* v = rbrew_diagnostic_env("RBREW_FRAME_DUMP_EVERY"); return v ? unsigned(std::strtoul(v, nullptr, 10)) : 0u; }();
    if (every) { if (frame != 1 && frame % every) return; }
    else if (!(frame == 1 || frame == 60 || frame % 300 == 0)) return;
    std::vector<uint8_t> raw;
    if (!gx2vk::read_image(image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, width, height, raw)) return;
    char name[32]; std::snprintf(name, sizeof(name), "frame_%05u", frame);
    write_frame_bmp(image.desc.format, width, height, raw.data(), name);
}

// RBREW_FRAME_SIG=1: one brightness line per present for flicker timelines.
// Note: read_image(level, layer) copies the whole image from the origin, so sample probes on CPU.
static void frame_signature(gx2vk::Image& image, uint32_t width, uint32_t height, bool gamepad) {
    static const bool enabled = rbrew_diagnostic_env("RBREW_FRAME_SIG") != nullptr;
    if (!enabled) return;
    static unsigned seq = 0; ++seq;
    std::vector<uint8_t> raw;
    if (!gx2vk::read_image(image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, width, height, raw)) {
        RBREW_LOGF(stderr, "[sig] %u g=%u readback failed\n", seq, gamepad ? 1u : 0u);
        return;
    }
    const VkFormat format = image.desc.format;
    const uint32_t texel = gx2vk::format_block_bytes(format, VK_IMAGE_ASPECT_COLOR_BIT);
    auto probe = [&](uint32_t x0, uint32_t y0) -> unsigned {
        if (uint64_t(x0) + 16 > width || uint64_t(y0) + 8 > height) return 0;
        unsigned sum = 0;
        for (unsigned y = 0; y < 8; ++y) for (unsigned x = 0; x < 16; ++x) {
            const uint8_t* p = raw.data() + (size_t(y0 + y) * width + (x0 + x)) * texel;
            unsigned r = 0, g = 0, b = 0;
            if (format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB) { r = p[0]; g = p[1]; b = p[2]; }
            else if (format == VK_FORMAT_A2B10G10R10_UNORM_PACK32) { uint32_t v; std::memcpy(&v, p, 4); r = uint8_t((v & 1023) >> 2); g = uint8_t(((v >> 10) & 1023) >> 2); b = uint8_t(((v >> 20) & 1023) >> 2); }
            sum += (r + g + b) / 3;
        }
        return sum / 128;
    };
    const unsigned a = probe(std::max(0u, width / 8), std::max(0u, height / 8));
    const unsigned b = probe(std::max(0u, width / 2), std::max(0u, height / 2));
    // Rolling median of recent presents; BMP-capture deviating presents. The
    // one-frame flicker returns to baseline immediately, so a strong deviation
    // followed by a normal frame is captured retrospectively.
    static unsigned hist_a[12]{}, hist_b[12]{}, hist_n = 0, hist_pos = 0, captures = 0;
    static std::vector<uint8_t> prev_raw;
    static unsigned prev_seq = 0, prev_dev = 0;
    unsigned dev = 0;
    if (hist_n >= 4) {
        unsigned sa[12], sb[12];
        for (unsigned i = 0; i < hist_n; ++i) { sa[i] = hist_a[i]; sb[i] = hist_b[i]; }
        std::nth_element(sa, sa + hist_n / 2, sa + hist_n);
        std::nth_element(sb, sb + hist_n / 2, sb + hist_n);
        const unsigned ma = sa[hist_n / 2], mb = sb[hist_n / 2];
        dev = std::max(a > ma ? a - ma : ma - a, b > mb ? b - mb : mb - b);
    }
    if (captures < 256) {
        if (prev_dev > 25 && dev <= 10 && prev_raw.size() == raw.size()) {
            ++captures;
            char name[48]; std::snprintf(name, sizeof(name), "glitch_%05u_retro", prev_seq);
            write_frame_bmp(format, width, height, prev_raw.data(), name);
        } else if (dev > 25) {
            ++captures;
            char name[48]; std::snprintf(name, sizeof(name), "glitch_%05u_direct", seq);
            write_frame_bmp(format, width, height, raw.data(), name);
        }
    }
    if (dev > 25) gx2r_trace_glitch_frames(40);
    prev_raw = raw; prev_seq = seq; prev_dev = dev;
    hist_a[hist_pos] = a; hist_b[hist_pos] = b; hist_pos = (hist_pos + 1) % 12;
    if (hist_n < 12) ++hist_n;
    RBREW_LOGF(stderr, "[sig] %u g=%u a=%u b=%u%s\n", seq, gamepad ? 1u : 0u, a, b, dev > 25 ? " GLITCH" : "");
}
#else
static void dump_frame(gx2vk::Image&, uint32_t, uint32_t) {}
static void frame_signature(gx2vk::Image&, uint32_t, uint32_t, bool) {}
#endif

size_t gx2r_vertex_cache_entries();   // gx2_render_vk_draw.cpp
uint64_t gx2r_vertex_cache_bytes();
std::pair<uint64_t, uint64_t> gx2_dl_recording_counts();   // gx2_display_list.cpp
void gx2_render_present(void* sdl_window, bool gamepad_mode) {
    TXDIAG(txdiag::present(gx2vk::recording_serial(),gx2vk::known_completed_serial());txdiag::snapshot());
    Gx2PerfScope perf_scope(kPerfPresent);
    (void)sdl_window;
    if (!gx2vk::ready()) return;
    end_rendering();
    const unsigned index = gamepad_mode ? 1 : 0;
    gx2vk::Image* image = S.scan_image[index];
    const uint32_t w = S.scan_w[index], h = S.scan_h[index];
    // RBREW_PRESENT_LOG=1: present sequence for frame/flicker correlation.
    static const bool present_log = rbrew_diagnostic_env("RBREW_PRESENT_LOG") != nullptr;
    static unsigned present_seq = 0; ++present_seq;
    if (present_seq % 600 == 0) {   // ~10 s: memory in use (log only)
        const Gx2HostMemory m = gx2_host_memory();
        constexpr uint64_t guest_committed = uint64_t(RBREW_APPLICATION_ARENA_END) + RBREW_FG_FREE_SIZE + RBREW_MEM1_SIZE + RBREW_SHARED_DATA_SIZE;
        const auto [lists_recorded, lists_resident] = gx2_dl_recording_counts();
        RBREW_LOGF(stderr, "[mem] process in RAM %llu MB (private %llu MB) | system free %llu of %llu MB | guest reserved %llu MB committed %llu MB"
                     " | GPU allocations %llu MB | vertex cache %zu meshes %llu MB | display lists %llu resident of %llu recorded\n",
                     (unsigned long long)(m.working_set >> 20), (unsigned long long)(m.private_bytes >> 20),
                     (unsigned long long)(m.available >> 20), (unsigned long long)(m.total >> 20),
                     (unsigned long long)(uint64_t(WIIU_MEM_SIZE) >> 20), (unsigned long long)(guest_committed >> 20),
                     (unsigned long long)(gx2vk::allocated_bytes() >> 20), gx2r_vertex_cache_entries(),
                     (unsigned long long)(gx2r_vertex_cache_bytes() >> 20), (unsigned long long)lists_resident,
                     (unsigned long long)lists_recorded);
    }
    if (present_log)
        RBREW_LOGF(stderr, "[present] %u gamepad=%u image=%p size=%ux%u\n", present_seq, gamepad_mode ? 1u : 0u, (void*)image, w, h);
    gx2vk::Image* source = image;
    if (image && image->image && (image->desc.format == VK_FORMAT_R8G8B8A8_SRGB)) {
        // GPU blits sRGB scan textures to the window unconverted.
        if (!s_present_raw.image || s_present_raw.desc.width != image->desc.width || s_present_raw.desc.height != image->desc.height) {
            gx2vk::destroy_image(s_present_raw);
            gx2vk::ImageDesc desc;
            desc.width = image->desc.width; desc.height = image->desc.height;
            desc.format = VK_FORMAT_R8G8B8A8_UNORM;
            desc.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            gx2vk::create_image(s_present_raw, desc);
        }
        VkCommandBuffer cb = gx2vk::cmd();
        gx2vk::barrier_if_dirty();
        VkImageCopy copy{};
        copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.extent = {image->desc.width, image->desc.height, 1};
        vkCmdCopyImage(cb, image->image, VK_IMAGE_LAYOUT_GENERAL, s_present_raw.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        gx2vk::note_gpu_write(s_present_raw);
        source = &s_present_raw;
    }
    if (source && source->image && w && h) { frame_signature(*source, w, h, gamepad_mode); dump_frame(*source, w, h); }
    if (gx2vk::has_surface()) {
        static gx2vk::Image none;
#ifndef RBREW_DISABLE_LOGGING
        const bool capture = !gamepad_mode && !s_pending_present_capture.empty();
#else
        constexpr bool capture = false;   // screen captures are a diagnostic-build feature
#endif
        const bool previous = capture ? gx2vk::capture_presents(true) : false;
        const bool presented = gx2vk::present(source && source->image ? *source : none, w, h);
        if (capture) {
            gx2vk::capture_presents(previous);
            std::vector<uint8_t> rgba; uint32_t width=0,height=0;
            if (presented && gx2vk::last_present(rgba,width,height)) {
                // Readback is from the acquired swapchain image, not a replay
                // or an earlier render target. No per-draw waits are enabled.
                const auto path = std::exchange(s_pending_present_capture, {});
                std::vector<uint8_t> rgb(size_t(width)*height*3);
                for(size_t i=0,j=0;i<rgba.size();i+=4,j+=3)std::copy_n(rgba.data()+i,3,rgb.data()+j);
                bool saved=false;
                if(FILE* file=std::fopen(path.c_str(),"wb")) {
                    const bool wrote=std::fprintf(file,"P6\n%u %u\n255\n",width,height)>0 &&
                        std::fwrite(rgb.data(),1,rgb.size(),file)==rgb.size();
                    saved=std::fclose(file)==0 && wrote;
                }
                if(saved)RBREW_LOGF(stderr,"[capture] presented -> %s (%ux%u scan=%u)\n",path.c_str(),width,height,S.scan_tv_source_id);
                else RBREW_LOGF(stderr,"[capture] cannot write presented image %s\n",path.c_str());
            }
        }
    } else {
        gx2vk::submit(false);
    }
    gx2_render_query_poll();
}

// ---------------------------------------------------------------------------
// Shader / fetch / uniform state (no GPU work)
// ---------------------------------------------------------------------------
void gx2_render_save_fetch_shader(uint32_t fs_addr, uint32_t attrib_count, uint32_t streams_guest_addr, uint8_t* mem) {
    if (!fs_addr || attrib_count > 32 || !rbrew_memory_accessible(mem, streams_guest_addr, uint64_t(attrib_count) * 32)) return;
    FetchShaderInfo fetch;
    for (unsigned i = 0; i < attrib_count; ++i) {
        uint32_t a = streams_guest_addr + i * 32;
        fetch.attributes.push_back({rbrew_read32(mem, a), rbrew_read32(mem, a + 4), rbrew_read32(mem, a + 8),
                                    rbrew_read32(mem, a + 12), rbrew_read32(mem, a + 20), rbrew_read32(mem, a + 28),
                                    rbrew_read32(mem, a + 24), rbrew_read32(mem, a + 16) == 1});
    }
    // The program buffer as GX2SetFetchShader will see it (size [2] = 16 + 16
    // per attribute); a later change means another program was loaded there.
    const uint32_t size = 16 + attrib_count * 16;
    if (rbrew_memory_accessible(mem, fs_addr, size)) fetch.program.assign(mem + fs_addr, mem + fs_addr + size);
    S.fetch_shaders[fs_addr] = std::move(fetch);
}
void gx2_render_set_active_fetch_shader(uint32_t fs_addr) {
    S.active_fetch = fs_addr;
    static std::set<uint32_t> s_unknown;
    if (fs_addr && !S.fetch_shaders.count(fs_addr) && s_unknown.insert(fs_addr).second && s_unknown.size() <= 20)
        RBREW_LOGF(stderr, "[gx2_render] fetch shader program %08X has no GX2InitFetchShaderEx layout\n", fs_addr);
}
void gx2_render_store_fetch_attributes(uint32_t program, const std::vector<R700FetchAttribute>& attributes) {
    if (!program || attributes.empty()) return;
    auto& fetch = S.fetch_shaders[program];
    fetch.attributes = attributes;
    fetch.program.clear();
}
void gx2_render_store_fetch_attributes(uint32_t program, const std::vector<R700FetchAttribute>& attributes,
                                       const uint8_t* bytes, uint32_t size) {
    if (!program) return;
    auto it = S.fetch_shaders.find(program);
    if (attributes.empty()) {
        // Undecodable bytes: keep a known layout, but stop re-decoding them.
        if (it != S.fetch_shaders.end()) it->second.program.assign(bytes, bytes + size);
        return;
    }
    auto& fetch = it != S.fetch_shaders.end() ? it->second : S.fetch_shaders[program];
    fetch.attributes = attributes;
    fetch.program.assign(bytes, bytes + size);
}
bool gx2_render_fetch_program_current(uint32_t program, const uint8_t* bytes, uint32_t size) {
    auto it = S.fetch_shaders.find(program);
    if (it == S.fetch_shaders.end() || it->second.attributes.empty()) return false;
    const auto& known = it->second.program;
    return known.empty() || (known.size() == size && std::memcmp(known.data(), bytes, size) == 0);
}
uint32_t gx2_render_active_fetch_attribute_count() {
    auto it = S.fetch_shaders.find(S.active_fetch);
    return it == S.fetch_shaders.end() ? 0u : uint32_t(it->second.attributes.size());
}
void gx2_render_set_attrib_buffer(uint32_t slot, uint32_t guest_addr, uint32_t size, uint32_t stride) {
    if (slot >= 32) return;
    auto& ab = S.attrib[slot];
    ab.guest_addr = guest_addr; ab.size = size; ab.stride = stride;
}
void gx2_render_set_vertex_shader(uint32_t vs_addr, uint8_t* mem) {
    if (!vs_addr) return;
    S.cur_vs = vs_addr; S.vs_from_regs = false;
    auto p = gx2_shader_program({mem, WIIU_MEM_SIZE}, vs_addr, false);
    S.cur_vs_attribs = p.attribute_count;
    r700_shader_registers(S.regs.data(), mem + vs_addr, false);
}
void gx2_render_set_pixel_shader(uint32_t ps_addr, uint8_t* mem) {
    if (!ps_addr) return;
    S.cur_ps = ps_addr; S.ps_from_regs = false;
    r700_shader_registers(S.regs.data(), mem + ps_addr, true);
}
void gx2_render_set_uniform_reg(bool is_pixel, uint32_t index, uint32_t src_addr, uint32_t count, uint8_t* mem) {
    // gx2.rpl 0202f608/0202f754: offset and count are DWORD units.
    if (!src_addr || !count || count > 1024 || !rbrew_memory_accessible(mem, src_addr, uint64_t(count) * 4)) return;
    std::vector<uint32_t> words(count);
    for (unsigned i = 0; i < count; ++i) words[i] = rbrew_read32(mem, src_addr + i * 4);
    gx2_render_set_uniform_words(is_pixel, index, words.data(), count);
}
void gx2_render_set_uniform_words(bool is_pixel, uint32_t index, const uint32_t* words, uint32_t count) {
    index &= 0x7FFF;
    if (!words || !count || index >= 1024 || count > 1024 - index) return;
    for (unsigned i = 0; i < count; ++i) S.uniform_words[is_pixel][index + i] = int32_t(words[i]);
}
uint32_t gx2_render_uniform_word(bool is_pixel, uint32_t index) { return index < 1024 ? uint32_t(S.uniform_words[is_pixel][index]) : 0; }
void gx2_render_set_uniform_block(bool is_pixel, uint32_t block_index, uint32_t src_addr, uint32_t size, uint8_t* mem) {
    if (block_index >= 16 || !src_addr || !size || (size & 3) || !rbrew_memory_accessible(mem, src_addr, size)) return;
    auto& block = S.uniform_blocks[is_pixel][block_index];
    block.address = src_addr; block.size = size;
}
uint64_t gx2_render_uniform_lookup_count() { return C.uniform_lookups; }
uint64_t gx2_render_uniform_upload_count() { return C.uniform_uploads; }
uint64_t gx2_render_uniform_buffer_upload_count() { return C.uniform_buffer_uploads; }
uint64_t gx2_render_state_query_count() { return C.state_queries; }   // the Vulkan back-end never reads driver state back
uint64_t gx2_render_vertex_upload_count() { return C.vertex_uploads; }
uint64_t gx2_render_program_link_count() { return C.programs_linked; }

// ---------------------------------------------------------------------------
// Test inspection (gx2_render_vk_debug.h)
// ---------------------------------------------------------------------------
#include "gx2_render_vk_debug.h"
bool gx2_render_vk_read_color(const uint8_t* descriptor, uint8_t* mem, std::vector<uint8_t>& texels, uint32_t& width, uint32_t& height,
                              uint32_t& bytes_per_texel) {
    auto it = S.cb_map.find(color_image_key(0, mem, descriptor));
    if (it == S.cb_map.end()) return false;
    auto& t = it->second;
    end_rendering();
    width = t.width; height = t.height;
    bytes_per_texel = gx2vk::format_block_bytes(t.image.desc.format, VK_IMAGE_ASPECT_COLOR_BIT);
    return gx2vk::read_image(t.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, t.width, t.height, texels);
}
bool gx2_render_vk_read_depth(const uint8_t* descriptor, uint8_t* mem, std::vector<float>& depth, uint32_t& width, uint32_t& height) {
    auto it = S.depth_map.find(color_image_key(0, mem, descriptor));
    if (it == S.depth_map.end()) return false;
    auto& d = it->second;
    end_rendering();
    std::vector<uint8_t> raw;
    if (!gx2vk::read_image(d.image, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, d.width, d.height, raw)) return false;
    width = d.width; height = d.height;
    depth.resize(size_t(width) * height);
    for (size_t i = 0; i < depth.size(); ++i) {
        if (d.format == VK_FORMAT_D16_UNORM) { uint16_t v; std::memcpy(&v, raw.data() + i * 2, 2); depth[i] = v / 65535.0f; }
        else if (d.format == VK_FORMAT_D24_UNORM_S8_UINT) { uint32_t v; std::memcpy(&v, raw.data() + i * 4, 4); depth[i] = float(v & 0xFFFFFF) / 16777215.0f; }
        else std::memcpy(&depth[i], raw.data() + i * 4, 4);
    }
    return true;
}
bool gx2_render_vk_read_stencil(const uint8_t* descriptor, uint8_t* mem, std::vector<uint8_t>& stencil) {
    auto it = S.depth_map.find(color_image_key(0, mem, descriptor));
    if (it == S.depth_map.end() || !it->second.stencil) return false;
    end_rendering();
    return gx2vk::read_image(it->second.image, VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, it->second.width, it->second.height, stencil);
}
uint64_t gx2_render_vk_submissions() { return gx2vk::submission_count(); }
uint64_t gx2_render_vk_validation_errors() { return gx2vk::validation_error_count(); }
const char* gx2_render_vk_last_validation_error() { static std::string s; s = gx2vk::last_validation_error(); return s.c_str(); }
uint64_t gx2_render_vk_pipelines() { return C.pipelines; }
uint64_t gx2_render_vk_rendering_instances() { return C.rendering_instances; }
static float half_to_float_local(uint16_t h) {
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? (m ? NAN : INFINITY) : std::ldexp(float(m | 1024), int(e) - 25);
    return s ? -v : v;
}
static float ufloat_local(uint32_t bits, unsigned mbits) {
    const uint32_t e = bits >> mbits, m = bits & ((1u << mbits) - 1);
    if (e == 0) return std::ldexp(float(m), -14 - int(mbits));
    if (e == 31) return m ? NAN : INFINITY;
    return std::ldexp(float(m | (1u << mbits)), int(e) - 15 - int(mbits));
}
bool gx2_render_vk_convert_rgba8(int vk_format, const std::vector<uint8_t>& raw, size_t n, std::vector<uint8_t>& out) {
    const VkFormat f = VkFormat(vk_format);
    const uint32_t texel = gx2vk::format_block_bytes(f, VK_IMAGE_ASPECT_COLOR_BIT);
    if (raw.size() < n * texel) return false;
    out.assign(n * 4, 0);
    auto unorm = [](float v) { return uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    for (size_t i = 0; i < n; ++i) {
        uint8_t* o = out.data() + i * 4;
        const uint8_t* p = raw.data() + i * texel;
        float c[4] = {0, 0, 0, 1};
        switch (f) {
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: std::memcpy(o, p, 4); continue;
        case VK_FORMAT_R8_UNORM: o[0] = p[0]; o[3] = 255; continue;
        case VK_FORMAT_R8G8_UNORM: o[0] = p[0]; o[1] = p[1]; o[3] = 255; continue;
        case VK_FORMAT_R16_UNORM: {uint16_t v;std::memcpy(&v,p,2);c[0]=v/65535.0f;} break;
        case VK_FORMAT_R16G16B16A16_SFLOAT: for (unsigned k = 0; k < 4; ++k) { uint16_t h; std::memcpy(&h, p + k * 2, 2); c[k] = half_to_float_local(h); } break;
        case VK_FORMAT_R16_SFLOAT: { uint16_t h; std::memcpy(&h, p, 2); c[0] = half_to_float_local(h); } break;
        case VK_FORMAT_R16G16_SFLOAT: for (unsigned k = 0; k < 2; ++k) { uint16_t h; std::memcpy(&h, p + k * 2, 2); c[k] = half_to_float_local(h); } break;
        case VK_FORMAT_R32G32B32A32_SFLOAT: std::memcpy(c, p, 16); break;
        case VK_FORMAT_R32_SFLOAT: std::memcpy(c, p, 4); break;
        case VK_FORMAT_R32G32_SFLOAT: std::memcpy(c, p, 8); break;
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32: { uint32_t v; std::memcpy(&v, p, 4); c[0] = ufloat_local(v & 0x7FF, 6); c[1] = ufloat_local((v >> 11) & 0x7FF, 6); c[2] = ufloat_local(v >> 22, 5); } break;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: { uint32_t v; std::memcpy(&v, p, 4); c[0] = (v & 1023) / 1023.0f; c[1] = ((v >> 10) & 1023) / 1023.0f; c[2] = ((v >> 20) & 1023) / 1023.0f; c[3] = (v >> 30) / 3.0f; } break;
        case VK_FORMAT_R5G6B5_UNORM_PACK16: { uint16_t v; std::memcpy(&v, p, 2); c[0] = (v >> 11) / 31.0f; c[1] = ((v >> 5) & 63) / 63.0f; c[2] = (v & 31) / 31.0f; } break;
        default: continue;   // integer formats: GL_INVALID_OPERATION, nothing read
        }
        for (unsigned k = 0; k < 4; ++k) o[k] = unorm(c[k]);
    }
    return true;
}
static bool read_attachment(unsigned attachment, std::vector<uint8_t>& raw, ColorTarget*& target) {
    target = attachment < 8 ? S.framebuffer.colors[attachment] : nullptr;
    if (!target) return false;
    end_rendering();
    return gx2vk::read_image(target->image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, target->width, target->height, raw);
}
bool gx2_render_vk_read_pixels(unsigned attachment, int x, int y, int width, int height, std::vector<uint8_t>& rgba8) {
    std::vector<uint8_t> raw; ColorTarget* t = nullptr;
    if (!read_attachment(attachment, raw, t)) return false;
    std::vector<uint8_t> all;
    if (!gx2_render_vk_convert_rgba8(int(t->image.desc.format), raw, size_t(t->width) * t->height, all)) return false;
    rgba8.assign(size_t(std::max(width, 0)) * std::max(height, 0) * 4, 0);
    for (int row = 0; row < height; ++row) for (int col = 0; col < width; ++col) {
        const int sx = x + col, sy = y + row;
        if (sx < 0 || sy < 0 || sx >= int(t->width) || sy >= int(t->height)) continue;
        std::memcpy(&rgba8[(size_t(row) * width + col) * 4], &all[(size_t(sy) * t->width + sx) * 4], 4);
    }
    return true;
}
bool gx2_render_vk_read_pixels_integer(unsigned attachment, int x, int y, int width, int height, std::vector<uint32_t>& rgba32) {
    std::vector<uint8_t> raw; ColorTarget* t = nullptr;
    if (!read_attachment(attachment, raw, t) || !t->fmt.integer) return false;
    const uint32_t texel=gx2vk::format_block_bytes(t->image.desc.format,VK_IMAGE_ASPECT_COLOR_BIT);
    if(texel!=16 && t->image.desc.format!=VK_FORMAT_R16G16B16A16_UINT)return false;
    rgba32.assign(size_t(std::max(width, 0)) * std::max(height, 0) * 4, 0);
    for (int row = 0; row < height; ++row) for (int col = 0; col < width; ++col) {
        const int sx = x + col, sy = y + row;
        if (sx < 0 || sy < 0 || sx >= int(t->width) || sy >= int(t->height)) continue;
        auto* out=&rgba32[(size_t(row) * width + col) * 4];
        const auto* pixel=&raw[(size_t(sy) * t->width + sx) * texel];
        if(texel==16)std::memcpy(out,pixel,16);
        else for(unsigned c=0;c<4;++c){uint16_t value;std::memcpy(&value,pixel+c*2,2);out[c]=value;}
    }
    return true;
}
void gx2_render_vk_state(Gx2VkStateView& o) {
    const auto& g = S.raster;
    o.depth_test = g.depth_test; o.depth_write = g.depth_write; o.stencil_test = g.stencil_test; o.cull = g.cull; o.blend = g.blend;
    o.scissor_test = g.scissor_test; o.alpha_to_coverage = g.alpha_to_coverage; o.primitive_restart = g.primitive_restart;
    o.polygon_offset_fill = g.polygon_offset_fill;
    o.depth_func = g.depth_func; o.cull_face = g.cull_face; o.front_face = g.front_face; o.clip_origin = g.clip_origin;
    o.blend_src_rgb = g.blend_src_rgb; o.blend_dst_rgb = g.blend_dst_rgb; o.blend_src_alpha = g.blend_src_alpha; o.blend_dst_alpha = g.blend_dst_alpha;
    o.blend_eq_rgb = g.blend_eq_rgb; o.blend_eq_alpha = g.blend_eq_alpha;
    std::memcpy(o.blend_color, g.blend_color.data(), 16);
    std::memcpy(o.color_mask, g.color_mask.data(), 8);
    o.point_size = g.point_size; o.restart_index = g.restart_index;
    std::memcpy(o.viewport, g.viewport, 16); std::memcpy(o.scissor, g.scissor, 16);
    o.depth_range[0] = g.depth_range[0]; o.depth_range[1] = g.depth_range[1];
    o.offset_factor = g.offset_factor; o.offset_units = g.offset_units; o.offset_clamp = g.offset_clamp;
    auto face = [](const StencilFace& s) { return Gx2VkStateView::Face{s.func, s.ref, s.mask, s.fail, s.zfail, s.zpass, s.write_mask}; };
    o.front = face(g.front); o.back = face(g.back);
    const bool upper = g.clip_origin == wire::UPPER_LEFT;
    o.vk_viewport[0] = float(g.viewport[0]);
    o.vk_viewport[1] = float(upper ? g.viewport[1] + g.viewport[3] : g.viewport[1]);
    o.vk_viewport[2] = float(g.viewport[2]);
    o.vk_viewport[3] = upper ? -float(g.viewport[3]) : float(g.viewport[3]);
    o.vk_viewport[4] = float(g.depth_range[0]); o.vk_viewport[5] = float(g.depth_range[1]);
    bool ccw = g.front_face == wire::CCW;
    if (!upper) ccw = !ccw;
    o.vk_front_face = ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
}
static uint32_t gl_swizzle_enum(uint32_t sel) {
    static const uint32_t map[6] = {wire::RED, wire::GREEN, wire::BLUE, wire::ALPHA, wire::ZERO, wire::ONE};
    return sel < 6 ? map[sel] : wire::ZERO;
}
bool gx2_render_vk_unit_texture(uint32_t unit, uint32_t target, Gx2VkTextureView& o) {
    if (unit >= S.units.size() || target > 3) return false;
    TexObject* t = S.units[unit][target];
    if (!t || !t->image || !t->image->image) return false;
    o.id = t->id; o.width = t->image->desc.width; o.height = t->image->desc.height; o.depth = t->image->desc.depth;
    o.levels = t->levels; o.layers = t->image->desc.layers; o.vk_format = int(t->view_format);
    for (unsigned c = 0; c < 4; ++c) o.swizzle[c] = gl_swizzle_enum((t->swizzle >> (16 + 3 * c)) & 7);
    o.min_filter = t->params.min_filter; o.mag_filter = t->params.mag_filter; o.max_level = t->params.max_level;
    o.integer = t->integer; o.depth_format = t->depth;
    return true;
}
void gx2_render_vk_framebuffer(Gx2VkFramebufferView& o) {
    o = {};
    o.valid = S.framebuffer.valid; o.depth_only = S.framebuffer.depth_only;
    for (unsigned i = 0; i < 8; ++i) if (auto* c = S.framebuffer.colors[i]) { o.color[i] = c->id; o.color_width[i] = c->width; }
    if (auto* d = S.framebuffer.depth) { o.depth = d->id; o.depth_width = d->width; o.depth_height = d->height; }
}
void gx2_render_vk_clear_uint(const uint32_t words[4]) {
    if (!S.framebuffer.valid || !ensure_rendering()) return;
    std::vector<VkClearAttachment> clears;
    for (unsigned i = 0; i < 8; ++i) if (S.framebuffer.colors[i]) {
        VkClearAttachment a{VK_IMAGE_ASPECT_COLOR_BIT, i, {}};
        std::memcpy(a.clearValue.color.uint32, words, 16);
        clears.push_back(a);
        break;   // glClearBufferuiv(GL_COLOR, 0, ...): draw buffer 0 only
    }
    if (clears.empty()) return;
    VkClearRect rect{render_area(), 0, 1};
    vkCmdClearAttachments(gx2vk::cmd(), uint32_t(clears.size()), clears.data(), 1, &rect);
    gx2vk::note_gpu_write(S.framebuffer.colors[clears[0].colorAttachment]->image);
}
bool gx2_render_vk_sampler(uint32_t unit, Gx2VkSamplerView& o) {
    o = {};
    if (unit / 32 >= 3 || unit % 32 >= 18) return false;
    const auto& s = S.samplers[(unit / 32) * 18 + unit % 32];
    o.valid = s.valid;
    if (!s.valid) return true;
    const auto& p = s.params;
    o.wrap[0] = p.wrap_s; o.wrap[1] = p.wrap_t; o.wrap[2] = p.wrap_r;
    o.min_filter = p.min_filter; o.mag_filter = p.mag_filter; o.compare_func = p.compare_func; o.compare_mode = p.compare_mode;
    o.min_lod = p.min_lod; o.max_lod = p.max_lod; o.lod_bias = p.lod_bias; o.anisotropy = p.anisotropy;
    std::memcpy(o.border, p.border.data(), 16);
    extern void gx2r_sampler_info(const TexParams&, bool, bool, VkSamplerCreateInfo&);
    VkSamplerCreateInfo info{};
    gx2r_sampler_info(p, false, false, info);
    o.vk_address[0] = info.addressModeU; o.vk_address[1] = info.addressModeV; o.vk_address[2] = info.addressModeW;
    o.vk_min = info.minFilter; o.vk_mag = info.magFilter; o.vk_mipmap = info.mipmapMode; o.vk_border_color = info.borderColor;
    o.vk_min_lod = info.minLod; o.vk_max_lod = info.maxLod;
    return true;
}
void gx2_render_vk_capture_presents(bool enable) { gx2vk::capture_presents(enable); }
bool gx2_render_vk_last_present(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) { return gx2vk::last_present(rgba, width, height); }
bool gx2_render_vk_validation_selftest() {
    if (!gx2vk::caps().validation) return false;
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.anisotropyEnable = VK_TRUE;
    info.maxAnisotropy = gx2vk::caps().limits.maxSamplerAnisotropy * 1000.0f;   // VUID-VkSamplerCreateInfo-anisotropyEnable-01071
    info.maxLod = 1.0f;
    VkSampler sampler = VK_NULL_HANDLE;
    if (vkCreateSampler(gx2vk::device(), &info, nullptr, &sampler) == VK_SUCCESS && sampler) vkDestroySampler(gx2vk::device(), sampler, nullptr);
    return true;
}
bool gx2_render_vk_gpu_idle() { return gx2vk::completed_serial() + 1 >= gx2vk::recording_serial() && !gx2vk::has_recorded_commands(); }
bool gx2_render_vk_clear_unit(uint32_t unit, uint32_t target) {
    if (unit >= S.units.size() || target > 3) return false;
    TexObject* t = S.units[unit][target];
    if (!t || !t->image || !t->image->image) return false;
    if (t->depth) clear_image_depth(*t->image, 0.0f, 0); else clear_image_color(*t->image, VkClearColorValue{});
    return true;
}
bool gx2_render_vk_set_unit_lod(uint32_t unit, uint32_t target, float min_lod, float max_lod) {
    if (unit >= S.units.size() || target > 3 || !S.units[unit][target]) return false;
    S.units[unit][target]->params.min_lod = min_lod; S.units[unit][target]->params.max_lod = max_lod;
    return true;
}

// ---------------------------------------------------------------------------
// Startup splash (owner request). The render thread's first job warms the
// shader cache ahead of the first game frame while the guest keeps booting;
// meanwhile loading.jpg (next to the executable, or RBREW_LOADING_IMAGE) is
// shown letterboxed on black above a progress bar. Without an image only the
// bar is drawn. The swapchain keeps the last splash until the game presents.
// ---------------------------------------------------------------------------
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#include <thread>
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#endif
void gx2_render_vk_startup_warmup(void (*progress)(double));
namespace {
constexpr uint32_t kSplashWidth = 1280, kSplashHeight = 720, kSplashBarArea = 80;
struct Splash { bool ready = false; std::vector<uint8_t> base; gx2vk::Image image; };
Splash s_splash;

bool decode_splash_image(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) {
#if defined(_WIN32)
    std::wstring path;
    if (const wchar_t* env = _wgetenv(L"RBREW_LOADING_IMAGE"); env && *env) path = env;
    else {
        wchar_t exe[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        if (!n || n >= MAX_PATH) return false;
        path = (std::filesystem::path(exe).parent_path() / L"loading.jpg").wstring();
    }
    bool ok = false;
    // WIC on a short-lived thread: no COM state is left on the render thread.
    std::thread([&] {
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return;
        IWICImagingFactory* factory = nullptr; IWICBitmapDecoder* decoder = nullptr;
        IWICBitmapFrameDecode* frame = nullptr; IWICBitmapSource* converted = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) &&
            SUCCEEDED(decoder->GetFrame(0, &frame)) &&
            SUCCEEDED(WICConvertBitmapSource(GUID_WICPixelFormat32bppRGBA, frame, &converted))) {
            UINT w = 0, h = 0;
            if (SUCCEEDED(converted->GetSize(&w, &h)) && w && h && w <= 8192 && h <= 8192) {
                rgba.resize(size_t(w) * h * 4);
                ok = SUCCEEDED(converted->CopyPixels(nullptr, w * 4, UINT(rgba.size()), rgba.data()));
                width = w; height = h;
            }
        }
        if (converted) converted->Release();
        if (frame) frame->Release();
        if (decoder) decoder->Release();
        if (factory) factory->Release();
        CoUninitialize();
    }).join();
    return ok;
#else
    (void)rgba; (void)width; (void)height;
    return false;
#endif
}

// Black canvas with the image scaled (bilinear) to fit above the bar.
void build_splash_base() {
    auto& base = s_splash.base;
    base.assign(size_t(kSplashWidth) * kSplashHeight * 4, 0);
    for (size_t i = 3; i < base.size(); i += 4) base[i] = 255;
    std::vector<uint8_t> source;
    uint32_t sw = 0, sh = 0;
    if (!decode_splash_image(source, sw, sh)) return;
    const uint32_t area = kSplashHeight - kSplashBarArea;
    const double scale = std::min(double(kSplashWidth) / sw, double(area) / sh);
    const uint32_t dw = std::max(1u, uint32_t(sw * scale)), dh = std::max(1u, uint32_t(sh * scale));
    const uint32_t x0 = (kSplashWidth - dw) / 2, y0 = (area - dh) / 2 + kSplashBarArea / 4;
    for (uint32_t y = 0; y < dh; ++y) {
        const double fy = std::clamp((y + 0.5) / scale - 0.5, 0.0, double(sh - 1));
        const uint32_t y1 = uint32_t(fy), y2 = std::min(y1 + 1, sh - 1);
        const double ty = fy - y1;
        for (uint32_t x = 0; x < dw; ++x) {
            const double fx = std::clamp((x + 0.5) / scale - 0.5, 0.0, double(sw - 1));
            const uint32_t x1 = uint32_t(fx), x2 = std::min(x1 + 1, sw - 1);
            const double tx = fx - x1;
            uint8_t* out = &base[(size_t(y0 + y) * kSplashWidth + x0 + x) * 4];
            const auto texel = [&](uint32_t px, uint32_t py, unsigned c) { return double(source[(size_t(py) * sw + px) * 4 + c]); };
            const double alpha = ((texel(x1, y1, 3) * (1 - tx) + texel(x2, y1, 3) * tx) * (1 - ty) +
                                  (texel(x1, y2, 3) * (1 - tx) + texel(x2, y2, 3) * tx) * ty) / 255.0;
            for (unsigned c = 0; c < 3; ++c) {
                const double v = (texel(x1, y1, c) * (1 - tx) + texel(x2, y1, c) * tx) * (1 - ty) +
                                 (texel(x1, y2, c) * (1 - tx) + texel(x2, y2, c) * tx) * ty;
                out[c] = uint8_t(std::clamp(v * alpha + 0.5, 0.0, 255.0));
            }
        }
    }
}

void present_splash(double progress) {
    if (!gx2vk::ready() || !gx2vk::has_surface()) return;
    if (!s_splash.ready) {
        build_splash_base();
        gx2vk::ImageDesc desc;
        desc.format = VK_FORMAT_R8G8B8A8_UNORM;
        desc.width = kSplashWidth; desc.height = kSplashHeight;
        desc.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (!gx2vk::create_image(s_splash.image, desc)) return;
        s_splash.ready = true;
    }
    const VkDeviceSize bytes = s_splash.base.size();
    const gx2vk::Span span = gx2vk::upload(bytes, 16);
    if (!span.data) return;
    std::memcpy(span.data, s_splash.base.data(), size_t(bytes));
    // Progress bar: 640x10, dark track, purple fill.
    const uint32_t bar_w = 640, bar_h = 10, bx = (kSplashWidth - bar_w) / 2, by = kSplashHeight - kSplashBarArea / 2 - bar_h / 2;
    const uint32_t filled = uint32_t(std::clamp(progress, 0.0, 1.0) * bar_w);
    for (uint32_t y = by; y < by + bar_h; ++y)
        for (uint32_t x = bx; x < bx + bar_w; ++x) {
            uint8_t* p = span.data + (size_t(y) * kSplashWidth + x) * 4;
            const bool fill = x - bx < filled;
            p[0] = fill ? 140 : 40; p[1] = fill ? 60 : 40; p[2] = fill ? 230 : 40; p[3] = 255;
        }
    end_rendering();
    const auto cb = gx2vk::cmd();
    gx2vk::barrier_if_dirty();
    VkBufferImageCopy region{};
    region.bufferOffset = span.offset;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {kSplashWidth, kSplashHeight, 1};
    vkCmdCopyBufferToImage(cb, span.buffer, s_splash.image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    gx2vk::note_gpu_write(s_splash.image);
    gx2vk::present(s_splash.image, kSplashWidth, kSplashHeight);
}
}  // namespace

void gx2_render_startup() {
    struct Startup final : Gx2AsyncWork {
        void run() override {
            present_splash(0);
            gx2_render_vk_startup_warmup(present_splash);
            present_splash(1);
            if (s_splash.image) gx2vk::destroy_image(s_splash.image);   // deferred; the swapchain keeps the picture
            s_splash = Splash{};
        }
    };
    gx2_native_post(new Startup);
}
#ifdef RBREW_GX2_QUEUE_TESTS
std::vector<uint8_t> gx2_render_vk_test_splash_canvas() {
    build_splash_base();
    std::vector<uint8_t> canvas = std::move(s_splash.base);
    s_splash = Splash{};
    return canvas;
}
#endif
