#include "../../runtime/rbrew_diagnostics.h"
#include "../../runtime/texture_diagnostics.h"
// GX2 draw, state, shader, texture, and surface stubs

#include "../os_common.h"
#include "gx2_attrib.h"
#include "gx2_blend_state.h"
#include "gx2_pm4_state.h"
#include "gx2_fetch_decode.h"
#include "gx2_native_context.h"
#include "gx2_perf.h"
#include "gx2_render.h"
#include "gx2_surface.h"
#include "gx2_surface_layout.h"
#include "gx2_state_api.h"
#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

static uint32_t gx2_arg(CPUState* cpu,unsigned i) {
  return i<8?cpu->r[3+i]:rbrew_read32(MEM,cpu->r[1]+8+(i-8)*4);
}
static uint32_t float_word(double f) { const float value=float(f);uint32_t w;std::memcpy(&w,&value,4);return w; }
static uint32_t gx2_float_integer(double value) {
  // The library's unsigned conversion uses two saturating fctiwz paths.
  auto signed_word=[](double v){if(!(v==v) || v<-2147483648.0)return 0x80000000u;if(v>2147483647.0)return 0x7FFFFFFFu;return uint32_t(int32_t(v));};
  return value>=2147483648.0?signed_word(value-2147483648.0)+0x80000000u:signed_word(value);
}
static void descriptor_command(CPUState*,Gx2DescriptorFn,uint32_t);
static uint32_t reg_word(const uint8_t*);
static void register_words(CPUState* cpu,uint32_t base,const uint32_t* words,unsigned count) {
  std::vector<Gx2RegisterWrite> writes;for(unsigned i=0;i<count;++i)writes.push_back({base+i,words[i]});
  gx2_render_apply_registers(writes.data(),writes.size());(void)cpu;
}
static void descriptor_registers(CPUState* cpu,const uint8_t* descriptor,uint32_t base,unsigned count) {
  const auto* d=descriptor?descriptor:((ARG0 && rbrew_memory_accessible(MEM,ARG0,count*4))?MEM+ARG0:nullptr);
  if(!d)return;
  std::vector<uint32_t> words;for(unsigned i=0;i<count;++i)words.push_back(reg_word(d+i*4));
  register_words(cpu,base,words.data(),count);
}


// ---------------------------------------------------------------------------
// Draw calls
// ---------------------------------------------------------------------------

// GX2DrawEx(primitiveType, count, firstVertex, numInstances)
// r3=primType, r4=count, r5=firstVertex, r6=numInstances
static void GX2DrawEx(CPUState *cpu) { gx2_render_draw(ARG0, ARG1, ARG2, MEM,ARG3); }

// GX2DrawIndexedEx(primitiveType, count, indexType, indices, firstVertex,
// numInstances) r3=primType, r4=count, r5=indexType, r6=indices(guest),
// r7=firstVertex, r8=numInstances
static void GX2DrawIndexedEx(CPUState *cpu) {
  gx2_render_draw_indexed(ARG0, ARG1, ARG2, ARG3, ARG4, MEM,ARG5);
}

// GX2DrawEx2 — same layout as GX2DrawEx but with additional params; treat as
// DrawEx
void GX2DrawEx2(CPUState *cpu) {
  gx2_render_draw(ARG0, ARG1, ARG2, MEM,ARG3,ARG4);
}

// GX2DrawIndexedEx2 — same as DrawIndexedEx with extra params
static void GX2DrawIndexedEx2(CPUState *cpu) {
  gx2_render_draw_indexed(ARG0, ARG1, ARG2, ARG3, ARG4, MEM,ARG5,ARG6);
}

static void GX2DrawStreamOut(CPUState *cpu) { (void)cpu; }
static void GX2SetStreamOutEnable(CPUState *cpu) {
  const Gx2RegisterWrite write{0xA2AC, ARG0 & 1u};
  gx2_render_apply_registers(&write, 1);
}
#if defined(RBREW_GRAPHICS_VULKAN)
static void apply_stream_out_context(CPUState* cpu) {
  const uint32_t value=ARG2==0?rbrew_read32(MEM,ARG1):ARG2==1?0:ARG1;
  const Gx2RegisterWrite offset{0xA2B7+(ARG0&3)*4,value};
  gx2_render_apply_registers(&offset,1);
}
static void GX2SetStreamOutContext(CPUState* cpu) {
  CPUState command=*cpu;
  // gx2.rpl consumes the descriptor's context pointer while recording;
  // the GPU consumes that context's current byte offset during replay.
  if(ARG2==0) {
    if(!ARG1 || !rbrew_memory_accessible(MEM,ARG1,0x20))return;
    command.r[4]=rbrew_read32(MEM,ARG1+0x1C);
    if(!command.r[4] || !rbrew_memory_accessible(MEM,command.r[4],4))return;
  }
  if(gx2_dl_recording())gx2_dl_record(apply_stream_out_context,&command);
  else apply_stream_out_context(&command);
}
static void apply_save_stream_out_context(CPUState* cpu) {
  rbrew_write32(MEM,ARG1,gx2_render_register(0xA2B7+(ARG0&3)*4));
}
static void GX2SaveStreamOutContext(CPUState* cpu) {
  if(!ARG1 || !rbrew_memory_accessible(MEM,ARG1,0x20))return;
  CPUState command=*cpu;command.r[4]=rbrew_read32(MEM,ARG1+0x1C);
  if(!command.r[4] || !rbrew_memory_accessible(MEM,command.r[4],4))return;
  if(gx2_dl_recording())gx2_dl_record(apply_save_stream_out_context,&command);
  else apply_save_stream_out_context(&command);
}
#endif
static void GX2SetPrimitiveRestartIndex(CPUState *cpu) { const Gx2RegisterWrite w{0xA103,ARG0};gx2_render_apply_registers(&w,1); }
// Occlusion result fast path. The guest counts each query begin issued for an
// address; the renderer records which begin's counters it published into
// guest memory. Once the newest begin is published, GX2QueryGetOcclusionResult
// reads those counters without waiting behind queued render work - the value
// the renderer path computes. Anything else (not yet published, or begins
// inside display lists, which may replay any number of times) waits as before.
namespace {
struct QueryGeneration { uint64_t issued = 0, executed = 0, published = 0; bool listed = false; };
std::mutex s_query_generation_mutex;
std::unordered_map<uint32_t, QueryGeneration> s_query_generations;
}
void gx2_query_render_begin(uint32_t address) {
  std::lock_guard lock(s_query_generation_mutex);
  ++s_query_generations[address].executed;
}
void gx2_query_render_published(uint32_t address) {
  std::lock_guard lock(s_query_generation_mutex);
  auto& g = s_query_generations[address];
  g.published = g.executed;
}
static void note_query_issued(uint32_t address, bool listed) {
  std::lock_guard lock(s_query_generation_mutex);
  auto& g = s_query_generations[address];
  if (listed) g.listed = true; else ++g.issued;
}
static bool query_result_published(uint32_t address) {
  std::lock_guard lock(s_query_generation_mutex);
  const auto it = s_query_generations.find(address);
  if (it == s_query_generations.end()) return false;
  const auto& g = it->second;
  return !g.listed && g.issued && g.issued == g.executed && g.published == g.executed;
}
static void apply_query_begin(CPUState* cpu) { gx2_render_query_begin(ARG0,ARG1,MEM); }
static void GX2QueryBegin(CPUState* cpu) {
  note_query_issued(ARG1, gx2_dl_list_open());
  if(gx2_dl_recording()) {
    // The CPU query helper in gx2.rpl clears its data immediately, including
    // while command recording. GPU-memory queries defer that write to replay.
    if(ARG0==0 && ARG1 && rbrew_memory_accessible(MEM,ARG1,64)) {
      const uint8_t zero[64]{};rbrew_write_memory(MEM,ARG1,zero,64);
    }
    gx2_dl_record(apply_query_begin,cpu);return;
  }
  apply_query_begin(cpu);
}
static void GX2QueryEnd(CPUState* cpu) { gx2_render_query_end(ARG0,ARG1,MEM); }
static void GX2QueryGetOcclusionResult(CPUState* cpu) {
  // Same counter evaluation as gx2_render_query_result after publication.
  if (gx2_native_async_active() && ARG1 && rbrew_memory_accessible(MEM,ARG1,8) &&
      rbrew_memory_accessible(MEM,ARG0,64) && query_result_published(ARG0)) {
    uint64_t samples = 0; bool complete = true;
    for (unsigned rb = 0; rb < 4 && complete; ++rb) {
      const uint64_t start = rbrew_bswap64(rbrew_read64(MEM, ARG0 + rb * 16));
      const uint64_t end = rbrew_bswap64(rbrew_read64(MEM, ARG0 + rb * 16 + 8));
      complete = (start & (1ull << 63)) && (end & (1ull << 63));
      samples += end - start;
    }
    if (complete) { rbrew_write64(MEM, ARG1, samples); RET = 1; return; }
  }
  uint32_t result = 0;
  gx2_native_run(true, [&] { result = gx2_render_query_result(ARG0,ARG1,MEM) ? 1 : 0; });
  RET = result;
}
static void GX2QueryBeginConditionalRender(CPUState* cpu) { gx2_render_conditional_begin(ARG0,ARG1,ARG2!=0,ARG3!=0,MEM); }
static void GX2QueryEndConditionalRender(CPUState*) { gx2_render_conditional_end(); }

