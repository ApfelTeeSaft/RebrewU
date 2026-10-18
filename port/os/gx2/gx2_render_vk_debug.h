#pragma once
// Test/diagnostic inspection of the Vulkan GX2 back-end's native objects
// (the GPU test suite reads the same facts through GPU queries).
#include <cstdint>
#include <vector>

// Raw texels of the native color view for a GX2ColorBuffer/GX2Surface
// descriptor (rows top to bottom as stored, tightly packed). False when no
// native view exists.
bool gx2_render_vk_read_color(const uint8_t* descriptor, uint8_t* mem, std::vector<uint8_t>& texels, uint32_t& width, uint32_t& height,
                              uint32_t& bytes_per_texel);
// Depth values (0..1) of the native depth view for a GX2DepthBuffer descriptor.
bool gx2_render_vk_read_depth(const uint8_t* descriptor, uint8_t* mem, std::vector<float>& depth, uint32_t& width, uint32_t& height);
// Stencil values of the native depth view.
bool gx2_render_vk_read_stencil(const uint8_t* descriptor, uint8_t* mem, std::vector<uint8_t>& stencil);
// Parameters of the guest sampler object bound to a native unit (GPU enums).
bool gx2_render_vk_sampler_params(uint32_t unit, uint32_t* wrap_s, uint32_t* min_filter, uint32_t* compare);
// Live counters of the Vulkan device layer.
uint64_t gx2_render_vk_submissions();
uint64_t gx2_render_vk_validation_errors();
const char* gx2_render_vk_last_validation_error();
uint64_t gx2_render_vk_pipelines();
uint64_t gx2_render_vk_rendering_instances();
// Waits for the GPU (GX2DrawDone without the native call wrapper).
bool gx2_render_vk_finish();
void gx2_render_vk_flush();
// glReadPixels equivalent on the bound framebuffer's color attachment:
// RGBA8 (float/unorm formats converted like GL_RGBA/GL_UNSIGNED_BYTE) or, for
// integer attachments with integer=true, RGBA 32-bit words. Rows start at
// window row y (memory row order, as GPU window coordinates).
bool gx2_render_vk_read_pixels(unsigned attachment, int x, int y, int width, int height, std::vector<uint8_t>& rgba8);
bool gx2_render_vk_read_pixels_integer(unsigned attachment, int x, int y, int width, int height, std::vector<uint32_t>& rgba32);
// Conversion of tightly packed texels of `format` (VkFormat) to RGBA8.
bool gx2_render_vk_convert_rgba8(int vk_format, const std::vector<uint8_t>& raw, size_t texels, std::vector<uint8_t>& rgba8);

// Mirrored GPU state (GPU enum values) the Vulkan draw state is derived from.
struct Gx2VkStateView {
    bool depth_test, depth_write, stencil_test, cull, blend, scissor_test, alpha_to_coverage, primitive_restart, polygon_offset_fill;
    uint32_t depth_func, cull_face, front_face, clip_origin;
    uint32_t blend_src_rgb, blend_dst_rgb, blend_src_alpha, blend_dst_alpha, blend_eq_rgb, blend_eq_alpha;
    float blend_color[4];
    uint8_t color_mask[8];
    float point_size;
    uint32_t restart_index;
    int32_t viewport[4], scissor[4];
    double depth_range[2];
    float offset_factor, offset_units, offset_clamp;
    struct Face { uint32_t func, ref, mask, fail, zfail, zpass, write_mask; } front, back;
    // Vulkan values derived for the next draw
    float vk_viewport[6];            // x, y, width, height, minDepth, maxDepth
    int vk_front_face;               // VkFrontFace
};
void gx2_render_vk_state(Gx2VkStateView& out);
// Texture object bound on a native unit for a target (0 2D, 1 2D array, 2 3D, 3 cube array).
struct Gx2VkTextureView {
    uint32_t id, width, height, depth, levels, layers;
    int vk_format;                   // view format
    uint32_t swizzle[4];             // GPU enums RED..ALPHA, ZERO, ONE
    uint32_t min_filter, mag_filter, max_level;
    bool integer, depth_format;
};
bool gx2_render_vk_unit_texture(uint32_t unit, uint32_t target, Gx2VkTextureView& out);
// glGetTexImage of that object: RGBA 32-bit words per texel (float bits, or
// integer values), all layers of the level; depth textures in word 0.
bool gx2_render_vk_read_unit(uint32_t unit, uint32_t target, uint32_t level, std::vector<uint32_t>& rgba, uint32_t& width,
                             uint32_t& height, uint32_t& layers);
// Attachments of the bound framebuffer (object ids, 0 = none) and their sizes.
struct Gx2VkFramebufferView { uint32_t color[8], color_width[8], depth, depth_width, depth_height; bool valid, depth_only; };
void gx2_render_vk_framebuffer(Gx2VkFramebufferView& out);
// glClearBufferuiv(GL_COLOR, 0, words) on the bound framebuffer.
void gx2_render_vk_clear_uint(const uint32_t words[4]);
// Translated program of the most recent draw (0 = none); primitives
// submitted by draws (vertex count / primitive size x instances).
uint32_t gx2_render_vk_last_program();
uint64_t gx2_render_vk_submitted_primitives();
// Guest sampler of a native unit as GPU sampler parameters and the Vulkan
// sampler it translates to.
struct Gx2VkSamplerView {
    bool valid;
    uint32_t wrap[3], min_filter, mag_filter, compare_func, compare_mode;
    float min_lod, max_lod, lod_bias, anisotropy, border[4];
    int vk_address[3], vk_min, vk_mag, vk_mipmap, vk_border_color;
    float vk_min_lod, vk_max_lod;
};
bool gx2_render_vk_sampler(uint32_t unit, Gx2VkSamplerView& out);
// Last swapchain image as presented (RGBA8, top row first), when the
// presentation readback is enabled (tests).
void gx2_render_vk_capture_presents(bool enable);
bool gx2_render_vk_last_present(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height);
// Negative control: issues one deliberately invalid Vulkan call (sampler
// maxAnisotropy above the device limit) so tests can prove that the
// validation layer is active. Returns false when validation is disabled.
bool gx2_render_vk_validation_selftest();
// True when every submitted command buffer has completed.
bool gx2_render_vk_gpu_idle();
// Overwrites the native texture bound on a unit (tests prove that a replay
// does not read the live image) / sets its GPU MIN/MAX_LOD parameters.
bool gx2_render_vk_clear_unit(uint32_t unit, uint32_t target);
bool gx2_render_vk_set_unit_lod(uint32_t unit, uint32_t target, float min_lod, float max_lod);
