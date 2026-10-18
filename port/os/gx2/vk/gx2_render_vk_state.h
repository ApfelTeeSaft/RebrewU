#pragma once
// Internal state of the native Vulkan GX2 renderer (gx2_render_vk*.cpp).
//
// Guest raster state is translated to Vulkan pipelines, descriptors and dynamic state.
// Capture wire values remain stable for replay compatibility.
#include "gx2_vk.h"
#include "../gx2_render.h"
#include "../gx2_surface.h"
#include "../gx2_detile.h"
#include "../r700_to_glsl.h"
#include <array>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

extern bool g_gx2_frame_trace_active;
namespace gx2r {

// GPU enum values used by the mirrored state and the capture format.
namespace wire {
constexpr uint32_t NEVER = 0x0200, LESS = 0x0201, EQUAL = 0x0202, LEQUAL = 0x0203, GREATER = 0x0204,
                   NOTEQUAL = 0x0205, GEQUAL = 0x0206, ALWAYS = 0x0207;
constexpr uint32_t ZERO = 0, ONE = 1, SRC_COLOR = 0x0300, ONE_MINUS_SRC_COLOR = 0x0301, SRC_ALPHA = 0x0302,
                   ONE_MINUS_SRC_ALPHA = 0x0303, DST_ALPHA = 0x0304, ONE_MINUS_DST_ALPHA = 0x0305, DST_COLOR = 0x0306,
                   ONE_MINUS_DST_COLOR = 0x0307, SRC_ALPHA_SATURATE = 0x0308, CONSTANT_COLOR = 0x8001,
                   ONE_MINUS_CONSTANT_COLOR = 0x8002, CONSTANT_ALPHA = 0x8003, ONE_MINUS_CONSTANT_ALPHA = 0x8004;
constexpr uint32_t FUNC_ADD = 0x8006, FUNC_SUBTRACT = 0x800A, FUNC_REVERSE_SUBTRACT = 0x800B, MIN = 0x8007, MAX = 0x8008;
constexpr uint32_t FRONT = 0x0404, BACK = 0x0405, FRONT_AND_BACK = 0x0408, CW = 0x0900, CCW = 0x0901;
constexpr uint32_t KEEP = 0x1E00, REPLACE = 0x1E01, INCR = 0x1E02, DECR = 0x1E03, INVERT = 0x150A,
                   INCR_WRAP = 0x8507, DECR_WRAP = 0x8508;
constexpr uint32_t NEAREST = 0x2600, LINEAR = 0x2601, NEAREST_MIPMAP_NEAREST = 0x2700, LINEAR_MIPMAP_NEAREST = 0x2701,
                   NEAREST_MIPMAP_LINEAR = 0x2702, LINEAR_MIPMAP_LINEAR = 0x2703;
constexpr uint32_t REPEAT = 0x2901, MIRRORED_REPEAT = 0x8370, CLAMP_TO_EDGE = 0x812F, CLAMP_TO_BORDER = 0x812D,
                   MIRROR_CLAMP_TO_EDGE = 0x8743, MIRROR_CLAMP_TO_BORDER = 0x8912;
constexpr uint32_t TEXTURE_2D = 0x0DE1, TEXTURE_3D = 0x806F, TEXTURE_2D_ARRAY = 0x8C1A, TEXTURE_CUBE_MAP_ARRAY = 0x9009,
                   TEXTURE_1D = 0x0DE0;
constexpr uint32_t UPPER_LEFT = 0x8CA2, LOWER_LEFT = 0x8CA1, NEGATIVE_ONE_TO_ONE = 0x935E;
constexpr uint32_t RED = 0x1903, GREEN = 0x1904, BLUE = 0x1905, ALPHA = 0x1906, RGBA = 0x1908, RGB = 0x1907, RG = 0x8227,
                   RGBA_INTEGER = 0x8D99, DEPTH_COMPONENT = 0x1902, DEPTH_STENCIL = 0x84F9;
constexpr uint32_t UNSIGNED_BYTE = 0x1401, UNSIGNED_SHORT = 0x1403, UNSIGNED_INT = 0x1405, INT = 0x1404, FLOAT = 0x1406,
                   HALF_FLOAT = 0x140B;
constexpr uint32_t RGBA8 = 0x8058, RGBA32F = 0x8814, DEPTH_COMPONENT32F = 0x8CAC;
}

// GX2 surface format -> Vulkan, mirroring gx2_wire_format().
struct Format {
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;   // image format
    VkFormat attachment = VK_FORMAT_R8G8B8A8_UNORM; // view used as a color attachment
    uint32_t bpp = 4;          // bytes per element (0 = compressed)
    bool depth = false, compressed = false, integer = false, srgb = false;
    uint32_t block = 0;        // compressed bytes per 4x4 block
    uint32_t wire_internal = wire::RGBA8, wire_format = wire::RGBA, wire_type = wire::UNSIGNED_BYTE;  // capture metadata
};
Format gx2_vk_format(uint32_t gx2_format);

inline uint32_t descriptor_word(const uint8_t* data, uint32_t offset) {
    const auto* p = data + offset;
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// ---------------------------------------------------------------------------
// GPU texture object equivalent. GPU keeps filtering/wrap/level parameters and
// the swizzle on the texture object; units reference objects.
// ---------------------------------------------------------------------------
struct TexParams {
    uint32_t min_filter = wire::NEAREST_MIPMAP_LINEAR, mag_filter = wire::LINEAR;
    uint32_t wrap_s = wire::REPEAT, wrap_t = wire::REPEAT, wrap_r = wire::REPEAT;
    uint32_t base_level = 0, max_level = 1000;
    float min_lod = -1000.0f, max_lod = 1000.0f, lod_bias = 0.0f, anisotropy = 1.0f;
    std::array<float, 4> border{};
    uint32_t compare_mode = 0, compare_func = wire::LEQUAL;
};
struct TexObject {
    gx2vk::Image* image = nullptr;       // storage (may be null: no storage = incomplete)
    uint32_t target = wire::TEXTURE_2D;
    VkFormat view_format = VK_FORMAT_UNDEFINED;
    uint32_t levels = 0, layers = 1;     // allocated levels / layers of the storage
    bool integer = false, depth = false, compressed = false;
    uint32_t swizzle = 0x06880000;       // SQ_TEX_RESOURCE_WORD4: R=0, G=1, B=2, A=3
    TexParams params;
    uint32_t id = 0;                     // stable diagnostic id ("gl tex")
    bool complete_storage = true;        // false: GPU would have no level 0
};

struct ColorTarget {             // CBEntry
    gx2vk::Image image;
    TexObject tex;               // sampled through texture aliasing
    uint32_t width = 0, height = 0, gx2_format = 0;
    Format fmt;
    uint32_t id = 0;
};
struct DepthTarget {             // DepthEntry
    gx2vk::Image image;
    TexObject tex;
    uint32_t width = 0, height = 0, gx2_format = 0;
    VkFormat format = VK_FORMAT_D32_SFLOAT;
    bool stencil = false;
    uint32_t id = 0;
};
struct Texture {                 // TexEntry
    gx2vk::Image image;
    TexObject tex;
    bool created = false;
    uint32_t image_addr = 0, image_size = 0, width = 0, height = 0, layers = 1, format = 0, mip_levels = 1;
    uint64_t content_hash = 0, written = 0;
    bool hash_valid = false, verify = false;
    // Native copies of live color views (array/volume slices or BC blocks).
    // Reuse only while both source and destination contents are unchanged.
    std::map<uint32_t, uint64_t> copied_sources;
    uint64_t copied_destination = 0;
};
struct ColorView {               // glTextureView of a 32-bit-class render target
    TexObject tex;
};

using ColorImageKey = std::array<uint32_t, 9>;
using TextureLayout = std::array<uint32_t, GX2TEX_COMP_MAP / 4>;
using TextureKey = std::pair<uintptr_t, TextureLayout>;

template<class Target> struct ImageViewRanges {
    using Iterator = typename std::map<ColorImageKey, Target>::const_iterator;
    size_t map_size = size_t(-1);
    std::unordered_map<uint32_t, std::pair<Iterator, Iterator>> ranges;
};
struct TextureEntryCache {
    const uint8_t* mem = nullptr;
    std::array<uint8_t, GX2TEX_COMP_MAP> layout{};
    Texture* entry = nullptr;
};

// ---------------------------------------------------------------------------
// Mirrored GPU state.
// ---------------------------------------------------------------------------
struct StencilFace { uint32_t func = wire::ALWAYS, ref = 0, mask = 0xFF, fail = wire::KEEP, zfail = wire::KEEP, zpass = wire::KEEP, write_mask = 0xFF; };
struct RasterState {
    bool depth_test = false, depth_write = true;
    uint32_t depth_func = wire::LESS;
    bool stencil_test = false;
    StencilFace front, back;
    bool cull = false;
    uint32_t cull_face = wire::BACK, front_face = wire::CCW;
    bool blend = false;
    uint32_t blend_src_rgb = wire::ONE, blend_dst_rgb = wire::ZERO, blend_src_alpha = wire::ONE, blend_dst_alpha = wire::ZERO;
    uint32_t blend_eq_rgb = wire::FUNC_ADD, blend_eq_alpha = wire::FUNC_ADD;
    std::array<float, 4> blend_color{};
    std::array<uint8_t, 8> color_mask{15, 15, 15, 15, 15, 15, 15, 15};
    bool alpha_to_coverage = false;      // no effect: every native target is single-sample (as in GPU)
    float point_size = 1.0f;
    bool primitive_restart = false;
    uint32_t restart_index = 0;
    int32_t viewport[4]{0, 0, 0, 0};
    double depth_range[2]{0.0, 1.0};
    uint32_t clip_origin = wire::LOWER_LEFT;
    bool scissor_test = false;
    int32_t scissor[4]{0, 0, 0, 0};
    bool polygon_offset_fill = false, polygon_offset_line = false, polygon_offset_point = false;
    float offset_factor = 0, offset_units = 0, offset_clamp = 0;
    std::array<float, 4> clear_color{};
    double clear_depth = 1.0;
    uint32_t clear_stencil = 0;
};

// Framebuffer composition (bind_target_framebuffer).
struct Framebuffer {
    std::array<ColorTarget*, 8> colors{};
    DepthTarget* depth = nullptr;
    bool depth_only = false;
    bool valid = false;
    bool operator==(const Framebuffer& o) const { return colors == o.colors && depth == o.depth && depth_only == o.depth_only && valid == o.valid; }
};

// Native GX2 occlusion query (see gx2_render.cpp): host measurement split in
// segments shared by all open guest queries. Vulkan queries cannot span
// rendering instances, so a segment holds one query per instance.
struct QuerySegment {
    std::vector<uint32_t> indices;
    uint64_t samples = 0, serial = 0;
    bool ready = false, closed = false;
    ~QuerySegment();
};
struct NativeQuery {
    uint8_t* mem = nullptr;
    uint32_t address = 0, type = 0;
    bool open = false, ended = false, published = false;
    std::vector<std::shared_ptr<QuerySegment>> segments;
};

struct BoundTexture { uint32_t address = 0; std::array<uint8_t, GX2TEX_SIZEOF> descriptor{}; };
struct AttribBufferState { uint32_t guest_addr = 0, size = 0, stride = 0; bool present = false; };
struct UniformBlock {
    uint32_t address = 0, size = 0;
    const uint8_t* uploaded_mem = nullptr;
    uint32_t uploaded_address = 0, uploaded_size = 0;
    uint64_t written = 0;
    bool uploaded = false;
    std::vector<uint8_t> uploaded_bytes;
    gx2vk::Span span;            // resident copy of uploaded_bytes
};
struct NativeContextSnapshot {
    std::array<ColorTarget*, 8> color_targets{};
    DepthTarget* depth_target = nullptr;
    std::array<BoundTexture, 96> bound_textures{};
    std::array<std::array<uint32_t, 3>, 32> attrib_buffers{};
    std::array<std::array<std::array<uint32_t, 2>, 16>, 2> uniform_blocks{};
    uint32_t vertex_shader = 0, pixel_shader = 0, fetch_shader = 0;
    bool vertex_shader_from_registers = false, pixel_shader_from_registers = false;
};
// program: the guest fetch-program bytes the layout belongs to (empty when
// unknown). Scenes reload models into freed memory, so the address alone
// does not identify a fetch program.
struct FetchShaderInfo { std::vector<R700FetchAttribute> attributes; std::vector<uint8_t> program; };

// Sampler state of one GX2 sampler slot (stage*18 + slot), as GPU sampler
// object parameters.
struct SamplerState { TexParams params; bool valid = false; };

// ---------------------------------------------------------------------------
// Renderer globals (render thread only).
// ---------------------------------------------------------------------------
struct State {
    std::map<ColorImageKey, ColorTarget> cb_map;
    std::map<ColorImageKey, DepthTarget> depth_map;
    std::map<TextureKey, Texture> tex_map;
    // Maps only grow until shutdown. Native target insertion invalidates the
    // cached ranges; texture nodes retain their addresses across insertion.
    ImageViewRanges<ColorTarget> color_ranges;
    ImageViewRanges<DepthTarget> depth_ranges;
    std::array<TextureEntryCache, 96> texture_entries{};
    // Address index of tex_map image/mip ranges for gx2_render_invalidate_textures,
    // rebuilt when tex_map grows (entries are only added, or all reset).
    struct TexRange { uint32_t start; uint64_t end; Texture* entry; };
    std::vector<TexRange> tex_ranges;
    std::vector<uint64_t> tex_range_max_end;  // running max of end over tex_ranges
    size_t tex_ranges_size = SIZE_MAX;
    std::map<std::array<uint32_t, 3>, ColorView> color_views;   // {target id, fmt, word4}
    std::map<std::array<uint32_t, 3>, ColorView> depth_views;   // {depth id, texture target, word4}
    std::array<ColorTarget*, 8> color_targets{};
    DepthTarget* depth_target = nullptr;
    std::array<BoundTexture, 96> bound_textures{};
    // GPU texture units: bound object per target (2D, 2D_ARRAY, 3D, CUBE_ARRAY).
    std::array<std::array<TexObject*, 4>, 96> units{};
    std::array<SamplerState, 54> samplers{};
    std::array<bool, 54> sampler_compare{};
    std::unordered_map<uint32_t, NativeContextSnapshot> context_snapshots;
    uint32_t target_masks = 0xFFFFFFFF;
    bool color_write = true;
    bool conditional_draw = true;
    RasterState raster;
    Framebuffer framebuffer;             // current GPU framebuffer binding
    uint32_t current_fbo_id = 0;         // diagnostic name of the bound framebuffer
    std::array<uint32_t, 0x10000> regs{};
    bool vs_from_regs = false, ps_from_regs = false;
    uint32_t cur_vs = 0, cur_ps = 0, cur_vs_attribs = 0;
    std::unordered_map<uint32_t, FetchShaderInfo> fetch_shaders;
    AttribBufferState attrib[32];
    uint32_t active_fetch = 0;
    std::array<int32_t, 1024> uniform_words[2]{};
    UniformBlock uniform_blocks[2][16];
    std::array<gx2vk::Span, 48> ubo_binding_points{};     // GPU UBO binding points (last bound)
    // scan buffers
    uint32_t scan_formats[2]{};
    ColorTarget* scan_source[2]{};       // TV, DRC
    gx2vk::Image* scan_image[2]{};
    uint32_t scan_w[2]{}, scan_h[2]{};
    uint32_t scan_tv_source_id = 0;
    uint32_t next_object_id = 1;
    // draw bookkeeping
    uint32_t draw_instances = 1, draw_first_instance = 0, instance_uniform = 0;
    bool single_instance_fetch = false;
    float sampler_max_anisotropy = 1.0f;
};
State& state();

// Shared helpers implemented in gx2_render_vk.cpp
ColorImageKey color_image_key(uint32_t addr, uint8_t* mem, const uint8_t* descriptor);
TextureKey texture_image_key(uint8_t* mem, const uint8_t* data);
void bind_target_framebuffer();
void apply_native_raster_registers(const Gx2RegisterWrite* writes, size_t count);
uint32_t gx2_compare_to_gl(uint32_t f);
VkCompareOp gl_compare_to_vk(uint32_t func);
VkFilter gl_filter(uint32_t filter);
// Rendering control (gx2_render_vk_draw.cpp)
void end_rendering();                      // closes the active rendering instance (if any)
bool ensure_rendering(bool streamout_only=false); // GPU-only updates need no raster attachments
bool rendering_active();
// One-shot TV swapchain readback, after the normal frame has been drawn.
void capture_next_present(const std::string& path);
VkRect2D render_area();
void note_framebuffer_changed();
void query_rendering_begin(VkCommandBuffer cb);
void query_rendering_end(VkCommandBuffer cb);
void before_submit();
void shutdown_draw_state();
TexObject* dummy_texture(uint32_t dim, bool integer, bool sint, bool shadow);
VkImageView tex_object_view(TexObject& tex, uint32_t shader_dim, bool integer_sampler, bool sint_sampler);
VkSampler tex_object_sampler(const TexParams& params, bool shadow, bool integer);

// Internal fullscreen-triangle passes (depth->R32F transfer, capture readback).
bool depth_to_color_pass(DepthTarget& source, ColorTarget& destination, uint32_t width, uint32_t height);
// Uploads a CPU-side linear level into a Vulkan image (outside rendering).
void upload_image_level(gx2vk::Image& image, uint32_t level, uint32_t first_layer, uint32_t layers,
                        uint32_t width, uint32_t height, uint32_t depth, const uint8_t* data, size_t bytes,
                        VkImageAspectFlags aspect);
void clear_image_color(gx2vk::Image& image, const VkClearColorValue& value);
void clear_image_depth(gx2vk::Image& image, float depth, uint32_t stencil);
// Shadow diagnostics (RBREW_SHADOW_LOG): logs depth-buffer creation/binds,
// clears, depth-only passes and depth sampling. Unlike the frame trace it
// never reads back, so it stays cheap enough to leave armed while playing.
bool shadow_log();
// Resolve GPU-written color/depth views at consumption time, including copied layers.
void refresh_bound_render_texture(uint32_t unit, uint8_t* mem);
// Per-frame shadow tally; called once per frame with the present signature.
void shadow_log_frame_end();
// Counters
struct Counters {
    uint64_t texture_uploads = 0, alias_probes = 0, framebuffer_binds = 0, uniform_lookups = 0, uniform_uploads = 0,
             uniform_buffer_uploads = 0, state_queries = 0, texture_hashes = 0, vertex_uploads = 0, programs_linked = 0,
             programs_reused = 0, pipelines = 0, draws = 0, rendering_instances = 0,
             image_view_searches = 0, texture_entry_searches = 0;
};
Counters& counters();
// Frame trace flag shared with gx2_draw.cpp (RBREW_FRAME_TRACE).
#define g_trace_active ::g_gx2_frame_trace_active
}  // namespace gx2r