// ---------------------------------------------------------------------------
// Clear
// ---------------------------------------------------------------------------

// Clears name their buffer through a descriptor. gx2.rpl encodes the selected
// view (mip/slice) into the command stream at call time, so a display list
// keeps the view chosen when it was recorded even if the game then retargets
// the same descriptor (cascade slices: 0 then 1). Snapshot it while recording.
static void apply_clear_color(CPUState *cpu, const uint8_t *descriptor) {
  // GX2ClearColor(GX2ColorBuffer* cb, float r, float g, float b, float a)
  // r3=cb_ptr, f1-f4=r,g,b,a  (PPC float args use FPR, not GPR)
  gx2_render_clear_color_buffer(ARG0, MEM, (float)cpu->f[1], (float)cpu->f[2],
                                (float)cpu->f[3], (float)cpu->f[4], descriptor);
}
static void GX2ClearColor(CPUState *cpu) {
  if (gx2_dl_recording()) gx2_dl_record_descriptor(apply_clear_color, cpu, GX2CB_SIZEOF);
  else apply_clear_color(cpu, nullptr);
}

static void apply_clear_depth_stencil(CPUState *cpu, const uint8_t *descriptor) {
  // GX2ClearDepthStencilEx(GX2DepthBuffer* db, float depth, uint8 stencil,
  // GX2ClearFlags flags) r3=db, f1=depth, r4=stencil, r5=flags
  gx2_render_clear_depth_buffer(ARG0, MEM, (float)cpu->f[1], (uint8_t)ARG1, ARG2, descriptor);
}
static void GX2ClearDepthStencilEx(CPUState *cpu) {
  if (gx2_dl_recording()) gx2_dl_record_descriptor(apply_clear_depth_stencil, cpu, GX2DB_SIZEOF);
  else apply_clear_depth_stencil(cpu, nullptr);
}

static void apply_clear_buffers(CPUState *cpu, const uint8_t *descriptors) {
  // GX2ClearBuffersEx(GX2ColorBuffer* cb, GX2DepthBuffer* db,
  //                   float r, float g, float b, float a,
  //                   float depth, uint8 stencil, GX2ClearFlags flags)
  // r3=cb, r4=db, f1-f4=r,g,b,a, f5=depth, r5=stencil, r6=flags
  gx2_render_clear_color_buffer(ARG0, MEM, (float)cpu->f[1], (float)cpu->f[2],
                                (float)cpu->f[3], (float)cpu->f[4], descriptors);
  gx2_render_clear_depth_buffer(ARG1, MEM, (float)cpu->f[5], (uint8_t)ARG2, ARG3,
                                descriptors ? descriptors + GX2CB_SIZEOF : nullptr);
}
static void GX2ClearBuffersEx(CPUState *cpu) {
  if (!gx2_dl_recording()) { apply_clear_buffers(cpu, nullptr); return; }
  if (!rbrew_memory_accessible(MEM, ARG0, GX2CB_SIZEOF) || !rbrew_memory_accessible(MEM, ARG1, GX2DB_SIZEOF)) return;
  std::vector<uint8_t> snapshot(MEM + ARG0, MEM + ARG0 + GX2CB_SIZEOF);
  snapshot.insert(snapshot.end(), MEM + ARG1, MEM + ARG1 + GX2DB_SIZEOF);
  gx2_dl_record_snapshot(apply_clear_buffers, cpu, snapshot);
}

extern bool g_gx2_frame_trace_active;
// [ftrace] one line describing a GX2Surface (image, format, size, aa, tile).
static void trace_surface(const char *what, CPUState *cpu, const uint8_t *d,
                          uint32_t extra) {
  if (!g_gx2_frame_trace_active || !d)
    return;
  auto w = [&](uint32_t o) {
    return uint32_t(d[o]) << 24 | uint32_t(d[o + 1]) << 16 |
           uint32_t(d[o + 2]) << 8 | d[o + 3];
  };
  RBREW_LOGF(stderr,
          "[ftrace] %s arg=%u image=%08X fmt=%X %ux%u aa=%u tile=%u use=%X "
          "mips=%u\n",
          what, extra, w(GX2SURF_IMAGE), w(GX2SURF_FORMAT), w(GX2SURF_WIDTH),
          w(GX2SURF_HEIGHT), w(GX2SURF_AA), w(GX2SURF_TILE_MODE),
          w(GX2SURF_USE), w(GX2SURF_MIP_LEVELS));
  (void)cpu;
}
static const uint8_t *guest_desc(CPUState *cpu, uint32_t addr, uint32_t bytes) {
  return addr && rbrew_memory_accessible(MEM, addr, bytes) ? MEM + addr
                                                           : nullptr;
}
static void apply_surface_copy(CPUState* cpu,const uint8_t* d);
static void surface_pair_command(CPUState* cpu,Gx2DescriptorFn fn,uint32_t src,uint32_t src_bytes,uint32_t dst) {
  if(!guest_desc(cpu,src,src_bytes) || !guest_desc(cpu,dst,GX2SURF_SIZEOF))return;
  std::vector<uint8_t> snapshot(MEM+src,MEM+src+src_bytes);
  snapshot.insert(snapshot.end(),MEM+dst,MEM+dst+GX2SURF_SIZEOF);
  // LINEAR_SPECIAL copies are synchronous CPU transfers in gx2.rpl: the
  // guest reads the destination right after the call returns.
  const bool cpu_copy=fn==apply_surface_copy &&
    (reg_word(MEM+src+GX2SURF_TILE_MODE)==16 || reg_word(MEM+dst+GX2SURF_TILE_MODE)==16);
  if(cpu_copy ? gx2_dl_list_open() : gx2_dl_recording())gx2_dl_record_snapshot(fn,cpu,snapshot);
  else gx2_native_run(true,[&]{fn(cpu,snapshot.data());});
}
static void apply_surface_copy(CPUState* cpu,const uint8_t* d) {
  gx2_render_copy_surface(d,ARG1,ARG2,d+GX2SURF_SIZEOF,ARG4,ARG5,MEM);
}
static void apply_depth_convert(CPUState* cpu,const uint8_t* d) {
  std::array<uint8_t,GX2DB_SIZEOF> src;std::copy_n(d,src.size(),src.data());src[GX2SURF_USE+3]|=4;
  const uint32_t slices=reg_word(d+GX2DB_VIEW_NUM_SLICES);
  for(unsigned i=0;i<slices;++i)gx2_render_copy_surface(src.data(),reg_word(d+GX2DB_VIEW_MIP),reg_word(d+GX2DB_VIEW_FIRST_SLICE)+i,d+GX2DB_SIZEOF,ARG2,ARG3+i,MEM);
}
[[maybe_unused]] static void GX2ResolveAAColorBuffer(CPUState *cpu) {
  trace_surface("ResolveAA src", cpu, guest_desc(cpu, ARG0, GX2SURF_SIZEOF), 0);
  trace_surface("ResolveAA dst", cpu, guest_desc(cpu, ARG1, GX2SURF_SIZEOF),
                ARG2);
}

// ---------------------------------------------------------------------------
// Raster state
// ---------------------------------------------------------------------------

static void GX2SetDepthOnlyControl(CPUState *cpu) {
  // GX2SetDepthOnlyControl(depthTest, depthWrite, compareFunc)
  gx2_render_set_depth_stencil_control(ARG0 != 0, ARG1 != 0, ARG2);
}

