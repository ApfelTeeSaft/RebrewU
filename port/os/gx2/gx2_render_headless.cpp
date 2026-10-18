#include "gx2_render.h"
#include "gx2_pm4_state.h"
#include "../os_common.h"
#include <vector>
// Renderer-free guest ABI fixtures; never linked into the game.
bool g_gx2_frame_trace_active=false;
void gx2_render_init() {}
void gx2_render_startup() {}
void gx2_render_set_color_buffer(uint32_t, uint8_t*, const uint8_t*, uint32_t) {}
void gx2_render_set_depth_buffer(uint32_t, uint8_t*, const uint8_t*) {}
void gx2_render_set_target_channel_masks(uint32_t) {}
void gx2_render_save_context_state(uint32_t) {}
void gx2_render_restore_context_state(uint32_t, uint8_t*) {}
void gx2_render_clear_color(float, float, float, float) {}
void gx2_render_clear_depth_stencil(float, uint8_t, uint32_t) {}
void gx2_render_clear_depth_buffer(uint32_t, uint8_t*, float, uint8_t, uint32_t, const uint8_t*) {}
void gx2_render_clear_color_buffer(uint32_t, uint8_t*, float, float, float, float, const uint8_t*) {}
void gx2_render_clear_buffers(float, float, float, float, float, uint8_t) {}
void gx2_render_set_viewport(float, float, float, float, float, float) {}
void gx2_render_set_scissor(uint32_t, uint32_t, uint32_t, uint32_t) {}
void gx2_render_set_depth_stencil_control(bool, bool, uint32_t) {}
void gx2_render_set_polygon_control(uint32_t, bool, bool) {}
void gx2_render_set_color_write(bool) {}
void gx2_render_invalidate_textures(uint32_t, uint32_t) {}
uint64_t gx2_render_texture_upload_count() { return 0; }
uint64_t gx2_render_texture_alias_probe_count() { return 0; }
uint64_t gx2_render_framebuffer_bind_count() { return 0; }
uint64_t gx2_render_uniform_lookup_count() { return 0; }
uint64_t gx2_render_uniform_upload_count() { return 0; }
uint64_t gx2_render_uniform_buffer_upload_count() { return 0; }
void gx2_render_query_begin(uint32_t,uint32_t,uint8_t*) {}
void gx2_render_query_end(uint32_t,uint32_t,uint8_t*) {}
bool gx2_render_query_result(uint32_t,uint32_t output,uint8_t* mem) { if(output)rbrew_write64(mem,output,0);return false; }
void gx2_render_query_poll() {}
void gx2_render_conditional_begin(uint32_t,uint32_t,bool,bool,uint8_t*) {}
void gx2_render_conditional_end() {}
uint64_t gx2_render_state_query_count() { return 0; }
uint64_t gx2_render_texture_hash_count() { return 0; }
uint64_t gx2_render_vertex_upload_count() { return 0; }
uint64_t gx2_render_program_link_count() { return 0; }
void gx2_render_set_blend_control(bool, uint32_t, uint32_t, uint32_t,
                                  uint32_t, uint32_t, uint32_t) {}
void gx2_render_upload_texture(uint32_t, uint32_t, uint8_t*, const uint8_t*) {}
void gx2_render_bind_texture(uint32_t, uint32_t, uint8_t*, const uint8_t*) {}
void gx2_render_copy_to_scan(uint32_t, uint32_t, uint8_t*, const uint8_t*) {}
void gx2_render_set_scan_format(bool,uint32_t) {}
void gx2_render_present(void*, bool) {}
void gx2_render_save_fetch_shader(uint32_t, uint32_t, uint32_t, uint8_t*) {}
void gx2_render_set_active_fetch_shader(uint32_t) {}
uint32_t gx2_render_active_fetch_attribute_count() { return 0; }
void gx2_render_store_fetch_attributes(uint32_t, const std::vector<R700FetchAttribute>&) {}
void gx2_render_store_fetch_attributes(uint32_t, const std::vector<R700FetchAttribute>&, const uint8_t*, uint32_t) {}
bool gx2_render_fetch_program_current(uint32_t, const uint8_t*, uint32_t) { return true; }
void gx2_render_set_attrib_buffer(uint32_t, uint32_t, uint32_t, uint32_t) {}
void gx2_render_draw(uint32_t, uint32_t, uint32_t, uint8_t*,uint32_t,uint32_t) {}
void gx2_render_draw_indexed(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint8_t*,uint32_t,uint32_t) {}
void gx2_render_set_vertex_shader(uint32_t, uint8_t*) {}
void gx2_render_set_pixel_shader(uint32_t, uint8_t*) {}
static uint32_t s_stub_uniform_words[2][1024];
void gx2_render_set_uniform_words(bool is_pixel, uint32_t index, const uint32_t* words, uint32_t count) {
    index &= 0x7FFF;
    if (!words || index >= 1024 || count > 1024 - index) return;
    for (uint32_t i = 0; i < count; ++i) s_stub_uniform_words[is_pixel][index + i] = words[i];
}
void gx2_render_set_uniform_reg(bool is_pixel, uint32_t index, uint32_t src, uint32_t count, uint8_t* mem) {
    if (!src || !count || count > 1024) return;
    std::vector<uint32_t> w(count);
    for (uint32_t i = 0; i < count; ++i) w[i] = rbrew_read32(mem, src + i * 4);
    gx2_render_set_uniform_words(is_pixel, index, w.data(), count);
}
uint32_t gx2_render_uniform_word(bool is_pixel, uint32_t index) { return index < 1024 ? s_stub_uniform_words[is_pixel][index] : 0; }
void gx2_render_set_uniform_block(bool, uint32_t, uint32_t, uint32_t, uint8_t*) {}
static uint32_t s_stub_regs[GX2_LATTE_REGISTER_COUNT];
void gx2_render_apply_registers(const Gx2RegisterWrite* w, size_t n) { for (size_t i = 0; i < n; ++i) s_stub_regs[w[i].index] = w[i].value; }
uint32_t gx2_render_register(uint32_t index) { return index < GX2_LATTE_REGISTER_COUNT ? s_stub_regs[index] : 0; }
void gx2_render_set_sampler_comparison(uint32_t,bool) {}
void gx2_render_set_special_state(uint32_t,bool) {}
void gx2_render_copy_surface(const uint8_t*,uint32_t,uint32_t,const uint8_t*,uint32_t,uint32_t,uint8_t*) {}
void gx2_render_capture_next_draw(const char*) {}
bool gx2_render_replay_capture(const char*, uint8_t*, void (*)(uint32_t, uint32_t), Gx2ReplayResult&) { return false; }
void gx2_render_shutdown() {}
