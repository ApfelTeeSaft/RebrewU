#include "../../runtime/rbrew_diagnostics.h"
// gx2_render_vk_capture.cpp - draw capture (RBREW_CAPTURE_DRAW), frame
// trace control and offline replay for the Vulkan GX2 back-end.
//
// The capture file format (RBCAP001) is the native graphics back-end's, including its
// GPU enum values, so captures recorded by either back-end replay in both.
// Texture levels are stored as the GPU back-end's glGetTexImage results:
// RGBA float (sRGB values undecoded - probe gl_conventions2), one float per
// texel for depth, RGBA 32-bit integers for integer formats.
#include "gx2_render.h"
#include "gx2_pm4_state.h"
#include "gx2_surface.h"
#include "r700_to_glsl.h"
#include "gx2_shader_program.h"
#include "gx2_attrib.h"
#include "vk/gx2_render_vk_state.h"
#include "vk/gx2_vk_shader.h"
#include "../os_common.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>

bool g_gx2_frame_trace_active = false;

using namespace gx2r;

void gx2r_set_xfb_hook(void (*hook)(bool, VkCommandBuffer));
uint32_t gx2r_last_trace_fragments();
void gx2r_reset_trace_fragments();
uint32_t gx2r_next_frame_draw();
void gx2r_frame_draw_index_reset();