static void GX2SetAlphaTest(CPUState *cpu) {
  const float ref=float(cpu->f[1]);uint32_t bits;std::memcpy(&bits,&ref,4);
  const Gx2RegisterWrite writes[]={{0xA104,((ARG0&1)<<3)|(ARG1&7)},{0xA10E,bits}};
  gx2_render_apply_registers(writes,2);
}
static void GX2SetBlendConstantColor(CPUState *cpu) {
  const uint32_t w[]={float_word(cpu->f[1]),float_word(cpu->f[2]),float_word(cpu->f[3]),float_word(cpu->f[4])};register_words(cpu,0xA105,w,4);
}
static void apply_blend_constant(CPUState* cpu,const uint8_t* d) { descriptor_registers(cpu,d,0xA105,4); }
static void GX2SetBlendConstantColorReg(CPUState* cpu) { descriptor_command(cpu,apply_blend_constant,16); }
void GX2SetColorMask(CPUState *cpu) {
  const uint32_t mask=(ARG0&1)|((ARG1&1)<<1)|((ARG2&1)<<2)|((ARG3&1)<<3);
  gx2_render_set_target_channel_masks(mask*0x11111111u);
}
static void GX2SetAlphaToMask(CPUState *cpu) {
  const uint32_t patterns[]={0xAA00,0x17800,0x1C600,0x12D00,0x19300};
  const Gx2RegisterWrite w{0xA351,patterns[ARG1<=4?ARG1:0]|(ARG0&1)};gx2_render_apply_registers(&w,1);
}
static void GX2SetStencilMask(CPUState *cpu) {
  const uint32_t w[]={((ARG0&255)<<8)|((ARG1&255)<<16)|(ARG2&255),((ARG3&255)<<8)|((ARG4&255)<<16)|(ARG5&255)};
  register_words(cpu,0xA10C,w,2);
}
static void apply_stencil_mask(CPUState* cpu,const uint8_t* d) { descriptor_registers(cpu,d,0xA10C,2); }
static void GX2SetStencilMaskReg(CPUState* cpu) { descriptor_command(cpu,apply_stencil_mask,8); }
void GX2SetPolygonOffset(CPUState *cpu) {
  const uint32_t w[]={float_word(cpu->f[2]*16),float_word(cpu->f[1]),float_word(cpu->f[4]*16),float_word(cpu->f[3])};
  register_words(cpu,0xA380,w,4);const Gx2RegisterWrite clamp{0xA37F,float_word(cpu->f[5])};gx2_render_apply_registers(&clamp,1);
}
static void apply_polygon_offset(CPUState* cpu,const uint8_t* descriptor) {
  const auto* d=descriptor?descriptor:guest_desc(cpu,ARG0,20);if(!d)return;
  descriptor_registers(cpu,d,0xA380,4);const Gx2RegisterWrite clamp{0xA37F,reg_word(d+16)};gx2_render_apply_registers(&clamp,1);
}
static void GX2SetPolygonOffsetReg(CPUState* cpu) { descriptor_command(cpu,apply_polygon_offset,20); }
static void GX2SetPointSize(CPUState *cpu) {
  auto fixed=[](double v){return std::min(gx2_float_integer(float(v*8.0)),0xFFFFu);};
  const Gx2RegisterWrite w{0xA280,(fixed(cpu->f[1])<<16)|fixed(cpu->f[2])};gx2_render_apply_registers(&w,1);
}
static void GX2SetTVBuffer(CPUState* cpu){gx2_render_set_scan_format(false,ARG3);}
static void GX2SetDRCBuffer(CPUState* cpu){gx2_render_set_scan_format(true,ARG3);}

static void GX2SetTargetChannelMasks(CPUState *cpu) {
  if (g_gx2_frame_trace_active)
    RBREW_LOGF(stderr, "[ftrace] SetTargetChannelMasks %X %X %X %X %X %X %X %X\n",
            ARG0, ARG1, ARG2, ARG3, ARG4, ARG5, ARG6, ARG7);
  const uint32_t args[]={ARG0,ARG1,ARG2,ARG3,ARG4,ARG5,ARG6,ARG7};
  uint32_t masks=0;
  for(unsigned i=0;i<8;++i)masks|=(args[i]&15u)<<(4*i);
  gx2_render_set_target_channel_masks(masks);
}
static void apply_target_channel_masks(CPUState *cpu,const uint8_t* descriptor) {
  const auto* d=descriptor?descriptor:guest_desc(cpu,ARG0,4);
  if(!d)return;
  const uint32_t masks=uint32_t(d[0])<<24|uint32_t(d[1])<<16|uint32_t(d[2])<<8|d[3];
  if(g_gx2_frame_trace_active)RBREW_LOGF(stderr,"[ftrace] SetTargetChannelMasksReg %08X\n",masks);
  gx2_render_set_target_channel_masks(masks);
}
static void GX2SetTargetChannelMasksReg(CPUState *cpu) {
  if(gx2_dl_recording())gx2_dl_record_descriptor(apply_target_channel_masks,cpu,4);
  else apply_target_channel_masks(cpu,nullptr);
}
// GX2ConvertDepthBufferToTextureSurface(depthBuffer, dstSurface, dstMip,
// dstSlice)
static void GX2ConvertDepthBufferToTextureSurface(CPUState *cpu) {
  trace_surface("DepthToTexture src", cpu,
                guest_desc(cpu, ARG0, GX2SURF_SIZEOF), 0);
  trace_surface("DepthToTexture dst", cpu,
                guest_desc(cpu, ARG1, GX2SURF_SIZEOF), ARG2);
  surface_pair_command(cpu,apply_depth_convert,ARG0,GX2DB_SIZEOF,ARG1);
}
static void GX2ExpandDepthBuffer(CPUState *cpu) {
  const auto* descriptor=guest_desc(cpu,ARG0,GX2DB_SIZEOF);
  trace_surface("ExpandDepth", cpu, descriptor, 0);
  if (!descriptor) {
    RBREW_LOGF(stderr,"[gx2] GX2ExpandDepthBuffer received an invalid depth buffer %08X\n",ARG0);
    return;
  }
  if (reg_word(descriptor+GX2SURF_AA)!=0) {
    RBREW_LOGF(stderr,"[gx2] GX2ExpandDepthBuffer does not support multisampled depth storage yet (aa=%u)\n",
            reg_word(descriptor+GX2SURF_AA));
    return;
  }
  // Native depth views have no HiZ compression metadata: the live GPU image is
  // already the expanded depth representation consumed by later passes.
}

// GX2Invalidate(GX2InvalidateMode mode, void* buffer, u32 size): only the
// texture bit (0x2, decaf GX2InvalidateMode::Texture) needs native work here -
// CPU-written textures (font glyph cache) must be uploaded again.
static void GX2Invalidate(CPUState *cpu) {
  TXDIAG(if(txdiag::sample("guest_invalidate",uint64_t(ARG1)<<32|ARG2,ARG0))
      txdiag::event("guest_invalidate","\"mode\":%u,\"address\":%u,\"size\":%u,\"lr\":%u,\"recording_list\":%s",ARG0,ARG1,ARG2,cpu->lr,gx2_dl_list_open()?"true":"false"));
  if (gx2_perf_enabled()) {
    static unsigned s_seen;
    if (s_seen < 40 || s_seen % 1000 == 0)
      RBREW_LOGF(gx2_perf_file(), "[perf-invalidate] #%u mode=%X addr=%08X size=%08X lr=%08X\n", s_seen, ARG0, ARG1, ARG2, cpu->lr);
    ++s_seen;
  }
  if (!(ARG0 & 0x2u)) return;  // only texture caches have native state
  struct Invalidate final : Gx2AsyncWork {
    uint32_t addr, size;
    Invalidate(uint32_t a, uint32_t s) : addr(a), size(s) {}
    void run() override { gx2_render_invalidate_textures(addr, size); }
  };
  // Ordered with the posted commands, like the console command buffer.
  if (!gx2_dl_list_open() && gx2_native_async_active())
    gx2_native_post(new Invalidate(ARG1, ARG2));
  else {
    const uint32_t addr = ARG1, size = ARG2;
    gx2_native_run(false, [&] { gx2_render_invalidate_textures(addr, size); });
  }
}

// Colour/blend state per gx2.rpl (see gx2_blend_state.h); pushed to GPU
// after every change.
Gx2ColorBlendState g_gx2_color_blend;
static void apply_color_blend() {
  const auto &b = g_gx2_color_blend.target0;
  gx2_render_set_blend_control(g_gx2_color_blend.blend_target0(), b.src_color,
                               b.dst_color, b.color_combine, b.src_alpha,
                               b.dst_alpha, b.alpha_combine);
  gx2_render_set_color_write(g_gx2_color_blend.color_write);
}
static void GX2SetColorControl(CPUState *cpu) {
  g_gx2_color_blend.set_color_control(ARG1, ARG3);
  apply_color_blend();
}
static void GX2SetBlendControl(CPUState *cpu) {
  g_gx2_color_blend.set_blend_control(ARG0, ARG1, ARG2, ARG3, ARG4, ARG5, ARG6,
                                      ARG7);
  apply_color_blend();
}

static void gx2_apply_native_default_state(CPUState*) {
  constexpr Gx2RegisterWrite defaults[] = {
      {0xA200, 0x49749716}, {0xA205, 0x280240},
      {0xA10C, 0x00FFFF01}, {0xA10D, 0x00FFFF01},
      {0xA380, 0}, {0xA381, 0}, {0xA382, 0}, {0xA383, 0}, {0xA37F, 0},
      {0xA280, 0x00080008}, {0xA103, 0xFFFFFFFF},
      {0xA104, 1}, {0xA10E, 0}, {0xA351, 0xAA00},
      {0xA2AC, 0}, {0xA343, 0},
  };
  gx2_render_apply_registers(defaults, std::size(defaults));
  gx2_render_set_target_channel_masks(0xFFFFFFFF);
  g_gx2_color_blend.set_color_control(0, 1);
  g_gx2_color_blend.set_blend_control(0, 4, 5, 0, 1, 4, 5, 0);
  apply_color_blend();
}

void gx2_draw_set_default_state(CPUState* cpu) {
  if (gx2_dl_recording())
    gx2_dl_record(gx2_apply_native_default_state, cpu);
  else
    gx2_apply_native_default_state(cpu);
}

