// gx2_render.h — Vulkan rendering for GX2
//
// Provides the GPU state manager called by gx2_draw.cpp and gx2_context.cpp.
// Renderer-free ABI fixtures have a separate headless implementation.
#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// Create the Vulkan device and swapchain after the SDL window exists.
void gx2_render_init();
// Show the loading splash and warm shader caches on the render thread.
void gx2_render_startup();
// Vulkan back-end: waits for the GPU and releases the device (GX2Shutdown).
void gx2_render_shutdown();

// ---------------------------------------------------------------------------
// Framebuffer management
// ---------------------------------------------------------------------------

// Bind (creating if needed) the FBO for this GX2ColorBuffer guest address.
// Allocates a GPU colour texture sized to the surface dimensions/format.
void gx2_render_set_color_buffer(uint32_t cb_addr, uint8_t* mem, const uint8_t* descriptor = nullptr, uint32_t target = 0);

// Attach (creating if needed) a sampleable depth/stencil texture to the FBO that
// was most recently bound by gx2_render_set_color_buffer.
void gx2_render_set_depth_buffer(uint32_t db_addr, uint8_t* mem, const uint8_t* descriptor = nullptr);
void gx2_render_set_target_channel_masks(uint32_t masks);
void gx2_render_save_context_state(uint32_t context_state);
void gx2_render_restore_context_state(uint32_t context_state, uint8_t* mem);

// ---------------------------------------------------------------------------
// Clear operations (operate on the currently-bound FBO)
// ---------------------------------------------------------------------------

// Colours come from PPC float registers f1-f4.
void gx2_render_clear_color(float r, float g, float b, float a);

// clear_mode is a bitmask of GX2_CLEAR_FLAGS_DEPTH | GX2_CLEAR_FLAGS_STENCIL.
void gx2_render_clear_depth_stencil(float depth, uint8_t stencil,
                                    uint32_t clear_mode);
// GX2ClearColor: clears the given color buffer, bound or not. descriptor: the
// buffer's words captured when a display list recorded the call (gx2.rpl
// encodes the selected view into the list), else read from mem at cb_addr.
void gx2_render_clear_color_buffer(uint32_t cb_addr, uint8_t* mem, float r, float g,
                                   float b, float a, const uint8_t* descriptor = nullptr);
// GX2ClearDepthStencilEx: clears the given depth buffer, bound or not.
void gx2_render_clear_depth_buffer(uint32_t db_addr, uint8_t* mem, float depth,
                                   uint8_t stencil, uint32_t clear_mode,
                                   const uint8_t* descriptor = nullptr);

// Combined clear (colour + depth + stencil).
void gx2_render_clear_buffers(float r, float g, float b, float a,
                               float depth, uint8_t stencil);

// ---------------------------------------------------------------------------
// Raster state
// ---------------------------------------------------------------------------

void gx2_render_set_viewport(float x, float y, float w, float h,
                              float near_z, float far_z);

void gx2_render_set_scissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

// depth_test / depth_write / GX2CompareFunction value
void gx2_render_set_depth_stencil_control(bool depth_test, bool depth_write,
                                          uint32_t gx2_compare_func);

// front_face: GX2_FRONT_FACE_CCW / CW
// cull_front / cull_back as booleans
void gx2_render_set_polygon_control(uint32_t front_face,
                                    bool cull_front, bool cull_back);

// Per-target blend.  enable=false → blending disabled for this target.
// src/dst are GX2_BLEND_MODE_* values; combine is GX2_BLEND_COMBINE_MODE_*.
void gx2_render_set_blend_control(bool blend_enable,
                                  uint32_t src_color, uint32_t dst_color,
                                  uint32_t color_combine,
                                  uint32_t src_alpha, uint32_t dst_alpha,
                                  uint32_t alpha_combine);

// ---------------------------------------------------------------------------
// Texture management
// ---------------------------------------------------------------------------

// Upload (or re-upload if the image pointer changed) a GX2Texture to the GPU
// texture unit at 'slot'.  Supports RGBA8, sRGB8A8, BC1-3, and float formats.
void gx2_render_upload_texture(uint32_t tex_addr, uint32_t slot, uint8_t* mem, const uint8_t* descriptor = nullptr);

// Bind cached GPU texture (no re-upload) for vertex / geometry shader slots.
void gx2_render_bind_texture(uint32_t tex_addr, uint32_t slot, uint8_t* mem, const uint8_t* descriptor = nullptr);

// ---------------------------------------------------------------------------
// Scan-buffer / present
// ---------------------------------------------------------------------------