namespace {
State& S = state();

struct CaptureMeta {
    uint32_t indexed, prim, count, index_type, index_addr, first_vertex;
    uint32_t vs_addr, ps_addr, fetch_addr, vs_attribs, color_write;
    int32_t viewport[4], scissor[4];
    double depth_range[2];
    uint32_t scissor_on, depth_test, depth_func, depth_mask, cull_on, cull_face, front_face;
    uint32_t blend_on, blend[6];
    uint32_t fbo_width, fbo_height, fbo_internal, fbo_format, fbo_type, has_depth;
    uint32_t vs_from_regs, ps_from_regs;
};
struct CaptureTexture { uint32_t slot, width, height, internal, format, type; int32_t parameters[10]; };
constexpr uint32_t capture_tag(const char (&s)[5]) {
    return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 | uint32_t(uint8_t(s[3])) << 24;
}
constexpr char kCaptureMagic[8] = {'R', 'B', 'C', 'A', 'P', '0', '0', '1'};
std::vector<char> s_capture_path;
unsigned s_capture_frame = 0;
unsigned s_glitch_trace_frames = 0;
bool s_captured_tv = false;
unsigned s_trigger_frame = 0;
unsigned s_trigger_first = 0, s_trigger_last = 0;
bool s_trigger_tv = false, s_trigger_tv_captured = false;
bool s_replaying = false;

uint32_t gl_swizzle(uint32_t sel) {
    static const uint32_t map[6] = {wire::RED, wire::GREEN, wire::BLUE, wire::ALPHA, wire::ZERO, wire::ONE};
    return sel < 6 ? map[sel] : wire::ZERO;
}
uint32_t swizzle_sel(int32_t gl_value) {
    switch (uint32_t(gl_value)) {
    case wire::RED: return 0; case wire::GREEN: return 1; case wire::BLUE: return 2; case wire::ALPHA: return 3; case wire::ONE: return 5;
    default: return 4;
    }
}

// ---------------------------------------------------------------------------
// Texture level readback through texelFetch (decodes BC, sRGB stays raw).
// ---------------------------------------------------------------------------
VkDescriptorSetLayout s_read_set = VK_NULL_HANDLE;
VkPipelineLayout s_read_layout = VK_NULL_HANDLE;
std::map<uint32_t, VkPipeline> s_read_pipelines;

VkPipeline read_pipeline(bool volume, uint32_t kind) {   // kind 0 float, 1 uint, 2 sint
    if (!s_read_layout) {
        VkDescriptorSetLayoutBinding bindings[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                                    {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        set.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR; set.bindingCount = 2; set.pBindings = bindings;
        vkCreateDescriptorSetLayout(gx2vk::device(), &set, nullptr, &s_read_set);
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout.setLayoutCount = 1; layout.pSetLayouts = &s_read_set; layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &range;
        vkCreatePipelineLayout(gx2vk::device(), &layout, nullptr, &s_read_layout);
    }
    auto& pipeline = s_read_pipelines[uint32_t(volume) | kind << 1];
    if (pipeline) return pipeline;
    const char* prefix = kind == 1 ? "u" : kind == 2 ? "i" : "";
    std::string source = "#version 450\nlayout(local_size_x=8,local_size_y=8) in;\n";
    source += std::string("layout(set=0,binding=0) uniform ") + prefix + (volume ? "sampler3D" : "sampler2DArray") + " src;\n";
    source += "layout(set=0,binding=1) buffer Out { uvec4 data[]; };\n";
    source += "layout(push_constant) uniform P { int level, width, height, layers; };\n";
    source += "void main(){ ivec3 p=ivec3(gl_GlobalInvocationID); if(p.x>=width||p.y>=height||p.z>=layers)return;\n";
    source += kind == 0 ? " uvec4 v=floatBitsToUint(texelFetch(src,p,level));\n" : " uvec4 v=uvec4(texelFetch(src,p,level));\n";
    source += " data[(p.z*height+p.y)*width+p.x]=v; }\n";
    VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = gx2vk::shader_module(VK_SHADER_STAGE_COMPUTE_BIT, source);
    info.stage.pName = "main";
    info.layout = s_read_layout;
    if (!info.stage.module || vkCreateComputePipelines(gx2vk::device(), gx2vk::pipeline_cache(), 1, &info, nullptr, &pipeline) != VK_SUCCESS)
        pipeline = VK_NULL_HANDLE;
    return pipeline;
}

// Returns RGBA 32-bit words per texel (float bits for float textures).
bool read_texture_level(TexObject& tex, uint32_t level, uint32_t width, uint32_t height, uint32_t layers, std::vector<uint32_t>& out) {
    if (!tex.image || !tex.image->image) return false;
    const bool volume = tex.target == wire::TEXTURE_3D;
    const uint32_t kind = tex.integer ? (tex.view_format == VK_FORMAT_R32G32B32A32_SINT ? 2 : 1) : 0;
    VkPipeline pipeline = read_pipeline(volume, kind);
    if (!pipeline) return false;
    gx2vk::ViewDesc v;
    v.type = volume ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    // GPU returns stored sRGB values undecoded.
    v.format = tex.view_format == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM :
               tex.view_format == VK_FORMAT_BC1_RGBA_SRGB_BLOCK ? VK_FORMAT_BC1_RGBA_UNORM_BLOCK :
               tex.view_format == VK_FORMAT_BC2_SRGB_BLOCK ? VK_FORMAT_BC2_UNORM_BLOCK :
               tex.view_format == VK_FORMAT_BC3_SRGB_BLOCK ? VK_FORMAT_BC3_UNORM_BLOCK : tex.view_format;
    v.base_level = 0; v.levels = std::max(1u, tex.levels);
    v.layers = volume ? 1 : std::max(1u, tex.image->desc.layers);
    if (tex.depth) v.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    // A reinterpreting view needs a mutable image; sRGB images are mutable
    // when they are render targets/textures, BC images use their own format.
    if (v.format != tex.image->desc.format && !(tex.image->desc.flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT)) v.format = tex.image->desc.format;
    VkImageView view = gx2vk::image_view(*tex.image, v);
    if (!view) return false;
    const VkDeviceSize bytes = VkDeviceSize(width) * height * layers * 16;
    auto buffer = gx2vk::create_buffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, gx2vk::MemoryUse::Readback);
    if (!buffer.buffer) return false;
    end_rendering();
    VkCommandBuffer cb = gx2vk::cmd();
    gx2vk::barrier_if_dirty();
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    TexParams nearest; nearest.min_filter = wire::NEAREST_MIPMAP_NEAREST; nearest.mag_filter = wire::NEAREST;
    VkDescriptorImageInfo image{tex_object_sampler(nearest, false, tex.integer), view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo storage{buffer.buffer, 0, bytes};
    VkWriteDescriptorSet writes[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    writes[0].dstBinding = 0; writes[0].descriptorCount = 1; writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[0].pImageInfo = &image;
    writes[1].dstBinding = 1; writes[1].descriptorCount = 1; writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[1].pBufferInfo = &storage;
    vkCmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s_read_layout, 0, 2, writes);
    const int32_t constants[4] = {int32_t(level), int32_t(width), int32_t(height), int32_t(layers)};
    vkCmdPushConstants(cb, s_read_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, constants);
    vkCmdDispatch(cb, (width + 7) / 8, (height + 7) / 8, layers);
    VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    host.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; host.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT; host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1; dependency.pMemoryBarriers = &host;
    vkCmdPipelineBarrier2(cb, &dependency);
    gx2vk::note_gpu_write();
    const bool ok = gx2vk::finish();
    if (ok) {
        out.resize(size_t(bytes / 4));
        std::memcpy(out.data(), buffer.memory.mapped, bytes);
    }
    gx2vk::destroy_buffer(buffer);
    return ok;
}

// Depth attachment values as GL_DEPTH_COMPONENT floats.
bool read_depth(DepthTarget& d, std::vector<float>& values) {
    std::vector<uint8_t> raw;
    if (!gx2vk::read_image(d.image, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, d.width, d.height, raw)) return false;
    values.resize(size_t(d.width) * d.height);
    for (size_t i = 0; i < values.size(); ++i) {
        if (d.format == VK_FORMAT_D16_UNORM) { uint16_t v; std::memcpy(&v, raw.data() + i * 2, 2); values[i] = v / 65535.0f; }
        else if (d.format == VK_FORMAT_D24_UNORM_S8_UINT) { uint32_t v; std::memcpy(&v, raw.data() + i * 4, 4); values[i] = float(v & 0xFFFFFF) / 16777215.0f; }
        else std::memcpy(&values[i], raw.data() + i * 4, 4);
    }
    return true;
}
}  // namespace

void gx2r_shutdown_capture_state() {
    for (auto& [key, pipeline] : s_read_pipelines) if (pipeline) vkDestroyPipeline(gx2vk::device(), pipeline, nullptr);
    s_read_pipelines.clear();
    if (s_read_layout) vkDestroyPipelineLayout(gx2vk::device(), s_read_layout, nullptr);
    if (s_read_set) vkDestroyDescriptorSetLayout(gx2vk::device(), s_read_set, nullptr);
    s_read_layout = VK_NULL_HANDLE; s_read_set = VK_NULL_HANDLE;
}

void gx2_render_capture_next_draw(const char* path) { s_capture_path.assign(path, path + std::strlen(path) + 1); }
// Called when the present signature detects a flicker; traces the following
// frames so the flicker frame's draws can be compared against normal frames.
void gx2r_trace_glitch_frames(unsigned frames) {
    static unsigned arms = 0;
    if (s_glitch_trace_frames || arms >= 6) return;
    ++arms; s_glitch_trace_frames = frames;
    RBREW_LOGF(stderr, "[ftrace] ---- arming %u glitch-trace frames (#%u)\n", frames, arms);
}
void gx2r_capture_frame_copy() {
    ++s_capture_frame;
    gx2r_frame_draw_index_reset();
    shadow_log_frame_end();
#ifndef RBREW_DISABLE_LOGGING
    // Optional diagnostic trigger: the file contains a draw index or inclusive
    // first:last range. "tv" captures one full-screen TV composite, "*"
    // explicitly requests every draw; an empty file captures draw zero.
    // No guest state or render state is changed.
    static const std::string trigger = [] {
        const char* p=rbrew_diagnostic_env("RBREW_CAPTURE_TRIGGER");
        RBREW_LOGF(stderr,"[capture] trigger path='%s'\n",p?p:"(unset)");
        return std::string(p?p:"");
    }();
    if (!trigger.empty()) {
        std::error_code error;
        if (FILE* file=std::fopen(trigger.c_str(),"r")) {
            char request[64]{};std::fgets(request,sizeof(request),file);std::fclose(file);
            unsigned first=0,last=0;
            bool valid=true;
            const bool trace_mode=std::strncmp(request,"trace",5)==0;
            const bool screen_mode=std::string_view(request)=="screen" || std::string_view(request)=="screen\n" || std::string_view(request)=="screen\r\n";
            const bool tv_mode=std::string_view(request)=="tv" || std::string_view(request)=="tv\n" || std::string_view(request)=="tv\r\n";
            if(trace_mode || tv_mode || screen_mode) { }
            else if(request[0]=='*') last=UINT32_MAX;
            else if(request[0]) {
                char* end=nullptr;first=unsigned(std::strtoul(request,&end,10));last=first;
                if(end && *end==':')last=unsigned(std::strtoul(end+1,&end,10));
                valid=end && (*end=='\0' || *end=='\n' || *end=='\r') && first<=last;
            }
            if(std::filesystem::remove(trigger,error) && valid) {
                if(screen_mode) {
                    s_trigger_frame=0;
                    const char* configured=rbrew_diagnostic_env("RBREW_CAPTURE_DIR");
                    const std::filesystem::path directory=configured?configured:"work/logs/capture";
                    std::filesystem::create_directories(directory,error);
                    const std::string stem="screen_f"+std::to_string(s_capture_frame);
                    auto path=directory/(stem+".ppm");
                    for(unsigned suffix=1;std::filesystem::exists(path,error);++suffix)
                        path=directory/(stem+"_"+std::to_string(suffix)+".ppm");
                    capture_next_present(path.string());
                    RBREW_LOGF(stderr,"[capture] trigger armed presented TV image (no draw trace)\n");
                } else if(trace_mode) {
                    s_trigger_frame=0;
                    s_glitch_trace_frames=1;
                    RBREW_LOGF(stderr,"[capture] trigger armed trace for one frame\n");
                } else {
                    s_trigger_frame=s_capture_frame;s_trigger_first=first;s_trigger_last=last;
                    s_trigger_tv=tv_mode;s_trigger_tv_captured=false;
                    if(tv_mode)RBREW_LOGF(stderr,"[capture] trigger armed frame %u TV composite\n",s_trigger_frame);
                    else RBREW_LOGF(stderr,"[capture] trigger armed frame %u draws %u:%u\n",s_trigger_frame,first,last);
                }
            } else if(!valid)RBREW_LOGF(stderr,"[capture] invalid trigger draw range\n");
        } else {
            static bool reported=false;
            if(!reported && std::filesystem::exists(trigger)){reported=true;RBREW_LOGF(stderr,"[capture] trigger fopen failed path='%s'\n",trigger.c_str());}
        }
    }
#endif
    if(s_trigger_frame) g_gx2_frame_trace_active=s_capture_frame==s_trigger_frame;
}

void gx2r_frame_trace_scan(uint32_t cb_addr, uint32_t scan_target) {
    // RBREW_FRAME_TRACE=<n>: trace the draws between TV copy n and n+1.
    static const unsigned s_trace_copy = [] {
        const char* v = rbrew_diagnostic_env("RBREW_FRAME_TRACE");
        if (!v) return 0u;
        const unsigned n = unsigned(std::strtoul(v, nullptr, 10));
        return n > 1 ? n : 300u;
    }();
    if (scan_target == 1 || scan_target == 0) {
        static unsigned s_tv_copies;
        ++s_tv_copies;
        if (!s_trigger_frame) g_gx2_frame_trace_active = false;
        if (s_trace_copy) {
            g_gx2_frame_trace_active = s_tv_copies == s_trace_copy;
            if (s_tv_copies == s_trace_copy || s_tv_copies == s_trace_copy + 1) RBREW_LOGF(stderr, "[ftrace] ---- TV copy %u cb=%08X\n", s_tv_copies, cb_addr);
        }
        if (s_glitch_trace_frames) {
            --s_glitch_trace_frames;
            g_gx2_frame_trace_active = true;
            RBREW_LOGF(stderr, "[ftrace] ---- glitch trace TV copy %u (left=%u) cb=%08X\n", s_tv_copies, s_glitch_trace_frames, cb_addr);
        }
    }
}

bool gx2r_capture_draw_if_armed(bool indexed, uint32_t prim, uint32_t count, uint32_t index_type, uint32_t index_addr,
                                uint32_t first_vertex, uint8_t* mem) {
    const unsigned draw = gx2r_next_frame_draw();
    // Replay owns its trace queries and must not recursively capture draws
    // or have its trace flag overwritten by a live capture trigger.
    if(s_replaying)return false;
#ifdef RBREW_DISABLE_LOGGING
    (void)indexed;(void)prim;(void)count;(void)index_type;(void)index_addr;(void)first_vertex;(void)mem;(void)draw;
    return false;
#else
    if(s_trigger_frame)g_gx2_frame_trace_active=s_capture_frame==s_trigger_frame;
    struct Request { unsigned frame = 0, draw = 0; bool all = false, tv = false; };
    static const Request request = [] {
        Request r;
        if (const char* v = rbrew_diagnostic_env("RBREW_CAPTURE_DRAW")) {
            char* end = nullptr;
            r.frame = unsigned(std::strtoul(v, &end, 10));
            if (end && *end == ':') {
                if (end[1] == '*') r.all = true;
                else if (std::string_view(end + 1) == "tv") r.tv = true;
                else r.draw = unsigned(std::strtoul(end + 1, nullptr, 10));
            }
        }
        return r;
    }();
    std::string path;
    const auto* tv_target=S.color_targets[0];
    const bool full_tv_draw=count==3 && (prim&31u)==4u && (S.target_masks&15u)==15u && tv_target && tv_target->id==S.scan_tv_source_id &&
        !S.raster.blend && !S.raster.depth_test && S.raster.viewport[0]==0 && S.raster.viewport[1]==0 &&
        S.raster.viewport[2]==int32_t(tv_target->width) && S.raster.viewport[3]==int32_t(tv_target->height) &&
        (!S.raster.scissor_test || (S.raster.scissor[0]==0 && S.raster.scissor[1]==0 &&
            S.raster.scissor[2]>=int32_t(tv_target->width) && S.raster.scissor[3]>=int32_t(tv_target->height)));
    const bool tv_composite=request.tv && !s_captured_tv && full_tv_draw;
    const bool triggered=s_trigger_frame && s_capture_frame==s_trigger_frame &&
        (s_trigger_tv ? !s_trigger_tv_captured && full_tv_draw : draw>=s_trigger_first && draw<=s_trigger_last);
    if (!s_capture_path.empty()) { path = s_capture_path.data(); s_capture_path.clear(); }
    else if (triggered ||
             (request.frame && s_capture_frame == request.frame && (request.all || tv_composite || (!request.tv && draw == request.draw)))) {
        if (tv_composite) s_captured_tv = true;
        if (triggered && s_trigger_tv) s_trigger_tv_captured=true;
        const char* dir = rbrew_diagnostic_env("RBREW_CAPTURE_DIR");
        std::string directory = dir ? dir : "work/logs/capture";
        std::error_code ignored; std::filesystem::create_directories(directory, ignored);
        char name[64]; std::snprintf(name, sizeof(name), "/f%u_d%03u.rbcap", s_capture_frame, draw);
        path = directory + name;
    }
    if (path.empty()) return false;
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { RBREW_LOGF(stderr, "[capture] cannot write %s\n", path.c_str()); return false; }
    std::fwrite(kCaptureMagic, 1, sizeof(kCaptureMagic), f);
    auto section = [&](uint32_t tag, const void* data, uint32_t size) { std::fwrite(&tag, 4, 1, f); std::fwrite(&size, 4, 1, f); if (size) std::fwrite(data, 1, size, f); };
    unsigned regions = 0;
    auto region = [&](uint32_t addr, uint64_t size) {
        if (!addr || !size || size > (64u << 20) || !rbrew_memory_accessible(mem, addr, size)) return;
        const uint32_t tag = capture_tag("MEM "), bytes = uint32_t(size) + 4;
        std::fwrite(&tag, 4, 1, f); std::fwrite(&bytes, 4, 1, f); std::fwrite(&addr, 4, 1, f);
        std::fwrite(mem + addr, 1, size_t(size), f); ++regions;
    };
    const auto& g = S.raster;
    CaptureMeta m{};
    m.indexed = indexed; m.prim = prim; m.count = count; m.index_type = index_type; m.index_addr = index_addr;
    m.first_vertex = first_vertex; m.vs_addr = S.cur_vs; m.ps_addr = S.cur_ps;
    m.fetch_addr = S.active_fetch; m.vs_attribs = S.cur_vs_attribs; m.color_write = S.color_write;
    m.vs_from_regs = S.vs_from_regs; m.ps_from_regs = S.ps_from_regs;
    std::memcpy(m.viewport, g.viewport, 16); std::memcpy(m.scissor, g.scissor, 16);
    m.depth_range[0] = g.depth_range[0]; m.depth_range[1] = g.depth_range[1];
    m.scissor_on = g.scissor_test; m.depth_test = g.depth_test; m.depth_func = g.depth_func; m.depth_mask = g.depth_write;
    m.cull_on = g.cull; m.cull_face = g.cull_face; m.front_face = g.front_face; m.blend_on = g.blend;
    const uint32_t blend[6] = {g.blend_src_rgb, g.blend_dst_rgb, g.blend_src_alpha, g.blend_dst_alpha, g.blend_eq_rgb, g.blend_eq_alpha};
    std::memcpy(m.blend, blend, sizeof(blend));
    m.fbo_width = uint32_t(g.viewport[0] + g.viewport[2]); m.fbo_height = uint32_t(g.viewport[1] + g.viewport[3]);
    m.fbo_internal = wire::RGBA8; m.fbo_format = wire::RGBA; m.fbo_type = wire::UNSIGNED_BYTE;
    if (S.framebuffer.valid && !S.framebuffer.depth_only && S.color_targets[0] && S.current_fbo_id == S.color_targets[0]->id) {
        const auto* e = S.color_targets[0];
        m.fbo_width = e->width; m.fbo_height = e->height;
        m.fbo_internal = e->fmt.wire_internal; m.fbo_format = e->fmt.wire_format; m.fbo_type = e->fmt.wire_type;
    }
    m.has_depth = S.framebuffer.valid && S.framebuffer.depth != nullptr;
    // RBREW_CAPTURE_LITE=1: geometry-only captures (no depth or texture
    // images) so every draw of a frame fits on disk; replay positions match.
    static const bool lite = [] { const char* v = rbrew_diagnostic_env("RBREW_CAPTURE_LITE"); return v && v[0] == '1'; }();
    section(capture_tag("META"), &m, sizeof(m));
    const uint32_t instance_state[] = {S.draw_instances, S.draw_first_instance}; section(capture_tag("INSC"), instance_state, sizeof(instance_state));
    const int32_t clip_state[2] = {int32_t(g.clip_origin), int32_t(wire::NEGATIVE_ONE_TO_ONE)};
    section(capture_tag("CLIP"), clip_state, sizeof(clip_state));
    if (!lite && m.has_depth && S.depth_target) {
        std::vector<float> values;
        if (read_depth(*S.framebuffer.depth, values)) section(capture_tag("FDEP"), values.data(), uint32_t(values.size() * sizeof(float)));
    }
    section(capture_tag("REGS"), S.regs.data(), uint32_t(S.regs.size() * 4));
    section(capture_tag("UNIF"), S.uniform_words, sizeof(S.uniform_words));
    section(capture_tag("MASK"), &S.target_masks, sizeof(S.target_masks));
    section(capture_tag("TEXD"), S.bound_textures.data(), uint32_t(sizeof(S.bound_textures)));
    std::array<bool, 54> valid{};
    for (unsigned s = 0; s < 54; ++s) valid[s] = S.samplers[s].valid;
    section(capture_tag("SAMP"), valid.data(), uint32_t(sizeof(valid)));
    std::vector<Gx2RegisterWrite> raster;
    for (unsigned r : {0xA105u, 0xA106u, 0xA107u, 0xA108u, 0xA351u, 0xA280u, 0xA103u, 0xA2A5u, 0xA205u, 0xA37Fu, 0xA380u, 0xA381u, 0xA382u, 0xA383u})
        raster.push_back({r, S.regs[r]});
    section(capture_tag("RAST"), raster.data(), uint32_t(raster.size() * sizeof(Gx2RegisterWrite)));
    unsigned texture_count = 0;
    for (unsigned slot = 0; slot < S.bound_textures.size(); ++slot) {
        const auto& bound = S.bound_textures[slot];
        if (lite || !bound.address) continue;
        // Capture the same current shadow contents the draw will consume.
        refresh_bound_render_texture(slot, mem);
        const auto dim = descriptor_word(bound.descriptor.data(), GX2SURF_DIM);
        const uint32_t target = dim == 3 ? wire::TEXTURE_CUBE_MAP_ARRAY : dim == 5 ? wire::TEXTURE_2D_ARRAY : dim == 2 ? wire::TEXTURE_3D : wire::TEXTURE_2D;
        const uint32_t target_index = target == wire::TEXTURE_2D_ARRAY ? 1 : target == wire::TEXTURE_3D ? 2 : target == wire::TEXTURE_CUBE_MAP_ARRAY ? 3 : 0;
        TexObject* tex = S.units[slot][target_index];
        if (!tex || !tex->image || !tex->image->image) continue;
        const auto& p = tex->params;
        uint32_t view[7] = {slot, p.base_level, p.max_level};
        std::memcpy(&view[3], &p.min_lod, 4); std::memcpy(&view[4], &p.max_lod, 4); std::memcpy(&view[5], &p.lod_bias, 4);
        view[6] = p.wrap_r;
        section(capture_tag("TVEW"), view, sizeof(view));
        const uint32_t max_level = std::min({13u, p.max_level, tex->levels ? tex->levels - 1 : 0});
        for (uint32_t level = 0; level <= max_level; ++level) {
            const uint32_t w = std::max(1u, tex->image->desc.width >> level), h = std::max(1u, tex->image->desc.height >> level);
            const uint32_t layers = target == wire::TEXTURE_3D ? std::max(1u, tex->image->desc.depth >> level) : std::max(1u, tex->image->desc.layers);
            const bool depth = tex->depth, integer = tex->integer;
            const uint64_t bytes = uint64_t(w) * h * layers * (depth ? 1 : 4) * sizeof(float);
            if (!bytes || bytes > (64u << 20)) continue;
            std::vector<uint32_t> texels;
            if (!read_texture_level(*tex, level, w, h, layers, texels)) continue;
            const Format vf = gx2_vk_format(depth ? 0x80E : 0x1A);
            uint32_t internal = wire::RGBA8;
            for (const auto& [key, entry] : S.tex_map) if (&entry.tex == tex) internal = gx2_vk_format(entry.format).wire_internal;
            for (const auto& [key, cb] : S.cb_map) if (&cb.tex == tex) internal = cb.fmt.wire_internal;
            (void)vf;
            CaptureTexture t{slot, w, h, depth ? wire::DEPTH_COMPONENT32F : internal, integer ? wire::RGBA_INTEGER : depth ? wire::DEPTH_COMPONENT : wire::RGBA,
                             integer ? (tex->view_format == VK_FORMAT_R32G32B32A32_SINT ? wire::INT : wire::UNSIGNED_INT) : wire::FLOAT, {}};
            const int32_t params[10] = {int32_t(p.min_filter), int32_t(p.mag_filter), int32_t(p.wrap_s), int32_t(p.wrap_t), int32_t(p.compare_mode ? 0x884E : 0),
                                        int32_t(p.compare_func), int32_t(gl_swizzle((tex->swizzle >> 16) & 7)), int32_t(gl_swizzle((tex->swizzle >> 19) & 7)),
                                        int32_t(gl_swizzle((tex->swizzle >> 22) & 7)), int32_t(gl_swizzle((tex->swizzle >> 25) & 7))};
            std::memcpy(t.parameters, params, sizeof(params));
            std::vector<uint8_t> payload(sizeof(t) + size_t(bytes));
            std::memcpy(payload.data(), &t, sizeof(t));
            if (depth) for (size_t i = 0; i < size_t(w) * h * layers; ++i) std::memcpy(payload.data() + sizeof(t) + i * 4, &texels[i * 4], 4);
            else std::memcpy(payload.data() + sizeof(t), texels.data(), size_t(bytes));
            const uint32_t dimensions[] = {slot, target, layers};
            section(capture_tag("TDIM"), dimensions, sizeof(dimensions));
            const uint32_t mip[] = {slot, level}; section(capture_tag("TLVL"), mip, sizeof(mip));
            section(capture_tag("TIMG"), payload.data(), uint32_t(payload.size())); ++texture_count;
        }
        const auto* d = bound.descriptor.data();
        region(descriptor_word(d, GX2SURF_IMAGE), descriptor_word(d, GX2SURF_IMAGE_SIZE));
        region(descriptor_word(d, GX2SURF_MIPMAPS), descriptor_word(d, GX2SURF_MIPMAP_SIZE));
    }
    uint32_t blocks[2][16][2]{}, buffers[32][3]{};
    for (unsigned s = 0; s < 2; ++s) for (unsigned b = 0; b < 16; ++b) blocks[s][b][0] = S.uniform_blocks[s][b].address, blocks[s][b][1] = S.uniform_blocks[s][b].size;
    for (unsigned i = 0; i < 32; ++i) buffers[i][0] = S.attrib[i].guest_addr, buffers[i][1] = S.attrib[i].size, buffers[i][2] = S.attrib[i].stride;
    section(capture_tag("UBLK"), blocks, sizeof(blocks));
    section(capture_tag("ATTB"), buffers, sizeof(buffers));
    if (auto it = S.fetch_shaders.find(S.active_fetch); it != S.fetch_shaders.end())
        section(capture_tag("FTCH"), it->second.attributes.data(), uint32_t(it->second.attributes.size() * sizeof(R700FetchAttribute)));
    region(S.cur_vs, 0x134); region(S.cur_ps, 0xE8);
    for (bool pixel : {false, true}) {
        GX2ShaderProgram p{};
        if (pixel ? S.ps_from_regs : S.vs_from_regs) {
            const uint32_t start = pixel ? 0xA210 : 0xA216;
            p.address = S.regs[start] << 8; p.size = S.regs[start + 1] << 3;
        } else if (const uint32_t d = pixel ? S.cur_ps : S.cur_vs) p = gx2_shader_program({mem, WIIU_MEM_SIZE}, d, pixel);
        if (p) region(p.address, p.size);
    }
    for (unsigned s = 0; s < 2; ++s) for (unsigned b = 0; b < 16; ++b) region(blocks[s][b][0], blocks[s][b][1]);
    for (unsigned i = 0; i < 32; ++i) region(buffers[i][0], buffers[i][1]);
    if (indexed) region(index_addr, uint64_t(count) * ((index_type == GX2_INDEX_TYPE_U32_BE || index_type == GX2_INDEX_TYPE_U32_LE) ? 4 : 2));
    std::fclose(f);
    RBREW_LOGF(stderr, "[capture] frame %u draw %u -> %s (%u regions, %u textures, fbo %ux%u)\n", s_capture_frame, draw, path.c_str(), regions,
                 texture_count, m.fbo_width, m.fbo_height);
    return true;
#endif
}

// ---------------------------------------------------------------------------
// Offline replay
// ---------------------------------------------------------------------------
namespace {
VkFormat replay_color_format(uint32_t internal) {
    switch (internal) {
    case 0x8C43: return VK_FORMAT_R8G8B8A8_SRGB;
    case 0x881A: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 0x8814: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 0x8230: return VK_FORMAT_R32G32_SFLOAT;
    case 0x8C3A: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case 0x8059: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 0x822E: return VK_FORMAT_R32_SFLOAT;
    case 0x822D: return VK_FORMAT_R16_SFLOAT;
    case 0x822F: return VK_FORMAT_R16G16_SFLOAT;
    case 0x8229: return VK_FORMAT_R8_UNORM;
    case 0x822A: return VK_FORMAT_R16_UNORM;
    case 0x822B: return VK_FORMAT_R8G8_UNORM;
    case 0x8D70: return VK_FORMAT_R32G32B32A32_UINT;
    case 0x8D76: return VK_FORMAT_R16G16B16A16_UINT;
    case 0x8D82: return VK_FORMAT_R32G32B32A32_SINT;
    default: return VK_FORMAT_R8G8B8A8_UNORM;
    }
}
// Texture image storage for a replayed TIMG level (GPU uploads the float data
// into the original internal format; Vulkan cannot encode BC, so decoded BC
// data is stored as 8-bit UNORM/SNORM).
VkFormat replay_texture_format(uint32_t internal, uint32_t format, uint32_t type) {
    if (format == wire::DEPTH_COMPONENT) return VK_FORMAT_D32_SFLOAT;
    if (format == wire::RGBA_INTEGER) return type == wire::INT ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_UINT;
    switch (internal) {
    case 0x8C43: case 0x8C4D: case 0x8C4E: case 0x8C4F: return VK_FORMAT_R8G8B8A8_SRGB;
    case 0x8DBC: case 0x8DBE: return VK_FORMAT_R8G8B8A8_SNORM;
    case 0x8058: case 0x8229: case 0x822B: case wire::RGB: case 0x83F1: case 0x83F2: case 0x83F3: case 0x8DBB: case 0x8DBD: return VK_FORMAT_R8G8B8A8_UNORM;
    default: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}
uint8_t unorm8(float v) { return uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }
uint8_t snorm8(float v) { return uint8_t(int8_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 127.0f))); }
float half_to_float(uint16_t h) {
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? (m ? NAN : INFINITY) : std::ldexp(float(m | 1024), int(e) - 25);
    return s ? -v : v;
}
float uf11(uint32_t bits, unsigned mbits) {
    const uint32_t e = bits >> mbits, m = bits & ((1u << mbits) - 1);
    if (e == 0) return std::ldexp(float(m), -14 - int(mbits));
    if (e == 31) return m ? NAN : INFINITY;
    return std::ldexp(float(m | (1u << mbits)), int(e) - 15 - int(mbits));
}
// glReadPixels(RGBA, UNSIGNED_BYTE) of a color image.
bool read_rgba8(ColorTarget& target, std::vector<uint8_t>& out) {
    std::vector<uint8_t> raw;
    if (!gx2vk::read_image(target.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, target.width, target.height, raw)) return false;
    const size_t n = size_t(target.width) * target.height;
    out.assign(n * 4, 0);
    const VkFormat f = target.image.desc.format;
    for (size_t i = 0; i < n; ++i) {
        uint8_t* o = out.data() + i * 4;
        float c[4] = {0, 0, 0, 1};
        switch (f) {
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: std::memcpy(o, raw.data() + i * 4, 4); continue;
        case VK_FORMAT_R8_UNORM: o[0] = raw[i]; o[3] = 255; continue;
        case VK_FORMAT_R8G8_UNORM: o[0] = raw[i * 2]; o[1] = raw[i * 2 + 1]; o[3] = 255; continue;
        case VK_FORMAT_R16_UNORM: {uint16_t v;std::memcpy(&v,raw.data()+i*2,2);c[0]=v/65535.0f;} break;
        case VK_FORMAT_R16G16B16A16_SFLOAT: for (unsigned k = 0; k < 4; ++k) { uint16_t h; std::memcpy(&h, raw.data() + i * 8 + k * 2, 2); c[k] = half_to_float(h); } break;
        case VK_FORMAT_R16_SFLOAT: { uint16_t h; std::memcpy(&h, raw.data() + i * 2, 2); c[0] = half_to_float(h); } break;
        case VK_FORMAT_R16G16_SFLOAT: for (unsigned k = 0; k < 2; ++k) { uint16_t h; std::memcpy(&h, raw.data() + i * 4 + k * 2, 2); c[k] = half_to_float(h); } break;
        case VK_FORMAT_R32G32B32A32_SFLOAT: std::memcpy(c, raw.data() + i * 16, 16); break;
        case VK_FORMAT_R32_SFLOAT: std::memcpy(c, raw.data() + i * 4, 4); break;
        case VK_FORMAT_R32G32_SFLOAT: std::memcpy(c, raw.data() + i * 8, 8); break;
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32: { uint32_t v; std::memcpy(&v, raw.data() + i * 4, 4); c[0] = uf11(v & 0x7FF, 6); c[1] = uf11((v >> 11) & 0x7FF, 6); c[2] = uf11(v >> 22, 5); } break;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: { uint32_t v; std::memcpy(&v, raw.data() + i * 4, 4); c[0] = (v & 1023) / 1023.0f; c[1] = ((v >> 10) & 1023) / 1023.0f; c[2] = ((v >> 20) & 1023) / 1023.0f; c[3] = (v >> 30) / 3.0f; } break;
        case VK_FORMAT_R5G6B5_UNORM_PACK16: { uint16_t v; std::memcpy(&v, raw.data() + i * 2, 2); c[0] = (v >> 11) / 31.0f; c[1] = ((v >> 5) & 63) / 63.0f; c[2] = (v & 31) / 31.0f; } break;
        default: continue;   // integer targets: GL_INVALID_OPERATION leaves zeros
        }
        for (unsigned k = 0; k < 4; ++k) o[k] = unorm8(c[k]);
    }
    return true;
}
uint32_t s_xfb_offset = 0, s_xfb_step = 0, s_xfb_capacity = 0;
VkBuffer s_xfb_buffer = VK_NULL_HANDLE;
}  // namespace

bool gx2_render_replay_capture(const char* path, uint8_t* mem, void (*commit)(uint32_t, uint32_t), Gx2ReplayResult& out) {
    if(s_replaying)return false;
    struct ReplayScope {
        ReplayScope(){s_replaying=true;}
        ~ReplayScope(){s_replaying=false;}
    } replay_scope;
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::vector<uint8_t> file;
    { uint8_t chunk[65536]; size_t n; while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) file.insert(file.end(), chunk, chunk + n); }
    std::fclose(f);
    if (file.size() < 8 || std::memcmp(file.data(), kCaptureMagic, 8) != 0 || !gx2vk::ready()) return false;
    CaptureMeta m{};
    bool have_meta = false;
    std::vector<R700FetchAttribute> attributes;
    struct LoadedTexture { std::vector<uint8_t> bytes; uint32_t target, layers, level; };
    std::vector<LoadedTexture> textures;
    std::vector<Gx2RegisterWrite> raster;
    std::array<uint32_t, 96> texture_levels{};
    std::array<std::array<uint32_t, 6>, 96> texture_views{}; std::array<bool, 96> have_texture_view{};
    std::vector<float> framebuffer_depth;
    std::array<uint32_t, 96> texture_targets{}, texture_layers{};
    texture_targets.fill(wire::TEXTURE_2D); texture_layers.fill(1);
    uint32_t target_masks = 0xFFFFFFFF;
    uint32_t instance_state[] = {1, 0};
    std::array<bool, 54> sampler_valid{};
    for (auto& s : S.samplers) s.valid = false;
    S.sampler_compare.fill(false); S.bound_textures.fill({});
    int32_t clip_state[2] = {int32_t(wire::LOWER_LEFT), int32_t(wire::NEGATIVE_ONE_TO_ONE)};
    uint32_t blocks[2][16][2]{}, buffers[32][3]{};
    for (size_t at = 8; at + 8 <= file.size();) {
        uint32_t tag, size; std::memcpy(&tag, &file[at], 4); std::memcpy(&size, &file[at + 4], 4);
        const uint8_t* data = &file[at + 8];
        if (at + 8 + uint64_t(size) > file.size()) return false;
        at += 8 + size_t(size);
        if (tag == capture_tag("META") && size == sizeof(m)) { std::memcpy(&m, data, size); have_meta = true; }
        else if (tag == capture_tag("INSC") && size == sizeof(instance_state)) std::memcpy(instance_state, data, size);
        else if (tag == capture_tag("RAST") && size % sizeof(Gx2RegisterWrite) == 0) { raster.resize(size / sizeof(Gx2RegisterWrite)); std::memcpy(raster.data(), data, size); }
        else if (tag == capture_tag("REGS") && size == S.regs.size() * 4) std::memcpy(S.regs.data(), data, size);
        else if (tag == capture_tag("UNIF") && size == sizeof(S.uniform_words)) std::memcpy(S.uniform_words, data, size);
        else if (tag == capture_tag("UBLK") && size == sizeof(blocks)) std::memcpy(blocks, data, size);
        else if (tag == capture_tag("ATTB") && size == sizeof(buffers)) std::memcpy(buffers, data, size);
        else if (tag == capture_tag("MASK") && size == 4) std::memcpy(&target_masks, data, 4);
        else if (tag == capture_tag("CLIP") && size == sizeof(clip_state)) std::memcpy(clip_state, data, size);
        else if (tag == capture_tag("SAMP") && size == sizeof(sampler_valid)) std::memcpy(sampler_valid.data(), data, size);
        else if (tag == capture_tag("TEXD") && (size == sizeof(S.bound_textures) || size == 32 * sizeof(BoundTexture))) std::memcpy(S.bound_textures.data(), data, size);
        else if (tag == capture_tag("TIMG") && size >= sizeof(CaptureTexture)) {
            uint32_t slot; std::memcpy(&slot, data, 4); if (slot >= 96) return false;
            textures.push_back({std::vector<uint8_t>(data, data + size), texture_targets[slot], texture_layers[slot], texture_levels[slot]});
        } else if (tag == capture_tag("TLVL") && size == 8) { uint32_t d[2]; std::memcpy(d, data, 8); if (d[0] >= 96 || d[1] > 13) return false; texture_levels[d[0]] = d[1]; }
        else if (tag == capture_tag("TVEW") && size == 28) { uint32_t slot; std::memcpy(&slot, data, 4); if (slot >= 96) return false; std::memcpy(texture_views[slot].data(), data + 4, 24); have_texture_view[slot] = true; }
        else if (tag == capture_tag("FDEP") && size % 4 == 0) { framebuffer_depth.resize(size / 4); std::memcpy(framebuffer_depth.data(), data, size); }
        else if (tag == capture_tag("TDIM") && size == 12) { uint32_t d[3]; std::memcpy(d, data, 12); if (d[0] >= 96 || !d[2]) return false; texture_targets[d[0]] = d[1]; texture_layers[d[0]] = d[2]; }
        else if (tag == capture_tag("FTCH") && size % sizeof(R700FetchAttribute) == 0) { attributes.resize(size / sizeof(R700FetchAttribute)); std::memcpy(attributes.data(), data, size); }
        else if (tag == capture_tag("MEM ") && size >= 4) {
            uint32_t addr; std::memcpy(&addr, data, 4);
            if (commit) commit(addr, size - 4);
            if (!rbrew_memory_accessible(mem, addr, size - 4)) return false;
            std::memcpy(mem + addr, data + 4, size - 4);
        }
    }
    if (!have_meta || !m.fbo_width || !m.fbo_height) return false;
    // GPU: valid guest samplers become sampler objects; other units use texture parameters.
    for (unsigned s = 0; s < 54; ++s) S.samplers[s].valid = false;
    {
        std::vector<Gx2RegisterWrite> sampler_writes;
        for (unsigned s = 0; s < 54; ++s) if (sampler_valid[s]) for (unsigned w = 0; w < 3; ++w) sampler_writes.push_back({0xF000 + s * 3 + w, S.regs[0xF000 + s * 3 + w]});
        // Re-apply the captured words through the normal path (marks valid, builds parameters).
        const auto saved_raster_regs = S.regs;
        if (!sampler_writes.empty()) gx2_render_apply_registers(sampler_writes.data(), sampler_writes.size());
        S.regs = saved_raster_regs;
    }
    for (unsigned s = 0; s < 2; ++s) for (unsigned b = 0; b < 16; ++b) S.uniform_blocks[s][b].address = blocks[s][b][0], S.uniform_blocks[s][b].size = blocks[s][b][1];
    for (unsigned i = 0; i < 32; ++i) S.attrib[i].guest_addr = buffers[i][0], S.attrib[i].size = buffers[i][1], S.attrib[i].stride = buffers[i][2];
    S.fetch_shaders[m.fetch_addr].attributes = attributes;
    S.active_fetch = m.fetch_addr;
    S.cur_vs = m.vs_addr; S.cur_ps = m.ps_addr; S.cur_vs_attribs = m.vs_attribs;
    S.vs_from_regs = m.vs_from_regs != 0; S.ps_from_regs = m.ps_from_regs != 0;