static void GX2SetPolygonControl(CPUState *cpu) {
  // GX2SetPolygonControl(frontFaceMode, cullFront, cullBack, ...)
  // r3=frontFace, r4=cullFront, r5=cullBack, ...
  const Gx2RegisterWrite w{0xA205,0x280000|(ARG1&1)|((ARG2&1)<<1)|((ARG0&1)<<2)|((ARG3&3)<<3)|((ARG4&7)<<5)|((ARG5&7)<<8)|((ARG6&1)<<11)|((ARG7&1)<<12)|((gx2_arg(cpu,8)&1)<<13)};
  auto apply=[](CPUState* call){const Gx2RegisterWrite word{0xA205,call->r[3]};gx2_render_apply_registers(&word,1);};
  if(gx2_dl_recording()){CPUState call=*cpu;call.r[3]=w.value;gx2_dl_record(apply,&call);}else gx2_render_apply_registers(&w,1);
}

static void GX2SetRasterizerClipControl(CPUState *cpu) {
  // GX2SetRasterizerClipControl(rasterizer, zclipEnable) writes the whole
  // PA_CL_CLIP_CNTL (decaf GX2SetRasterizerClipControlEx with halfZ=FALSE):
  // DX_RASTERIZATION_KILL = !rasterizer, ZCLIP_NEAR/FAR_DISABLE = !zclip.
  // Gambit brackets shadow casters with (1,0)..(1,1) (02CE4A14/02CE4A50).
  const Gx2RegisterWrite w{0xA204,((ARG0&1)?0u:1u<<22)|((ARG1&1)?0u:(3u<<26))};gx2_render_apply_registers(&w,1);
}

static void GX2SetSpecialState(CPUState *cpu) {
  // GX2SetSpecialState(state, enabled). Gambit's GX2UTClearRectOp uses
  // ClearHiZ (1, no native HiZ) and ClearColorAsDepth (8).
  gx2_render_set_special_state(ARG0, (ARG1 & 1) != 0);
}

static void GX2SetCullOnlyControl(CPUState *cpu) {
  // GX2SetCullOnlyControl(frontFaceMode, cullFront, cullBack)
  const Gx2RegisterWrite w{0xA205,0x280000|(ARG1&1)|((ARG2&1)<<1)|((ARG0&1)<<2)};gx2_render_apply_registers(&w,1);
}

static void GX2SetScissor(CPUState *cpu) {
  // GX2SetScissor(x, y, w, h) — uint32 in r3-r6
  gx2_render_set_scissor(ARG0, ARG1, ARG2, ARG3);
}

static void GX2SetViewport(CPUState *cpu) {
  // GX2SetViewport(x, y, w, h, near, far) — all floats in f1-f6
  gx2_render_set_viewport((float)cpu->f[1], (float)cpu->f[2], (float)cpu->f[3],
                          (float)cpu->f[4], (float)cpu->f[5], (float)cpu->f[6]);
}

static void apply_viewport(CPUState* cpu,const uint8_t* descriptor) {
  const auto* d=descriptor?descriptor:guest_desc(cpu,ARG0,48);if(!d)return;
  descriptor_registers(cpu,d,0xA10F,6);descriptor_registers(cpu,d+24,0xA303,4);descriptor_registers(cpu,d+40,0xA0B4,2);
}
void GX2SetViewportReg(CPUState *cpu) { descriptor_command(cpu,apply_viewport,48); }
static void apply_scissor(CPUState* cpu,const uint8_t* d) { descriptor_registers(cpu,d,0xA090,2); }
void GX2SetScissorReg(CPUState *cpu) { descriptor_command(cpu,apply_scissor,8); }
static uint32_t depth_control_word(CPUState* cpu,unsigned first) {
  uint32_t a[13];for(unsigned i=0;i<13;++i)a[i]=gx2_arg(cpu,i+first);
  return (a[3]&1)|((a[0]&1)<<1)|((a[1]&1)<<2)|((a[2]&7)<<4)|((a[4]&1)<<7)|((a[5]&7)<<8)|((a[8]&7)<<11)|((a[6]&7)<<14)|((a[7]&7)<<17)|((a[9]&7)<<20)|((a[12]&7)<<23)|((a[10]&7)<<26)|((a[11]&7)<<29);
}
void GX2InitDepthStencilControlReg(CPUState *cpu) {
  if(ARG0 && rbrew_memory_accessible(MEM,ARG0,4))rbrew_write32(MEM,ARG0,depth_control_word(cpu,1));
}
void GX2GetDepthStencilControlReg(CPUState *cpu) {
  const uint32_t w=rbrew_read32(MEM,ARG0);const unsigned shift[]={1,2,4,0,7,8,14,17,11,20,26,29,23};
  for(unsigned i=0;i<13;++i){const uint32_t dst=gx2_arg(cpu,i+1);if(dst)rbrew_write32(MEM,dst,(w>>shift[i])&(i<5 && i!=2?1:7));}
}

// ---- Register-struct variants (agl render state). Each takes a pointer to
// the hardware words built by the matching GX2Init*Reg (gx2.rpl):
//   DB_DEPTH_CONTROL   (0202c3e4): b0 stencil, b1 Z enable, b2 Z write, b4-6 Z
//   func PA_SU_SC_MODE_CNTL (0202c798): b0 cull front, b1 cull back, b2 front
//   face CB_COLOR_CONTROL   (0202d820): b4-6 special op (1 = colour off), b8-15
//   blend mask {target, CB_BLEND_CONTROL} (0202e1b4): b0-4 cSrc, b5-7 cComb,
//   b8-12 cDst,
//                                b16-20 aSrc, b21-23 aComb, b24-28 aDst, b29
//                                separate
static void descriptor_command(CPUState*,Gx2DescriptorFn,uint32_t);
static uint32_t reg_word(const uint8_t* d) {
  return uint32_t(d[0])<<24|uint32_t(d[1])<<16|uint32_t(d[2])<<8|d[3];
}
static void apply_depth_control(CPUState *cpu,const uint8_t* descriptor) {
  const auto* d=descriptor?descriptor:guest_desc(cpu,ARG0,4);
  if (!d)
    return;
  const uint32_t w = reg_word(d);
  const Gx2RegisterWrite write{0xA200,w};gx2_render_apply_registers(&write,1);
}
static void apply_polygon_control(CPUState *cpu,const uint8_t* descriptor) {
  const auto* d=descriptor?descriptor:guest_desc(cpu,ARG0,4);
  if (!d)
    return;
  const uint32_t w = reg_word(d);
  const Gx2RegisterWrite write{0xA205,w};gx2_render_apply_registers(&write,1);
}
static void apply_color_control(CPUState *cpu,const uint8_t* descriptor) {
  const auto* d=descriptor?descriptor:guest_desc(cpu,ARG0,4);
  if (!d)
    return;
  const uint32_t w = reg_word(d);
  g_gx2_color_blend.set_color_control((w >> 8) & 0xFF, ((w >> 4) & 7) != 1);
  apply_color_blend();
}
static void apply_blend_control(CPUState *cpu,const uint8_t* descriptor) {
  const auto* d=descriptor?descriptor:guest_desc(cpu,ARG0,8);
  if (!d)
    return;
  const uint32_t target = reg_word(d), w = reg_word(d+4);
  g_gx2_color_blend.set_blend_control(
      target, w & 0x1F, (w >> 8) & 0x1F, (w >> 5) & 7, (w >> 29) & 1,
      (w >> 16) & 0x1F, (w >> 24) & 0x1F, (w >> 21) & 7);
  apply_color_blend();
}
static void GX2SetDepthStencilControlReg(CPUState* cpu){descriptor_command(cpu,apply_depth_control,4);}
static void GX2SetPolygonControlReg(CPUState* cpu){descriptor_command(cpu,apply_polygon_control,4);}
static void GX2SetColorControlReg(CPUState* cpu){descriptor_command(cpu,apply_color_control,4);}
static void GX2SetBlendControlReg(CPUState* cpu){descriptor_command(cpu,apply_blend_control,8);}
static void apply_alpha_test(CPUState* cpu,const uint8_t* descriptor) {
  const auto* d=descriptor?descriptor:guest_desc(cpu,ARG0,8);if(!d)return;
  const Gx2RegisterWrite writes[]={{0xA104,reg_word(d)},{0xA10E,reg_word(d+4)}};
  gx2_render_apply_registers(writes,2);
}
static void GX2SetAlphaTestReg(CPUState* cpu){descriptor_command(cpu,apply_alpha_test,8);}

static void apply_depth_word(CPUState* cpu) { const Gx2RegisterWrite w{0xA200,ARG0};gx2_render_apply_registers(&w,1); }
static void GX2SetDepthStencilControl(CPUState *cpu) {
  CPUState call=*cpu;call.r[3]=depth_control_word(cpu,0);
  if(gx2_dl_recording())gx2_dl_record(apply_depth_word,&call);else apply_depth_word(&call);
}

// ---------------------------------------------------------------------------
// Surface / color buffer / depth buffer
// ---------------------------------------------------------------------------