// Record which FBO backs the TV or DRC scan buffer.
// scan_target: 0 = GX2_SCAN_TARGET_TV (0), 1 = GX2_SCAN_TARGET_DRC (4)
void gx2_render_copy_to_scan(uint32_t cb_addr, uint32_t scan_target,
                              uint8_t* mem, const uint8_t* descriptor = nullptr);
void gx2_render_set_scan_format(bool gamepad,uint32_t format);

// Blit the current scan FBO to the default framebuffer and swap.
// Called from GX2SwapScanBuffers.
void gx2_render_present(void* sdl_window, bool gamepad_mode);


// Called from GX2InitFetchShaderEx: records the attrib stream array associated
// with the fetch shader so it can be used at draw time.
void gx2_render_save_fetch_shader(uint32_t fs_addr,
                                  uint32_t attrib_count,
                                  uint32_t streams_guest_addr, uint8_t* mem);

// Called from GX2SetFetchShader: makes fs_addr the active fetch shader.
void gx2_render_set_active_fetch_shader(uint32_t fs_addr);

// Called from GX2SetAttribBuffer: records the guest vertex buffer for a slot.
// The buffer is uploaded (with endian-swap) to a VBO at draw time.
// slot     — attrib buffer index (0-31)
// guest_addr — guest memory address of the vertex data
// size     — total byte length of the buffer
// stride   — bytes between consecutive vertices
void gx2_render_set_attrib_buffer(uint32_t slot,
                                  uint32_t guest_addr,
                                  uint32_t size,
                                  uint32_t stride);

// Issue a non-indexed draw call after setting up VAO from the active fetch
// shader and the currently-registered attrib buffers.
// prim_type is a GX2PrimitiveMode value.
void gx2_render_draw(uint32_t prim_type,
                     uint32_t count,
                     uint32_t first_vertex,
                     uint8_t* mem,uint32_t instances=1,uint32_t first_instance=0);

// Issue an indexed draw call.
// index_type is a GX2IndexType value; idx_guest_addr is the guest pointer to
// index data.  Big-endian U16/U32 indices are byte-swapped automatically.
void gx2_render_draw_indexed(uint32_t prim_type,
                             uint32_t count,
                             uint32_t index_type,
                             uint32_t idx_guest_addr,
                             uint32_t first_vertex,
                             uint8_t* mem,uint32_t instances=1,uint32_t first_instance=0);


// Called from GX2SetVertexShader / GX2SetPixelShader.
// Reads the shader binary from the GX2VertexShader / GX2PixelShader struct,
// translates it to GLSL, and caches the compiled GPU program.
// vs_addr  — guest ptr to GX2VertexShader struct (0 = unset)
// ps_addr  — guest ptr to GX2PixelShader  struct (0 = unset)
// num_attribs — number of vertex attributes (from GX2VertexShader)
void gx2_render_set_vertex_shader(uint32_t vs_addr, uint8_t* mem);
void gx2_render_set_pixel_shader (uint32_t ps_addr, uint8_t* mem);

// Called from GX2Set*UniformReg.
// Writes float4 registers into the VS or PS constant buffer (vc[]).
// is_pixel  — true = pixel shader constants
// index     — destination register index (into vc[])
// src_addr  — guest ptr to the source float data (big-endian)
// num_vecs  — number of vec4s to upload (size/16)
void gx2_render_set_uniform_reg(bool is_pixel,
                                uint32_t index,
                                uint32_t src_addr,
                                uint32_t num_vecs,
                                uint8_t* mem);

// Called from GX2Set*UniformBlock.
// Maps a guest memory block into the VS or PS UBO.
// is_pixel  — true = pixel shader UBO
// block_index — UBO binding index from game (0-based)
// src_addr  — guest ptr to the float data
// size      — byte size of the block
void gx2_render_set_uniform_block(bool is_pixel,
                                  uint32_t block_index,
                                  uint32_t src_addr,
                                  uint32_t size,
                                  uint8_t* mem);

// Register file shared with the shader translator. Static display lists
// shipped in game files carry register-set packets; GX2CallDisplayList
// decodes them (gx2_pm4_state, read-only) and loads them here.
#include <stddef.h>
struct Gx2RegisterWrite;
void gx2_render_apply_registers(const Gx2RegisterWrite* writes, size_t count);
uint32_t gx2_render_register(uint32_t index);
// Comparison is selected by the shader's SAMPLE_C opcode, not depth format.
void gx2_render_set_sampler_comparison(uint32_t native_slot,bool compare);
// GX2SetSpecialState (decaf GXSpecialState). Native Vulkan models
// ClearColorAsDepth (8); the GPU baseline ignores special states.
void gx2_render_set_special_state(uint32_t state, bool enable);
void gx2_render_copy_surface(const uint8_t* src,uint32_t src_mip,uint32_t src_slice,
                             const uint8_t* dst,uint32_t dst_mip,uint32_t dst_slice,uint8_t* mem);