    // Offscreen target of the captured size and format.
    ColorTarget color;
    color.width = m.fbo_width; color.height = m.fbo_height;
    color.gx2_format = 0;
    color.fmt.format = color.fmt.attachment = replay_color_format(m.fbo_internal);
    if (color.fmt.format == VK_FORMAT_R8G8B8A8_SRGB) color.fmt.attachment = VK_FORMAT_R8G8B8A8_UNORM;
    color.fmt.integer = gx2vk::format_is_integer(color.fmt.format);
    color.id = S.next_object_id++;
    {
        gx2vk::ImageDesc desc;
        desc.format = color.fmt.format; desc.width = color.width; desc.height = color.height;
        desc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (color.fmt.format != color.fmt.attachment) desc.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        if (!gx2vk::create_image(color.image, desc)) return false;
    }
    DepthTarget depth;
    if (m.has_depth) {
        depth.width = m.fbo_width; depth.height = m.fbo_height; depth.format = VK_FORMAT_D32_SFLOAT; depth.id = S.next_object_id++;
        gx2vk::ImageDesc desc;
        desc.format = depth.format; desc.width = depth.width; desc.height = depth.height;
        desc.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!gx2vk::create_image(depth.image, desc)) { gx2vk::destroy_image(color.image); return false; }
    }
    // Replayed textures (TIMG levels).
    struct ReplayTexture { gx2vk::Image image; TexObject tex; uint32_t levels = 0; };
    std::map<uint32_t, ReplayTexture> replay;
    for (const auto& image : textures) {
        CaptureTexture t{}; std::memcpy(&t, image.bytes.data(), sizeof(t));
        const uint64_t expected = uint64_t(t.width) * t.height * image.layers * (t.format == wire::DEPTH_COMPONENT ? 1 : 4) * 4;
        if (t.slot >= 96 || (t.type != wire::FLOAT && t.type != wire::UNSIGNED_INT && t.type != wire::INT) || expected != image.bytes.size() - sizeof(t)) return false;
        auto& r = replay[t.slot];
        r.levels = std::max(r.levels, image.level + 1);
    }
    for (const auto& image : textures) {
        CaptureTexture t{}; std::memcpy(&t, image.bytes.data(), sizeof(t));
        auto& r = replay[t.slot];
        const VkFormat format = replay_texture_format(t.internal, t.format, t.type);
        if (!r.image.image) {
            gx2vk::ImageDesc desc;
            desc.type = image.target == wire::TEXTURE_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
            desc.format = format;
            const uint32_t base_w = t.width << image.level, base_h = t.height << image.level;
            desc.width = base_w; desc.height = base_h;
            desc.depth = image.target == wire::TEXTURE_3D ? image.layers << image.level : 1;
            desc.layers = image.target == wire::TEXTURE_3D ? 1 : image.layers;
            desc.levels = r.levels;
            if (image.target == wire::TEXTURE_CUBE_MAP_ARRAY) desc.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
            desc.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            if (!gx2vk::create_image(r.image, desc)) continue;
            r.tex.image = &r.image;
            r.tex.target = image.target;
            r.tex.view_format = format;
            r.tex.levels = r.levels; r.tex.layers = image.layers;
            r.tex.integer = t.format == wire::RGBA_INTEGER; r.tex.depth = t.format == wire::DEPTH_COMPONENT;
            r.tex.params.min_filter = uint32_t(t.parameters[0]); r.tex.params.mag_filter = uint32_t(t.parameters[1]);
            r.tex.params.wrap_s = uint32_t(t.parameters[2]); r.tex.params.wrap_t = uint32_t(t.parameters[3]);
            r.tex.params.compare_mode = t.parameters[4] ? 1 : 0; r.tex.params.compare_func = uint32_t(t.parameters[5]);
            r.tex.swizzle = swizzle_sel(t.parameters[6]) << 16 | swizzle_sel(t.parameters[7]) << 19 | swizzle_sel(t.parameters[8]) << 22 | swizzle_sel(t.parameters[9]) << 25;
            if (image.target != wire::TEXTURE_2D) r.tex.params.wrap_r = wire::CLAMP_TO_EDGE;
            r.tex.params.max_level = r.levels - 1;
            r.tex.id = S.next_object_id++;
        }
        const uint8_t* payload = image.bytes.data() + sizeof(t);
        const size_t texels = size_t(t.width) * t.height * image.layers;
        std::vector<uint8_t> converted;
        if (format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB || format == VK_FORMAT_R8G8B8A8_SNORM) {
            converted.resize(texels * 4);
            for (size_t i = 0; i < texels * 4; ++i) { float v; std::memcpy(&v, payload + i * 4, 4); converted[i] = format == VK_FORMAT_R8G8B8A8_SNORM ? snorm8(v) : unorm8(v); }
        } else {
            converted.assign(payload, payload + texels * (format == VK_FORMAT_D32_SFLOAT ? 4 : 16));
        }
        upload_image_level(r.image, image.level, 0, image.layers, t.width, t.height, image.layers, converted.data(), converted.size(),
                           format == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT);
    }
    const auto previous_units = S.units;
    const auto previous_bindings = S.bound_textures;
    for (auto& [slot, r] : replay) {
        if (!r.image.image) continue;
        if (have_texture_view[slot]) {
            const auto& v = texture_views[slot];
            r.tex.params.base_level = v[0]; r.tex.params.max_level = v[1];
            std::memcpy(&r.tex.params.min_lod, &v[2], 4); std::memcpy(&r.tex.params.max_lod, &v[3], 4); std::memcpy(&r.tex.params.lod_bias, &v[4], 4);
            if (r.tex.target != wire::TEXTURE_2D) r.tex.params.wrap_r = v[5];
        }
        const uint32_t index = r.tex.target == wire::TEXTURE_2D_ARRAY ? 1 : r.tex.target == wire::TEXTURE_3D ? 2 : r.tex.target == wire::TEXTURE_CUBE_MAP_ARRAY ? 3 : 0;
        S.units[slot][index] = &r.tex;
        // Captured GPU bytes own this replay resource. An address from TEXD
        // describes its original guest storage, not a live alias to refresh
        // from the current process (which may already have cleared that image).
        S.bound_textures[slot].address = 0;
    }
    const auto previous_framebuffer = S.framebuffer;
    const auto previous_fbo = S.current_fbo_id;
    const auto previous_masks = S.target_masks;
    const bool previous_trace = g_gx2_frame_trace_active;
    g_gx2_frame_trace_active = true;   // per-draw diagnostics + fragment count
    auto bind_replay_framebuffer = [&] {
        Framebuffer fb;
        fb.valid = true;
        fb.colors[0] = &color;
        fb.depth = m.has_depth ? &depth : nullptr;
        S.framebuffer = fb;
        S.current_fbo_id = color.id;
    };
    auto pass = [&](bool captured_state) {
        if (!raster.empty()) apply_native_raster_registers(raster.data(), raster.size());
        S.raster.clip_origin = uint32_t(clip_state[0]) == wire::UPPER_LEFT ? wire::UPPER_LEFT : wire::LOWER_LEFT;
        bind_replay_framebuffer();
        std::memcpy(S.raster.viewport, m.viewport, 16);
        S.raster.depth_range[0] = std::clamp(m.depth_range[0], 0.0, 1.0); S.raster.depth_range[1] = std::clamp(m.depth_range[1], 0.0, 1.0);
        S.raster.scissor_test = false;
        S.raster.color_mask.fill(15); S.raster.depth_write = true;
        // glClear(COLOR|DEPTH|STENCIL) with (0,0,0,0), 1.0, 0.
        if (ensure_rendering()) {
            std::vector<VkClearAttachment> clears{{VK_IMAGE_ASPECT_COLOR_BIT, 0, {}}};
            if (m.has_depth) { VkClearAttachment d{VK_IMAGE_ASPECT_DEPTH_BIT, 0, {}}; d.clearValue.depthStencil = {1.0f, 0}; clears.push_back(d); }
            VkClearRect rect{render_area(), 0, 1};
            vkCmdClearAttachments(gx2vk::cmd(), uint32_t(clears.size()), clears.data(), 1, &rect);
            gx2vk::note_gpu_write(color.image);
            if (m.has_depth) gx2vk::note_gpu_write(depth.image);
        }
        if (captured_state) {
            if (m.has_depth && framebuffer_depth.size() == size_t(m.fbo_width) * m.fbo_height)
                upload_image_level(depth.image, 0, 0, 1, m.fbo_width, m.fbo_height, 1, reinterpret_cast<const uint8_t*>(framebuffer_depth.data()),
                                   framebuffer_depth.size() * 4, VK_IMAGE_ASPECT_DEPTH_BIT);
            if (m.scissor_on) { S.raster.scissor_test = true; std::memcpy(S.raster.scissor, m.scissor, 16); }
            S.raster.depth_test = m.depth_test != 0;
            S.raster.depth_func = m.depth_func; S.raster.depth_write = m.depth_mask != 0;
            S.raster.cull = m.cull_on != 0; S.raster.cull_face = m.cull_face; S.raster.front_face = m.front_face;
            S.raster.blend = m.blend_on != 0;
            S.raster.blend_src_rgb = m.blend[0]; S.raster.blend_dst_rgb = m.blend[1]; S.raster.blend_src_alpha = m.blend[2]; S.raster.blend_dst_alpha = m.blend[3];
            S.raster.blend_eq_rgb = m.blend[4]; S.raster.blend_eq_alpha = m.blend[5];
            S.target_masks = target_masks; gx2_render_set_color_write(m.color_write != 0);
        } else {
            S.raster.depth_test = false; S.raster.cull = false; S.raster.blend = false;
            S.target_masks = 0xFFFFFFFF; gx2_render_set_color_write(true);
        }
        bind_replay_framebuffer();
        gx2r_reset_trace_fragments();
        if (m.indexed) gx2_render_draw_indexed(m.prim, m.count, m.index_type, m.index_addr, m.first_vertex, mem, instance_state[0], instance_state[1]);
        else gx2_render_draw(m.prim, m.count, m.first_vertex, mem, instance_state[0], instance_state[1]);
        return gx2r_last_trace_fragments();
    };
    out.fragments_no_depth_cull = pass(false);
    out.fragments = pass(true);
    // Vertex outputs: gl_Position of the assembled vertices via transform
    // feedback of the same program, reported in the GPU clip convention
    // (the Vulkan vertex stage stores z' = (z + w) / 2).
    if (gx2vk::caps().transform_feedback) {
        const unsigned mode = m.prim & 31;
        uint32_t assembled = m.count;
        if (mode == 3) assembled = m.count > 1 ? (m.count - 1) * 2 : 0;
        if (mode == 5 || mode == 6) assembled = m.count > 2 ? (m.count - 2) * 3 : 0;
        if (mode == 0x13) assembled = (m.count / 4) * 6;
        if (mode == 0x14) assembled = m.count > 3 ? ((m.count - 2) / 2) * 6 : 0;
        if (mode == 0x11) assembled = (m.count / 3) * 6;
        const uint32_t capacity = uint32_t(std::min<uint64_t>((uint64_t(m.count) * 3 + 3) * instance_state[0], 1u << 16));
        auto buffer = gx2vk::create_buffer(VkDeviceSize(capacity) * 16, VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT, gx2vk::MemoryUse::Readback);
        if (buffer.buffer) {
            std::memset(buffer.memory.mapped, 0, size_t(capacity) * 16);
            s_xfb_buffer = buffer.buffer; s_xfb_offset = 0; s_xfb_capacity = capacity;
            s_xfb_step = assembled * (instance_state[1] ? 1 : instance_state[0]);
            gx2r_set_xfb_hook([](bool before, VkCommandBuffer cb) {
                if (before) {
                    const uint32_t offset = std::min(s_xfb_offset, s_xfb_capacity - 1);
                    const VkDeviceSize start = VkDeviceSize(offset) * 16, size = VkDeviceSize(s_xfb_capacity - offset) * 16;
                    vkCmdBindTransformFeedbackBuffersEXT(cb, 0, 1, &s_xfb_buffer, &start, &size);
                    vkCmdBeginTransformFeedbackEXT(cb, 0, 0, nullptr, nullptr);
                } else {
                    vkCmdEndTransformFeedbackEXT(cb, 0, 0, nullptr, nullptr);
                    s_xfb_offset += s_xfb_step;
                }
            });
            const bool trace = g_gx2_frame_trace_active;
            g_gx2_frame_trace_active = false;
            // Positions only: no depth or stencil writes (color writes are masked
            // by the capture pipeline).
            S.raster.depth_write = false; S.raster.stencil_test = false;
            bind_replay_framebuffer();
            if (m.indexed) gx2_render_draw_indexed(m.prim, m.count, m.index_type, m.index_addr, m.first_vertex, mem, instance_state[0], instance_state[1]);
            else gx2_render_draw(m.prim, m.count, m.first_vertex, mem, instance_state[0], instance_state[1]);
            g_gx2_frame_trace_active = trace;
            gx2r_set_xfb_hook(nullptr);
            end_rendering();
            VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            host.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFORM_FEEDBACK_BIT_EXT; host.srcAccessMask = VK_ACCESS_2_TRANSFORM_FEEDBACK_WRITE_BIT_EXT;
            host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT; host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.memoryBarrierCount = 1; dependency.pMemoryBarriers = &host;
            vkCmdPipelineBarrier2(gx2vk::cmd(), &dependency);
            gx2vk::finish();
            const uint32_t vertices = uint32_t(std::min<uint64_t>(uint64_t(assembled) * instance_state[0], capacity));
            out.positions.assign(size_t(vertices) * 4, 0.0f);
            std::memcpy(out.positions.data(), buffer.memory.mapped, size_t(vertices) * 16);
            if (!gx2vk::caps().negative_one_to_one && !(S.regs[0xA204] & (1u << 19)))
                for (size_t i = 0; i < vertices; ++i) out.positions[i * 4 + 2] = 2.0f * out.positions[i * 4 + 2] - out.positions[i * 4 + 3];
            gx2vk::destroy_buffer(buffer);
            s_xfb_buffer = VK_NULL_HANDLE;
        }
    }
    g_gx2_frame_trace_active = previous_trace;
    end_rendering();
    out.width = m.fbo_width; out.height = m.fbo_height;
    out.top_down = uint32_t(clip_state[0]) == wire::UPPER_LEFT;
    read_rgba8(color, out.rgba);
    out.nonblack = 0;
    for (size_t i = 0; i < out.rgba.size(); i += 4) out.nonblack += (out.rgba[i] | out.rgba[i + 1] | out.rgba[i + 2]) != 0;
    const size_t cx = size_t(std::max(0, m.viewport[0] + m.viewport[2] / 2)), cy = size_t(std::max(0, m.viewport[1] + m.viewport[3] / 2));
    if (cx < m.fbo_width && cy < m.fbo_height && !out.rgba.empty()) std::memcpy(out.centre, &out.rgba[(cy * m.fbo_width + cx) * 4], 4);
    S.framebuffer = previous_framebuffer; S.current_fbo_id = previous_fbo;
    S.target_masks = previous_masks; gx2_render_set_color_write(S.color_write);
    S.units = previous_units;
    S.bound_textures = previous_bindings;
    for (auto& [slot, r] : replay) gx2vk::destroy_image(r.image);
    gx2vk::destroy_image(color.image);
    if (m.has_depth) gx2vk::destroy_image(depth.image);
    gx2vk::finish();
    return true;
}

#include "gx2_render_vk_debug.h"
bool gx2_render_vk_read_unit(uint32_t unit, uint32_t target, uint32_t level, std::vector<uint32_t>& rgba, uint32_t& width,
                             uint32_t& height, uint32_t& layers) {
    if (unit >= S.units.size() || target > 3) return false;
    TexObject* t = S.units[unit][target];
    if (!t || !t->image || !t->image->image || level >= std::max(1u, t->levels)) return false;
    width = std::max(1u, t->image->desc.width >> level);
    height = std::max(1u, t->image->desc.height >> level);
    layers = t->target == wire::TEXTURE_3D ? std::max(1u, t->image->desc.depth >> level) : std::max(1u, t->image->desc.layers);
    return read_texture_level(*t, level, width, height, layers, rgba);
}