static void GX2CalcSurfaceSizeAndAlignment(CPUState *cpu) {
  const uint32_t surf = ARG0;
  if (!surf || !rbrew_memory_accessible(MEM, surf, GX2SURF_SIZEOF))
    return;
  auto read = [&](uint32_t off) { return rbrew_read32(MEM, surf + off); };
  RbrewGx2SurfaceDesc desc;
  desc.dim = read(GX2SURF_DIM);
  desc.width = read(GX2SURF_WIDTH);
  desc.height = read(GX2SURF_HEIGHT);
  desc.depth = read(GX2SURF_DEPTH);
  desc.levels = read(GX2SURF_MIP_LEVELS);
  desc.format = read(GX2SURF_FORMAT);
  desc.aa = read(GX2SURF_AA);
  desc.use = read(GX2SURF_USE);
  desc.tile_mode = read(GX2SURF_TILE_MODE);
  desc.swizzle = read(GX2SURF_SWIZZLE);
  // Reject a missing layout explicitly instead of publishing an undersized
  // allocation.
  const auto layout = rbrew_gx2_calc_surface_layout(desc);
  if (!layout) {
    RBREW_LOGF(stderr,
            "[gx2] unsupported surface layout dim=%u fmt=%X tile=%u aa=%u "
            "mips=%u lr=%08X\n",
            desc.dim, desc.format, desc.tile_mode, desc.aa, desc.levels,
            cpu->lr);
    abort();
  }
  rbrew_write32(MEM, surf + GX2SURF_MIP_LEVELS, layout->levels);
  rbrew_write32(MEM, surf + GX2SURF_IMAGE_SIZE, layout->image_size);
  rbrew_write32(MEM, surf + GX2SURF_MIPMAP_SIZE, layout->mip_size);
  rbrew_write32(MEM, surf + GX2SURF_TILE_MODE, layout->tile_mode);
  rbrew_write32(MEM, surf + GX2SURF_SWIZZLE, layout->swizzle);
  rbrew_write32(MEM, surf + GX2SURF_ALIGNMENT, layout->alignment);
  rbrew_write32(MEM, surf + GX2SURF_PITCH, layout->pitch);
  for (uint32_t i = 0; i < layout->mip_offsets.size(); ++i)
    rbrew_write32(MEM, surf + GX2SURF_MIP_OFFSETS + i * 4,
                  layout->mip_offsets[i]);
}

static void GX2CalcColorBufferAuxInfo(CPUState *cpu) {
  RBREW_LOGF(stderr,
          "[gx2] GX2CalcColorBufferAuxInfo is unsupported without the exact "
          "AddrLib auxiliary-layout calculation (buffer=%08X, output=%08X)\n",
          ARG0, ARG1);
  (void)cpu;
}

static void GX2CalcDepthBufferHiZInfo(CPUState *cpu) {
  RBREW_LOGF(stderr,
          "[gx2] GX2CalcDepthBufferHiZInfo is unsupported without the exact "
          "AddrLib HiZ-layout calculation (buffer=%08X, size=%08X, alignment=%08X)\n",
          ARG0, ARG1, ARG2);
  (void)cpu;
}

static std::optional<RbrewGx2SurfaceLayout> buffer_register_layout(CPUState* cpu,uint32_t bytes) {
  if(!ARG0 || !rbrew_memory_accessible(MEM,ARG0,bytes))return {};
  auto w=[&](uint32_t o){return rbrew_read32(MEM,ARG0+o);};
  return rbrew_gx2_calc_surface_layout({w(0),w(4),w(8),w(12),std::max(w(16),w(0x74)+1),w(20),w(24),w(28),w(48),w(52)});
}
static void GX2InitColorBufferRegs(CPUState *cpu) {
  const auto layout=buffer_register_layout(cpu,GX2CB_SIZEOF);if(!layout)return;
  auto w=[&](uint32_t o){return rbrew_read32(MEM,ARG0+o);};
  const uint32_t mip=w(0x74);if(mip>=layout->levels)return;
  const uint32_t pitch=layout->level_pitch[mip],height=layout->level_height[mip],format=w(0x14),hw=format&63;
  const uint32_t size=(((pitch>>3)-1)&1023)|((((pitch*height>>6)-1)&0xFFFFF)<<10);
  unsigned number=0;switch(format&0xF00){case 0x100:number=4;break;case 0x200:number=1;break;case 0x300:number=5;break;case 0x400:number=6;break;case 0x800:number=7;break;}
  // gx2.rpl's format metadata (10012e0c), independently expressed as
  // normalized-export formats. All supported Cafe formats have ENDIAN=0.
  bool normalized=false;
  for(unsigned f:{0u,1u,2u,3u,4u,6u,7u,8u,9u,10u,11u,12u,16u,21u,22u,23u,24u,25u,26u,27u,32u,36u,37u,38u,44u,49u,50u,51u,52u,53u})normalized|=hw==f;
  const bool bypass=(format&0x100) || format==0x11 || format==0x811 || format==0x81C;
  uint32_t info=(hw<<2)|((layout->level_tile[mip]&15)<<8)|(number<<12)|((hw==12 || hw==27)?0x20000:0)|(w(0x18)?0x80000:0);
  if(bypass)info|=0x400000;
  else if(number==0 || number==1 || number==6)info|=0x100000;
  if(!(format&0x100) && normalized)info|=0x8000000;
  if(format&0x800)info|=0x2000000;
  rbrew_write32(MEM,ARG0+0x88,size);rbrew_write32(MEM,ARG0+0x8C,info);
  rbrew_write32(MEM,ARG0+0x90,w(0x30)==16?0:(w(0x78)&0x7FF)|(((w(0x78)+w(0x7C)-1)&0x7FF)<<13));
  rbrew_write32(MEM,ARG0+0x94,0);
}
static void GX2InitDepthBufferRegs(CPUState *cpu) {
  const auto layout=buffer_register_layout(cpu,GX2DB_SIZEOF);if(!layout)return;
  auto w=[&](uint32_t o){return rbrew_read32(MEM,ARG0+o);};
  const uint32_t mip=w(0x74);if(mip>=layout->levels)return;
  const uint32_t pitch=layout->level_pitch[mip],height=layout->level_height[mip];
  uint32_t format=0,poly=0;
  switch(w(0x14)){case 5:format=1;poly=0xF0;break;case 0x11:format=3;poly=0xE8;break;case 0x80E:format=6;poly=0x1E9;break;case 0x811:format=5;poly=0x1EC;break;case 0x81C:format=7;poly=0x1E9;break;}
  const uint32_t values[]={(((pitch>>3)-1)&1023)|((((pitch>>3)*(height>>3)-1)&0xFFFFF)<<10),
      (w(0x78)&0x7FF)|(((w(0x78)+w(0x7C)-1)&0x7FF)<<13),
      8|format|((layout->level_tile[mip]&15)<<15)|(w(0x80)?0x02000000:0),0xB,
      ((w(8)>>3)-1)&1023,(((w(4)>>5)&255)|((w(8)<<3)&0xFF00))<<16,poly};
  for(unsigned i=0;i<7;++i)rbrew_write32(MEM,ARG0+0x90+i*4,values[i]);
}
static void GX2InitDepthBufferHiZEnable(CPUState *cpu) {
  if(ARG0 && rbrew_memory_accessible(MEM,ARG0,GX2DB_SIZEOF))rbrew_write32(MEM,ARG0+0x98,(rbrew_read32(MEM,ARG0+0x98)&0xFDFFFFFF)|(ARG1==1?0x02000000:0));
}

static void apply_color_buffer(CPUState *cpu, const uint8_t *descriptor) {
  // GX2SetColorBuffer(GX2ColorBuffer* cb, GX2RenderTarget target)
  // r3=cb, r4=target (0 = colour buffer 0)
  trace_surface("SetColorBuffer target", cpu,
                descriptor ? descriptor : guest_desc(cpu, ARG0, GX2CB_SIZEOF),
                ARG1);
  gx2_render_set_color_buffer(ARG0, MEM, descriptor, ARG1);
}

static void apply_depth_buffer(CPUState *cpu, const uint8_t *descriptor) {
  // GX2SetDepthBuffer(GX2DepthBuffer* db)
  trace_surface("SetDepthBuffer", cpu,
                descriptor ? descriptor : guest_desc(cpu, ARG0, GX2DB_SIZEOF),
                0);
  gx2_render_set_depth_buffer(ARG0, MEM, descriptor);
}

