// gx2_render.h — OpenGL rendering back-end for GX2
//
// Provides the GL state manager called by gx2_draw.cpp and gx2_context.cpp.
// All functions are no-ops when HAVE_SDL2 is not defined.
#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// Call once after SDL_GL_CreateContext succeeds in GX2Init.
// Loads all GL 3.x+ function pointers and creates the stub shader program.
void gx2_render_init();

// ---------------------------------------------------------------------------
// Framebuffer management
// ---------------------------------------------------------------------------

// Bind (creating if needed) the FBO for this GX2ColorBuffer guest address.
// Allocates a GL colour texture sized to the surface dimensions/format.
void gx2_render_set_color_buffer(uint32_t cb_addr, uint8_t* mem);

// Attach (creating if needed) a depth/stencil renderbuffer to the FBO that
// was most recently bound by gx2_render_set_color_buffer.
void gx2_render_set_depth_buffer(uint32_t db_addr, uint8_t* mem);

// ---------------------------------------------------------------------------
// Clear operations (operate on the currently-bound FBO)
// ---------------------------------------------------------------------------

// Colours come from PPC float registers f1-f4.
void gx2_render_clear_color(float r, float g, float b, float a);

// clear_mode is a bitmask of GX2_CLEAR_FLAGS_DEPTH | GX2_CLEAR_FLAGS_STENCIL.
void gx2_render_clear_depth_stencil(float depth, uint8_t stencil,
                                    uint32_t clear_mode);

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

// Upload (or re-upload if the image pointer changed) a GX2Texture to the GL
// texture unit at 'slot'.  Supports RGBA8, sRGB8A8, BC1-3, and float formats.
void gx2_render_upload_texture(uint32_t tex_addr, uint32_t slot, uint8_t* mem);

// Bind cached GL texture (no re-upload) for vertex / geometry shader slots.
void gx2_render_bind_texture(uint32_t tex_addr, uint32_t slot, uint8_t* mem);

// ---------------------------------------------------------------------------
// Scan-buffer / present
// ---------------------------------------------------------------------------

// Record which FBO backs the TV or DRC scan buffer.
// scan_target: 0 = GX2_SCAN_TARGET_TV (0), 1 = GX2_SCAN_TARGET_DRC (4)
void gx2_render_copy_to_scan(uint32_t cb_addr, uint32_t scan_target,
                              uint8_t* mem);

// Blit the current scan FBO to the default framebuffer and swap.
// Called from GX2SwapScanBuffers.
void gx2_render_present(void* sdl_window, bool gamepad_mode);


// Called from GX2InitFetchShaderEx: records the attrib stream array associated
// with the fetch shader so it can be used at draw time.
void gx2_render_save_fetch_shader(uint32_t fs_addr,
                                  uint32_t attrib_count,
                                  uint32_t streams_guest_addr);

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
                     uint8_t* mem);

// Issue an indexed draw call.
// index_type is a GX2IndexType value; idx_guest_addr is the guest pointer to
// index data.  Big-endian U16/U32 indices are byte-swapped automatically.
void gx2_render_draw_indexed(uint32_t prim_type,
                             uint32_t count,
                             uint32_t index_type,
                             uint32_t idx_guest_addr,
                             uint32_t first_vertex,
                             uint8_t* mem);


// Called from GX2SetVertexShader / GX2SetPixelShader.
// Reads the shader binary from the GX2VertexShader / GX2PixelShader struct,
// translates it to GLSL, and caches the compiled GL program.
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