// GX2SetColorControl colorBufferEnable: false disables all colour writes.
void gx2_render_set_color_write(bool enable);

// Shader constant registers (DWORD index/count, gx2.rpl 0202f608/0202f754)
// from host values already in native order - used by display-list replay,
// which snapshots the values at record time like the console's command list.
void gx2_render_set_uniform_words(bool is_pixel, uint32_t index, const uint32_t* words, uint32_t count);
uint32_t gx2_render_uniform_word(bool is_pixel, uint32_t index);

// GX2Invalidate with the texture bit: re-upload textures overlapping the range.
void gx2_render_invalidate_textures(uint32_t addr, uint32_t size);
// Number of CPU image uploads, for regression tests and profiling.
uint64_t gx2_render_texture_upload_count();
// Native render-target view candidates examined by texture bindings.
uint64_t gx2_render_texture_alias_probe_count();
uint64_t gx2_render_framebuffer_bind_count();
uint64_t gx2_render_uniform_lookup_count();
uint64_t gx2_render_uniform_upload_count();
uint64_t gx2_render_uniform_buffer_upload_count();
void gx2_render_query_begin(uint32_t type,uint32_t address,uint8_t* mem);
void gx2_render_query_end(uint32_t type,uint32_t address,uint8_t* mem);
bool gx2_render_query_result(uint32_t address,uint32_t output,uint8_t* mem);
// Occlusion fast-path bookkeeping (gx2_draw.cpp), called by the renderer when
// a query begin executes and when its counters reach guest memory.
void gx2_query_render_begin(uint32_t address);
void gx2_query_render_published(uint32_t address);
void gx2_render_query_poll();
void gx2_render_conditional_begin(uint32_t type,uint32_t address,bool no_wait,bool pixels_pass,uint8_t* mem);
void gx2_render_conditional_end();
// GPU state queries (glIsEnabled/glGet*) issued by the renderer; each one
// stalls a threaded driver until its command queue drains.
uint64_t gx2_render_state_query_count();
// Content verifications of cached textures (hashing guest image bytes) and
// vertex buffer uploads; both skip guest pages the CPU has not written.
uint64_t gx2_render_texture_hash_count();
uint64_t gx2_render_vertex_upload_count();
// GPU programs compiled and linked (identical translated shaders are reused).
uint64_t gx2_render_program_link_count();

// Attributes of the active fetch shader (0 when none is known) - for tests.
uint32_t gx2_render_active_fetch_attribute_count();

// Attribute layout decoded from a pre-built fetch program (gx2_fetch_decode),
// keyed by program pointer like GX2InitFetchShaderEx layouts.
#include <vector>
#include "r700_to_glsl.h"
void gx2_render_store_fetch_attributes(uint32_t program, const std::vector<R700FetchAttribute>& attributes);
// Same, recording the program bytes the layout was decoded from; empty
// attributes keep a known layout and only record the bytes.
void gx2_render_store_fetch_attributes(uint32_t program, const std::vector<R700FetchAttribute>& attributes,
                                       const uint8_t* bytes, uint32_t size);
// True when program has a layout recorded for exactly these bytes (or for
// unknown bytes). Freed scene memory is reused for other fetch programs.
bool gx2_render_fetch_program_current(uint32_t program, const uint8_t* bytes, uint32_t size);

// Draw capture / offline replay. RBREW_CAPTURE_DRAW=<tv frame>:<draw|*>
// (frame numbering as RBREW_FRAME_TRACE) writes the draw's guest regions
// (shaders, uniform blocks, vertex/index buffers, texture descriptors/images)
// and renderer state, including GPU-produced textures and prepass depth, to
// RBREW_CAPTURE_DIR (default work/logs/capture). Captures hold game data.
void gx2_render_capture_next_draw(const char* path);
struct Gx2ReplayResult {
    uint32_t fragments{}, fragments_no_depth_cull{}, width{}, height{}, nonblack{};
    uint8_t centre[4]{};
    bool top_down{};
    std::vector<uint8_t> rgba; // captured-state pass; row order given by top_down
    std::vector<float> positions; // VS gl_Position per assembled vertex (xyzw)
};
// Loads a capture into `mem` (commit(addr,size) makes a region writable,
// may be null), restores renderer state and issues the same draw into an
// offscreen target of the captured size. Needs a current GPU context.
bool gx2_render_replay_capture(const char* path, uint8_t* mem,
                               void (*commit)(uint32_t addr, uint32_t size),
                               Gx2ReplayResult& out);