// GX2 consumes these structures into command state at record time
// (gx2.rpl GX2SetColorBuffer 02036830 / GX2SetDepthBuffer 02036a74).
static void descriptor_command(CPUState *cpu, Gx2DescriptorFn fn,
                               uint32_t bytes) {
  if (gx2_dl_recording())
    gx2_dl_record_descriptor(fn, cpu, bytes);
  else
    fn(cpu, nullptr);
}
static void GX2SetColorBuffer(CPUState *cpu) {
  descriptor_command(cpu, apply_color_buffer, GX2CB_SIZEOF);
}
static void GX2SetDepthBuffer(CPUState *cpu) {
  descriptor_command(cpu, apply_depth_buffer, GX2DB_SIZEOF);
}
static void apply_stream_out_buffer(CPUState* cpu,const uint8_t* descriptor) {
  const uint32_t slot=reg_word(descriptor);
  const uint8_t* buffer=descriptor+4;
  // gx2.rpl GX2SetStreamOutBuffer 020310c8: a non-null dataPtr (+0x04) uses
  // the byte size at +0x00; a null dataPtr uses the GX2RBuffer at +0x0C
  // (elemSize +0x10, elemCount +0x14, buffer pointer +0x18).
  uint32_t size=reg_word(buffer);
  uint32_t base=reg_word(buffer+4);
  if(base==0) {
    size=reg_word(buffer+16)*reg_word(buffer+20);
    base=reg_word(buffer+24);
  }
  const Gx2RegisterWrite writes[]={
      {0xA2B4+slot*4,size>>2},
      {0xA2B6+slot*4,base>>8},
      {0x7200+slot,base>>8},
  };
  gx2_render_apply_registers(writes,std::size(writes));
  (void)cpu;
}
static void GX2SetStreamOutBuffer(CPUState* cpu) {
  const uint32_t slot=ARG0;
  if(slot>=4) {
    RBREW_LOGF(stderr,"[gx2] GX2SetStreamOutBuffer received invalid buffer index %u\n",slot);
    return;
  }
  if(!ARG1 || !rbrew_memory_accessible(MEM,ARG1,0x1C)) {
    RBREW_LOGF(stderr,"[gx2] GX2SetStreamOutBuffer received invalid buffer descriptor %08X\n",ARG1);
    return;
  }
  // RBREW_XFB_DUMP=1: raw descriptor fields for layout verification against
  // gx2.rpl GX2SetStreamOutBuffer 020310c8.
  static const bool xfb_dump = rbrew_diagnostic_env("RBREW_XFB_DUMP") != nullptr;  // read once
  if (xfb_dump) {
    static unsigned dumped[4]{};
    if (dumped[slot] < 4) {
      ++dumped[slot];
      uint32_t w[7];
      for (unsigned i = 0; i < 7; ++i) w[i] = rbrew_read32(MEM, ARG1 + i * 4);
      RBREW_LOGF(stderr, "[xfb] slot=%u desc=%08X words=%08X %08X %08X %08X %08X %08X %08X\n",
              slot, ARG1, w[0], w[1], w[2], w[3], w[4], w[5], w[6]);
    }
  }
  std::array<uint8_t,0x20> snapshot{};
  snapshot[0]=uint8_t(slot>>24);snapshot[1]=uint8_t(slot>>16);
  snapshot[2]=uint8_t(slot>>8);snapshot[3]=uint8_t(slot);
  std::copy_n(MEM+ARG1,0x1C,snapshot.data()+4);
  if(gx2_dl_recording())
    gx2_dl_record_snapshot(apply_stream_out_buffer,cpu,snapshot);
  else
    apply_stream_out_buffer(cpu,snapshot.data());
}

// GX2CopySurface(src, srcLevel, srcSlice, dst, dstLevel, dstSlice)
static void GX2CopySurface(CPUState *cpu) {
  trace_surface("CopySurface src", cpu, guest_desc(cpu, ARG0, GX2SURF_SIZEOF),
                ARG1);
  trace_surface("CopySurface dst", cpu, guest_desc(cpu, ARG3, GX2SURF_SIZEOF),
                ARG4);
  surface_pair_command(cpu,apply_surface_copy,ARG0,GX2SURF_SIZEOF,ARG3);
}

// ---------------------------------------------------------------------------
// Texture / sampler
// ---------------------------------------------------------------------------

static void GX2InitTextureRegs(CPUState *cpu) {
  uint32_t tex = ARG0;
  if (!tex || !rbrew_memory_accessible(MEM, tex, GX2TEX_SIZEOF))
    return;
  auto read = [&](uint32_t off) { return rbrew_read32(MEM, tex + off); };
  auto minimum_one = [&](uint32_t off) {
    uint32_t v = read(off);
    if (!v)
      rbrew_write32(MEM, tex + off, v = 1);
    return v;
  };
  // Pure Cafe/Latte descriptor packing; no allocation, binding or rendering.
  // The embedded surface ends at 0x74 and only five words live at 0x88..98.
  uint32_t width = minimum_one(GX2SURF_WIDTH),
           height = minimum_one(GX2SURF_HEIGHT);
  uint32_t depth = minimum_one(GX2SURF_DEPTH);
  minimum_one(GX2SURF_MIP_LEVELS);
  uint32_t levels = minimum_one(GX2TEX_VIEW_NUM_MIPS);
  uint32_t slices = minimum_one(GX2TEX_VIEW_NUM_SLICES);
  uint32_t dim = read(GX2SURF_DIM), format = read(GX2SURF_FORMAT);
  uint32_t hw_format = format & 63u, pitch = read(GX2SURF_PITCH);
  if (hw_format >= 0x31u && hw_format <= 0x35u)
    pitch *= 4u; // BC block pitch -> pixels
  uint32_t pitch_units = pitch < 8u ? 1u : pitch / 8u;
  uint32_t depth_field =
      dim == GX2_SURFACE_DIM_TEXTURE_CUBE
          ? depth / 6u - 1u
          : (dim == 2u || dim == 4u || dim == 5u || dim == 7u ? depth - 1u
                                                              : 0u);
  uint32_t first_mip = read(GX2TEX_VIEW_MIP),
           first_slice = read(GX2TEX_VIEW_FIRST_SLICE);
  uint32_t components = read(GX2TEX_COMP_MAP);
  uint32_t words[5];
  words[0] = (dim & 7u) | ((read(GX2SURF_TILE_MODE) & 15u) << 3) |
             ((read(GX2SURF_USE) & 4u) ? 0x80u : 0u) |
             (((pitch_units - 1u) & 0x7FFu) << 8) |
             (((width - 1u) & 0x1FFFu) << 19);
  words[1] = ((height - 1u) & 0x1FFFu) | ((depth_field & 0x1FFFu) << 13) |
             (hw_format << 26);
  // Cafe surface formats use native byte ordering (endian-swap selector 0).
  words[2] = ((format & 0x200u) ? 0x55u : 0u) |
             ((format & 0x800u) ? 0x200u : ((format & 0x100u) ? 0x100u : 0u)) |
             ((format & 0x400u) ? 0x800u : 0u) | 0x8000u |
             ((first_mip & 15u) << 28);
  for (unsigned channel = 0; channel < 4; ++channel)
    words[2] |= ((components >> (24u - channel * 8u)) & 7u)
                << (16u + channel * 3u);
  uint32_t aa = read(GX2SURF_AA);
  words[3] =
      ((aa ? aa : first_mip + levels - 1u) & 15u) |
      ((first_slice & 0x1FFFu) << 4) |
      (((first_slice + slices - 1u) & 0x1FFFu) << 17) |
      ((dim == GX2_SURFACE_DIM_TEXTURE_CUBE && depth_field) ? 0x40000000u : 0u);
  // Preserve fields not owned by initialization, including advisory LOD state.
  words[4] = (read(GX2TEX_REGS + 16u) & ~0xC00000FCu) | 0x800000F0u;
  for (unsigned i = 0; i < 5; ++i)
    rbrew_write32(MEM, tex + GX2TEX_REGS + i * 4u, words[i]);
}

static void apply_pixel_texture(CPUState *cpu, const uint8_t *descriptor) {
  // GX2SetPixelTexture(GX2Texture* tex, uint32 slot)
  trace_surface("PixelTexture slot", cpu,
                descriptor ? descriptor : guest_desc(cpu, ARG0, GX2TEX_SIZEOF),
                ARG1);
  gx2_render_upload_texture(ARG0, ARG1, MEM, descriptor);
}

static void apply_vertex_texture(CPUState *cpu, const uint8_t *descriptor) {
  gx2_render_bind_texture(ARG0, ARG1 + 32, MEM, descriptor);
}

static void apply_geometry_texture(CPUState *cpu, const uint8_t *descriptor) {
  gx2_render_bind_texture(ARG0, ARG1 + 64, MEM, descriptor);
}
static void GX2SetPixelTexture(CPUState *cpu) {
  descriptor_command(cpu, apply_pixel_texture, GX2TEX_SIZEOF);
}
static void GX2SetVertexTexture(CPUState *cpu) {
  descriptor_command(cpu, apply_vertex_texture, GX2TEX_SIZEOF);
}
static void GX2SetGeometryTexture(CPUState *cpu) {
  descriptor_command(cpu, apply_geometry_texture, GX2TEX_SIZEOF);
}

static void GX2InitSampler(CPUState *cpu) {
  uint32_t smp = ARG0;
  if (!smp || !rbrew_memory_accessible(MEM,smp,12))
    return;
  // gx2.rpl 02031b4c: SQ_TEX_SAMPLER_WORD0/1/2 (not zero defaults).
  const uint32_t clamp=ARG1&7,filter=ARG2&7;
  rbrew_write32(MEM,smp,0x02028000|(filter<<12)|(filter<<9)|(clamp<<6)|(clamp<<3)|clamp);
  rbrew_write32(MEM,smp+4,0x000FFC00);
  rbrew_write32(MEM,smp+8,0x80000000);
}

static void sampler_field(CPUState* cpu,uint32_t offset,uint32_t mask,uint32_t value) {
  if(ARG0 && rbrew_memory_accessible(MEM,ARG0,12))
    rbrew_write32(MEM,ARG0+offset,(rbrew_read32(MEM,ARG0+offset)&~mask)|(value&mask));
}
static void GX2InitSamplerBorderType(CPUState *cpu) { sampler_field(cpu,0,0x00C00000,(ARG1&3)<<22); }
static void GX2InitSamplerClamping(CPUState* cpu) {
  sampler_field(cpu,0,0x1FF,(ARG1&7)|((ARG2&7)<<3)|((ARG3&7)<<6));
}
static void GX2InitSamplerXYFilter(CPUState* cpu) {
  const uint32_t mag=ARG1+(ARG3?4:0),min=ARG2+(ARG3?4:0);
  sampler_field(cpu,0,0x00387E00,((mag&7)<<9)|((min&7)<<12)|((ARG3&7)<<19));
}
static void GX2InitSamplerZMFilter(CPUState* cpu) {
  sampler_field(cpu,0,0x00078000,((ARG1&3)<<15)|((ARG2&3)<<17));
}
static void GX2InitSamplerDepthCompare(CPUState* cpu) { sampler_field(cpu,0,0x1C000000,(ARG1&7)<<26); }
static void GX2InitSamplerLOD(CPUState* cpu) {
  // fctiwz truncation followed by field masks, gx2.rpl 02031c38.
  auto fixed=[](double v) { return gx2_float_integer(float(v*64.0)); };
  if(ARG0 && rbrew_memory_accessible(MEM,ARG0,12))
    rbrew_write32(MEM,ARG0+4,(fixed(cpu->f[1])&1023)|((fixed(cpu->f[2])&1023)<<10)|(fixed(cpu->f[3])<<20));
}
static void apply_sampler(CPUState* cpu,const uint8_t* descriptor,uint32_t stage) {
  const auto* d=descriptor?descriptor:guest_desc(cpu,ARG0,12);if(!d || ARG1>=18)return;
  const uint32_t base=0xF000+(stage*18+ARG1)*3;
  const Gx2RegisterWrite words[]={{base,reg_word(d)},{base+1,reg_word(d+4)},{base+2,reg_word(d+8)}};
  gx2_render_apply_registers(words,3);
}
static void apply_pixel_sampler(CPUState* cpu,const uint8_t* d) { apply_sampler(cpu,d,0); }
static void apply_vertex_sampler(CPUState* cpu,const uint8_t* d) { apply_sampler(cpu,d,1); }
static void apply_geometry_sampler(CPUState* cpu,const uint8_t* d) { apply_sampler(cpu,d,2); }
static void GX2SetPixelSampler(CPUState *cpu) { descriptor_command(cpu,apply_pixel_sampler,12); }
static void GX2SetVertexSampler(CPUState *cpu) { descriptor_command(cpu,apply_vertex_sampler,12); }
static void GX2SetGeometrySampler(CPUState *cpu) { descriptor_command(cpu,apply_geometry_sampler,12); }
static void sampler_border(CPUState* cpu,uint32_t stage) {
  if(ARG0>=18)return;
  Gx2RegisterWrite words[4];
  for(unsigned i=0;i<4;++i) { const float f=float(cpu->f[i+1]);uint32_t w;std::memcpy(&w,&f,4);words[i]={0xA400+stage*0x80+ARG0*4+i,w}; }
  gx2_render_apply_registers(words,4);
}
static void GX2SetPixelSamplerBorderColor(CPUState* cpu) { sampler_border(cpu,0); }
static void GX2SetVertexSamplerBorderColor(CPUState* cpu) { sampler_border(cpu,1); }
static void GX2SetGeometrySamplerBorderColor(CPUState* cpu) { sampler_border(cpu,2); }

// ---------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------

// gx2.rpl 02031038..020310b0: the resources register is word zero in
// each GX2 shader descriptor. No allocation or guest state changes.
static void GX2GetVertexShaderGPRs(CPUState *cpu) {
  RET = rbrew_read32(MEM, ARG0) & 0xFF;
}
static void GX2GetPixelShaderGPRs(CPUState *cpu) {
  RET = rbrew_read32(MEM, ARG0) & 0xFF;
}
static void GX2GetGeometryShaderGPRs(CPUState *cpu) {
  RET = rbrew_read32(MEM, ARG0) & 0xFF;
}
static void GX2GetVertexShaderStackEntries(CPUState *cpu) {
  RET = (rbrew_read32(MEM, ARG0) >> 8) & 0xFF;
}
static void GX2GetPixelShaderStackEntries(CPUState *cpu) {
  RET = (rbrew_read32(MEM, ARG0) >> 8) & 0xFF;
}
static void GX2GetGeometryShaderStackEntries(CPUState *cpu) {
  RET = (rbrew_read32(MEM, ARG0) >> 8) & 0xFF;
}

// GX2SetVertexShader(GX2VertexShader* vs)   r3 = vs_addr
static void GX2SetVertexShader(CPUState *cpu) {
  gx2_render_set_vertex_shader(ARG0, MEM);
}

// GX2SetPixelShader(GX2PixelShader* ps)   r3 = ps_addr
static void GX2SetPixelShader(CPUState *cpu) {
  gx2_render_set_pixel_shader(ARG0, MEM);
}

// RBREW_SHADER_DUMP_DIR, read once (these GX2 calls run per draw).
static bool shader_dump_enabled() {
  static const bool enabled = rbrew_diagnostic_env("RBREW_SHADER_DUMP_DIR") != nullptr;
  return enabled;
}
static void GX2SetGeometryShader(CPUState *cpu) {
  if (shader_dump_enabled())
    RBREW_LOGF(stderr,"[shader-stage] GS descriptor=%08X lr=%08X\n",ARG0,cpu->lr);
}

// GX2Set{Vertex,Pixel}UniformReg(u32 offset, u32 count, const u32* values)
// gx2.rpl 0202f608/0202f754: offset/count in DWORDs; the values are copied
// into the command stream at call time. Inside an open display list we keep
// a snapshot - the game reuses the source buffer before the list is called.
static void set_uniform_reg(CPUState *cpu, bool is_pixel) {
  const uint32_t index = ARG0, count = ARG1, src = ARG2;
  if (gx2_dl_recording()) {
    if (!src || !count || count > 1024 ||
        !rbrew_memory_accessible(MEM, src, uint64_t(count) * 4))
      return;
    std::vector<uint32_t> words(count);
    for (uint32_t i = 0; i < count; ++i)
      words[i] = rbrew_read32(MEM, src + i * 4);
    gx2_dl_record_uniform_words(is_pixel, index, std::move(words));
    return;
  }
  gx2_render_set_uniform_reg(is_pixel, index, src, count, MEM);
}
static void GX2SetVertexUniformReg(CPUState *cpu) {
  set_uniform_reg(cpu, false);
}
static void GX2SetPixelUniformReg(CPUState *cpu) { set_uniform_reg(cpu, true); }

static void GX2SetGeometryUniformReg(CPUState *cpu) { (void)cpu; }

// GX2SetVertexUniformBlock(uint32_t blockIndex, uint32_t size, const void*
// data) r3=blockIndex, r4=size (bytes), r5=data ptr
static void GX2SetVertexUniformBlock(CPUState *cpu) {
  gx2_render_set_uniform_block(false, ARG0, ARG2, ARG1, MEM);
}

// GX2SetPixelUniformBlock(uint32_t blockIndex, uint32_t size, const void* data)
static void GX2SetPixelUniformBlock(CPUState *cpu) {
  gx2_render_set_uniform_block(true, ARG0, ARG2, ARG1, MEM);
}

static void GX2SetGeometryUniformBlock(CPUState *cpu) {
  if (shader_dump_enabled())
    RBREW_LOGF(stderr,"[shader-stage] GS block=%u address=%08X bytes=%u lr=%08X\n",ARG0,ARG2,ARG1,cpu->lr);
}
static void GX2SetShaderModeEx(CPUState *cpu) {
  if (shader_dump_enabled()) {
    static thread_local uint32_t seen_modes=0;
    if(ARG0<32 && !(seen_modes&(1u<<ARG0))) {
      RBREW_LOGF(stderr,"[shader-stage] mode=%u VS=%u/%u GS=%u/%u PS=%u/%u lr=%08X\n",ARG0,ARG1,ARG2,ARG3,ARG4,ARG5,ARG6,cpu->lr);
      seen_modes|=1u<<ARG0;
    }
  }
}

// GX2SetFetchShader(GX2FetchShader* fs)
// Sets the active fetch shader, which defines the vertex attribute layout.
static void GX2SetFetchShader(CPUState *cpu) {
  // Look the layout up by the struct's program pointer ([3]), which is the
  // same for the original struct and any copy of it.
  const uint32_t program = ARG0 && rbrew_memory_accessible(MEM, ARG0, 0x10)
                               ? rbrew_read32(MEM, ARG0 + 0x0C)
                               : 0;
  gx2_render_set_active_fetch_shader(program);
  // Pre-built fetch shader (models): no GX2InitFetchShaderEx layout, so
  // decode the program's own fetch microcode ([2] = program size). Leaving a
  // level frees its models and the hub reloads its own into that memory, so
  // a cached layout is reused only while the program bytes are unchanged.
  if (program && rbrew_memory_accessible(MEM, ARG0, 0x10)) {
    const uint32_t size = rbrew_read32(MEM, ARG0 + 0x08);
    // [5] numDivisors, [6..7] instance step rates (part of the layout).
    uint32_t divisors[2]{};
    const bool stepped = rbrew_memory_accessible(MEM, ARG0, 0x20) && rbrew_read32(MEM, ARG0 + 0x14);
    if (stepped) { divisors[0] = rbrew_read32(MEM, ARG0 + 0x18); divisors[1] = rbrew_read32(MEM, ARG0 + 0x1C); }
    if (size && size <= 0x10000 && rbrew_memory_accessible(MEM, program, size)) {
      static thread_local std::vector<uint8_t> identity;
      identity.assign(MEM + program, MEM + program + size);
      if (stepped) identity.insert(identity.end(), reinterpret_cast<const uint8_t*>(divisors),
                                   reinterpret_cast<const uint8_t*>(divisors) + sizeof(divisors));
      if (!gx2_render_fetch_program_current(program, identity.data(), uint32_t(identity.size()))) {
        std::vector<R700FetchAttribute> attributes;
        gx2_decode_fetch_program(MEM + program, size, attributes, divisors);
        gx2_render_store_fetch_attributes(program, attributes, identity.data(), uint32_t(identity.size()));
        gx2_render_set_active_fetch_shader(program);
      }
    }
  }
  static unsigned s_logged;
  static const bool s_draw_trace = rbrew_diagnostic_env("RBREW_DRAW_TRACE") != nullptr;
  if (s_draw_trace && program &&
      !gx2_render_active_fetch_attribute_count() && s_logged < 6 &&
      rbrew_memory_accessible(MEM, ARG0, 0x20) &&
      rbrew_memory_accessible(MEM, program, 32)) {
    ++s_logged;
    RBREW_LOGF(stderr,
            "[fetch] struct %08X: %08X %08X %08X %08X %08X | program: %08X "
            "%08X %08X %08X %08X %08X\n",
            ARG0, rbrew_read32(MEM, ARG0), rbrew_read32(MEM, ARG0 + 4),
            rbrew_read32(MEM, ARG0 + 8), rbrew_read32(MEM, ARG0 + 12),
            rbrew_read32(MEM, ARG0 + 16), rbrew_read32(MEM, program),
            rbrew_read32(MEM, program + 4), rbrew_read32(MEM, program + 8),
            rbrew_read32(MEM, program + 12), rbrew_read32(MEM, program + 16),
            rbrew_read32(MEM, program + 20));
  }
}

// GX2SetAttribBuffer(uint32_t index, uint32_t size, uint32_t stride, void*
// buffer) r3=index, r4=size, r5=stride, r6=buffer (guest ptr)
static void GX2SetAttribBuffer(CPUState *cpu) {
  gx2_render_set_attrib_buffer(ARG0, ARG3, ARG1, ARG2);
}

static void GX2CalcFetchShaderSizeEx(CPUState *cpu) {
  uint32_t attrib_count = ARG0;
  RET = 16 + attrib_count * 16;
}

// GX2InitFetchShaderEx(GX2FetchShader* fs, void* program, uint32_t attribCount,
//                      GX2AttribStream* attribs, ...)
// r3=fs, r4=program, r5=attribCount, r6=attribs
static void GX2InitFetchShaderEx(CPUState *cpu) {
  uint32_t fs = ARG0;
  uint32_t attrib_count = ARG2;
  uint32_t streams_addr = ARG3;
  if (!fs || !rbrew_memory_accessible(MEM, fs, 0x20))
    return;
  // GX2FetchShader as gx2.rpl GX2InitFetchShaderEx writes it: [0] type,
  // [1] SQ_PGM_RESOURCES_FS, [2] program size, [3] program, [4] attribCount,
  // [5] numDivisors, [6..7] divisors. Engines copy this struct (the model
  // uses a copy), so the attribute layout is keyed by the program pointer.
  rbrew_write32(MEM, fs + 0x00, ARG4);
  rbrew_write32(MEM, fs + 0x08,
                16 + attrib_count * 16); // = GX2CalcFetchShaderSizeEx
  rbrew_write32(MEM, fs + 0x0C, ARG1);
  rbrew_write32(MEM, fs + 0x10, attrib_count);
  rbrew_write32(MEM, fs + 0x14, 0);
  rbrew_write32(MEM, fs + 0x18, 0);
  rbrew_write32(MEM, fs + 0x1C, 0);
  gx2_render_save_fetch_shader(ARG1, attrib_count, streams_addr, MEM);
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
#include "gx2_addrs.h"

void gx2_draw_register(CPUState *cpu) {
#if defined(HAVE_SDL2)
#define REG(name) rbrew_register_func(cpu, ADDR_##name, GX2_NATIVE_CALL(name))
#else
#define REG(name)                                                              \
  rbrew_register_func(cpu, ADDR_##name,                                        \
                      gx2_command_call<name, gx2_is_command(#name)>)
#endif
  REG(GX2DrawEx);
  REG(GX2DrawIndexedEx);
  REG(GX2DrawIndexedEx2);
  REG(GX2QueryBegin);
  REG(GX2QueryEnd);
  // Caller-side: answers from published counters without waiting, else
  // waits on the renderer itself (see GX2QueryGetOcclusionResult).
  rbrew_register_func(cpu, ADDR_GX2QueryGetOcclusionResult, gx2_native_call<GX2QueryGetOcclusionResult, false, false, Gx2Access::None>);
  REG(GX2QueryBeginConditionalRender);
  REG(GX2QueryEndConditionalRender);
  REG(GX2SetStreamOutEnable);
  REG(GX2SetStreamOutBuffer);
#if defined(RBREW_GRAPHICS_VULKAN)
  REG(GX2SetStreamOutContext);
  REG(GX2SaveStreamOutContext);
#endif
  REG(GX2SetPrimitiveRestartIndex);
  REG(GX2ClearColor);
  REG(GX2ClearDepthStencilEx);
  REG(GX2ClearBuffersEx);
  REG(GX2SetColorControl);
  REG(GX2SetDepthOnlyControl);
  REG(GX2SetAlphaTest);
  REG(GX2SetAlphaTestReg);
  REG(GX2SetBlendControl);
  REG(GX2SetBlendConstantColor);
  REG(GX2SetBlendConstantColorReg);
  REG(GX2SetTargetChannelMasks);
  REG(GX2SetTVBuffer);
  REG(GX2SetDRCBuffer);
  REG(GX2SetTargetChannelMasksReg);
  REG(GX2ConvertDepthBufferToTextureSurface);
  REG(GX2ExpandDepthBuffer);
  REG(GX2Invalidate);
  REG(GX2SetAlphaToMask);
  REG(GX2SetStencilMask);
  REG(GX2SetStencilMaskReg);
  REG(GX2SetPolygonControl);
  REG(GX2SetCullOnlyControl);
  REG(GX2SetRasterizerClipControl);
  REG(GX2SetSpecialState);
  REG(GX2SetPointSize);
  REG(GX2SetScissor);
  REG(GX2SetViewport);
  REG(GX2SetDepthStencilControl);
  REG(GX2SetDepthStencilControlReg);
  REG(GX2SetPolygonControlReg);
  REG(GX2SetPolygonOffsetReg);
  REG(GX2SetColorControlReg);
  REG(GX2SetBlendControlReg);
  REG(GX2CalcSurfaceSizeAndAlignment);
  REG(GX2CalcColorBufferAuxInfo);
  REG(GX2CalcDepthBufferHiZInfo);
  REG(GX2InitColorBufferRegs);
  REG(GX2InitDepthBufferRegs);
  REG(GX2InitDepthBufferHiZEnable);
  REG(GX2SetColorBuffer);
  REG(GX2SetDepthBuffer);
  REG(GX2CopySurface);
  REG(GX2InitTextureRegs);
  REG(GX2SetPixelTexture);
  REG(GX2SetVertexTexture);
  REG(GX2SetGeometryTexture);
  REG(GX2InitSampler);
  REG(GX2InitSamplerBorderType);
  REG(GX2InitSamplerClamping);
  REG(GX2InitSamplerDepthCompare);
  REG(GX2InitSamplerLOD);
  REG(GX2InitSamplerXYFilter);
  REG(GX2InitSamplerZMFilter);
  REG(GX2SetPixelSampler);
  REG(GX2SetVertexSampler);
  REG(GX2SetGeometrySampler);
  REG(GX2SetPixelSamplerBorderColor);
  REG(GX2SetVertexSamplerBorderColor);
  REG(GX2SetGeometrySamplerBorderColor);
  REG(GX2SetVertexShader);
  REG(GX2GetVertexShaderGPRs);
  REG(GX2GetPixelShaderGPRs);
  REG(GX2GetGeometryShaderGPRs);
  REG(GX2GetVertexShaderStackEntries);
  REG(GX2GetPixelShaderStackEntries);
  REG(GX2GetGeometryShaderStackEntries);
  REG(GX2SetPixelShader);
  REG(GX2SetGeometryShader);
  REG(GX2SetVertexUniformReg);
  REG(GX2SetPixelUniformReg);
  REG(GX2SetVertexUniformBlock);
  REG(GX2SetPixelUniformBlock);
  REG(GX2SetGeometryUniformBlock);
  REG(GX2SetShaderModeEx);
  REG(GX2SetFetchShader);
  REG(GX2CalcFetchShaderSizeEx);
  REG(GX2InitFetchShaderEx);
  REG(GX2SetAttribBuffer);
#undef REG
}
