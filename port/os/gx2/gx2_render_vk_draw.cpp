#include "../../runtime/rbrew_diagnostics.h"
#include "gx2_texture_diagnostics.h"
// gx2_render_vk_draw.cpp - Vulkan draw submission for GX2: translated
// programs, pipelines, rendering instances, dynamic state, vertex/index
// streams, descriptors, occlusion queries and internal transfer passes.
// Mirrors the draw path of gx2_render.cpp (see gx2_render_vk_state.h).
#include "gx2_render.h"
#include "gx2_pm4_state.h"
#include "gx2_surface.h"
#include "r700_to_glsl.h"
#include "gx2_shader_program.h"
#include "gx2_perf.h"
#include "gx2_write_watch.h"
#include "gx2_shader_key.h"
#include "r700_translation_reads.h"
#include "gx2_host_memory.h"
#include "gx2_attrib.h"
#include "vk/gx2_render_vk_state.h"
#include "vk/gx2_vk_shader.h"
#include "vk/gx2_compile_queue.h"
#include "../os_common.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <immintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#include <memory>
#include <cstdio>
#include <cstring>
#include <future>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <filesystem>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <type_traits>
#include <unordered_set>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

using namespace gx2r;

namespace {
State& S = state();
Counters& C = counters();

// ---------------------------------------------------------------------------
// Rendering instance and per-command-buffer dynamic state
// ---------------------------------------------------------------------------
bool s_rendering = false;
bool s_streamout_only = false;
uint32_t s_last_program = 0;            // diagnostics (gx2_render_vk_debug.h)
uint64_t s_submitted_primitives = 0;
Framebuffer s_active_fb;
VkRect2D s_area{};
uint64_t s_dynamic_serial = 0;      // command buffer the tracked state belongs to
VkPipeline s_bound_pipeline = VK_NULL_HANDLE;
struct DynamicState {
    VkViewport viewport{}; VkRect2D scissor{};
    VkCullModeFlags cull = 0xFFFFFFFF; VkFrontFace front = VK_FRONT_FACE_MAX_ENUM;
    int depth_test = -1, depth_write = -1, stencil_test = -1, depth_bias = -1, restart = -1;
    VkCompareOp depth_op = VK_COMPARE_OP_MAX_ENUM;
    float bias[3]{NAN, NAN, NAN};
    float blend[4]{NAN, NAN, NAN, NAN};
    struct Face { VkStencilOp fail, pass, zfail; VkCompareOp op; uint32_t compare, write, ref; } face[2]{};
    bool face_valid = false, viewport_valid = false, scissor_valid = false;
} s_dyn;


// Command-buffer state, separate from resource residency. build_stage still
// checks live guest bytes and renews upload-span lifetimes on every draw.
// Only repeated binding commands with identical values may be omitted.
struct DescriptorValue {
    uint32_t binding=0;
    VkDescriptorType type=VK_DESCRIPTOR_TYPE_MAX_ENUM;
    VkDescriptorBufferInfo buffer{};
    VkDescriptorImageInfo image{};
};
// R700 imports up to 32 vertex semantics; the game's particle shaders read 17.
constexpr uint32_t kMaxVertexInputs = 32;
struct BindingState {
    VkPipelineLayout descriptor_layout=VK_NULL_HANDLE, constant_layout=VK_NULL_HANDLE;
    uint32_t descriptor_count=0, vertex_count=0;
    std::array<DescriptorValue,70> descriptors{};
    std::array<VkBuffer,kMaxVertexInputs> vertex_buffers{};
    std::array<VkDeviceSize,kMaxVertexInputs> vertex_offsets{};
    int32_t instance=0;
} s_bindings;

bool same_descriptors(VkPipelineLayout layout,const VkWriteDescriptorSet* writes,uint32_t count) {
    if(layout!=s_bindings.descriptor_layout || count!=s_bindings.descriptor_count)return false;
    for(uint32_t i=0;i<count;++i) {
        const auto& w=writes[i];const auto& old=s_bindings.descriptors[i];
        if(w.pNext || w.dstArrayElement || w.descriptorCount!=1 || w.dstBinding!=old.binding || w.descriptorType!=old.type)return false;
        if(w.descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
            const auto& b=*w.pBufferInfo;
            if(b.buffer!=old.buffer.buffer || b.offset!=old.buffer.offset || b.range!=old.buffer.range)return false;
        } else if(w.descriptorType==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
            const auto& b=*w.pImageInfo;
            if(b.sampler!=old.image.sampler || b.imageView!=old.image.imageView || b.imageLayout!=old.image.imageLayout)return false;
        } else return false;
    }
    return true;
}
void remember_descriptors(VkPipelineLayout layout,const VkWriteDescriptorSet* writes,uint32_t count) {
    s_bindings.descriptor_layout=VK_NULL_HANDLE;
    if(count>s_bindings.descriptors.size())return;
    for(uint32_t i=0;i<count;++i) {
        const auto& w=writes[i];auto& saved=s_bindings.descriptors[i];
        if(w.pNext || w.dstArrayElement || w.descriptorCount!=1)return;
        saved.binding=w.dstBinding;saved.type=w.descriptorType;
        if(w.descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)saved.buffer=*w.pBufferInfo;
        else if(w.descriptorType==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)saved.image=*w.pImageInfo;
        else return;
    }
    s_bindings.descriptor_layout=layout;s_bindings.descriptor_count=count;
}
void bind_instance(VkCommandBuffer cb,VkPipelineLayout layout,int32_t instance) {
    if(layout==s_bindings.constant_layout && instance==s_bindings.instance)return;
    vkCmdPushConstants(cb,layout,VK_SHADER_STAGE_VERTEX_BIT,0,4,&instance);
    s_bindings.constant_layout=layout;s_bindings.instance=instance;
}
void bind_vertices(VkCommandBuffer cb,uint32_t count,const VkBuffer* buffers,const VkDeviceSize* offsets) {
    if(!count)return;
    if(count==s_bindings.vertex_count && std::equal(buffers,buffers+count,s_bindings.vertex_buffers.begin()) &&
       std::equal(offsets,offsets+count,s_bindings.vertex_offsets.begin()))return;
    vkCmdBindVertexBuffers(cb,0,count,buffers,offsets);
    s_bindings.vertex_count=count;
    std::copy_n(buffers,count,s_bindings.vertex_buffers.begin());std::copy_n(offsets,count,s_bindings.vertex_offsets.begin());
}

void reset_command_tracking() {
    s_dyn = DynamicState{};
    s_bindings.descriptor_layout = s_bindings.constant_layout = VK_NULL_HANDLE;
    s_bindings.descriptor_count = s_bindings.vertex_count = 0;
    s_bound_pipeline = VK_NULL_HANDLE;
    s_dynamic_serial = gx2vk::recording_serial();
}

// ---------------------------------------------------------------------------
// Occlusion query storage
// ---------------------------------------------------------------------------
constexpr uint32_t kQueriesPerPool = 4096;
std::vector<VkQueryPool> s_query_pools;
std::vector<uint32_t> s_free_queries;
std::vector<uint32_t> s_deferred_free_queries;   // freed while their submission may still run
bool s_query_active = false;                     // a query of the open segment is begun in this instance
uint32_t s_active_query = 0;

uint32_t allocate_query() {
    if (s_free_queries.empty()) {
        VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        info.queryType = VK_QUERY_TYPE_OCCLUSION;
        info.queryCount = kQueriesPerPool;
        VkQueryPool pool = VK_NULL_HANDLE;
        vkCreateQueryPool(gx2vk::device(), &info, nullptr, &pool);
        vkResetQueryPool(gx2vk::device(), pool, 0, kQueriesPerPool);
        const uint32_t base = uint32_t(s_query_pools.size()) * kQueriesPerPool;
        s_query_pools.push_back(pool);
        for (uint32_t i = kQueriesPerPool; i-- > 0;) s_free_queries.push_back(base + i);
    }
    const uint32_t q = s_free_queries.back();
    s_free_queries.pop_back();
    return q;
}
VkQueryPool query_pool(uint32_t q) { return s_query_pools[q / kQueriesPerPool]; }
void release_query(uint32_t q) {
    // Host reset is legal once the GPU no longer uses the query.
    vkResetQueryPool(gx2vk::device(), query_pool(q), q % kQueriesPerPool, 1);
    s_free_queries.push_back(q);
}

std::map<std::pair<uintptr_t, uint32_t>, NativeQuery> s_native_queries;
std::set<NativeQuery*> s_open_queries;
std::shared_ptr<QuerySegment> s_query_segment;

// Per-draw diagnostic query (frame trace / replay fragment counts).
bool s_trace_query_open = false;
uint32_t s_trace_query = UINT32_MAX;
uint32_t s_last_trace_fragments = ~0u;

void begin_segment_query(VkCommandBuffer cb) {
    if (!s_query_segment || s_query_active) return;
    const uint32_t q = allocate_query();
    s_query_segment->indices.push_back(q);
    vkCmdBeginQuery(cb, query_pool(q), q % kQueriesPerPool, gx2vk::caps().precise_occlusion ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    s_query_active = true; s_active_query = q;
}
void end_segment_query(VkCommandBuffer cb) {
    if (!s_query_active) return;
    vkCmdEndQuery(cb, query_pool(s_active_query), s_active_query % kQueriesPerPool);
    s_query_active = false;
    if (s_query_segment) s_query_segment->serial = gx2vk::recording_serial();
}
}  // namespace

gx2r::QuerySegment::~QuerySegment() {
    if (!gx2vk::ready()) return;
    // Queries of an incomplete submission return to the pool after it completes.
    const bool done = serial <= gx2vk::completed_serial();
    for (auto q : indices) { if (done) release_query(q); else s_deferred_free_queries.push_back(q); }
}

bool gx2r::rendering_active() { return s_rendering; }
VkRect2D gx2r::render_area() { return s_area; }
void gx2r::note_framebuffer_changed() {}

void gx2r::query_rendering_begin(VkCommandBuffer cb) { begin_segment_query(cb); }
void gx2r::query_rendering_end(VkCommandBuffer cb) {
    end_segment_query(cb);
}

void gx2r::end_rendering() {
    if (!s_rendering) return;
    VkCommandBuffer cb = gx2vk::cmd();
    query_rendering_end(cb);
    if (s_trace_query_open) {
        // A traced draw's query never spans instances (begun and ended around it).
        s_trace_query_open = false;
        vkCmdEndQuery(cb, query_pool(s_trace_query), s_trace_query % kQueriesPerPool);
    }
    vkCmdEndRendering(cb);
    s_rendering = false;
    gx2vk::note_gpu_write();
}

void gx2r::before_submit() {
    end_rendering();
    // Deferred query slots whose submissions completed can be reused.
    const uint64_t done = gx2vk::completed_serial();
    (void)done;
    s_dynamic_serial = 0;
}

static VkImageView attachment_view(ColorTarget& t) {
    gx2vk::ViewDesc v;
    v.type = VK_IMAGE_VIEW_TYPE_2D;
    v.format = t.fmt.attachment;
    return gx2vk::image_view(t.image, v);
}
static VkImageView depth_attachment_view(DepthTarget& t) {
    gx2vk::ViewDesc v;
    v.type = VK_IMAGE_VIEW_TYPE_2D;
    v.aspect = t.image.aspect;
    return gx2vk::image_view(t.image, v);
}

bool gx2r::ensure_rendering(bool streamout_only) {
    if (!S.framebuffer.valid && !streamout_only) return false;
    if (s_rendering && s_active_fb == S.framebuffer && s_streamout_only==streamout_only) return true;
    end_rendering();
    const auto& fb = S.framebuffer;
    uint32_t w = UINT32_MAX, h = UINT32_MAX;
    for (auto* t : fb.colors) {
        if (!t) continue;
        // Incomplete framebuffer (non-renderable format): GPU draws nothing.
        if (!(t->image.desc.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) return false;
        w = std::min(w, t->width); h = std::min(h, t->height);
    }
    if (fb.depth) { w = std::min(w, fb.depth->width); h = std::min(h, fb.depth->height); }
    if(w==UINT32_MAX && streamout_only)w=h=1; // GPU update has no raster attachments
    if (w == UINT32_MAX || !w || !h) return false;
    VkCommandBuffer cb = gx2vk::cmd();
    if (s_dynamic_serial != gx2vk::recording_serial()) reset_command_tracking();
    gx2vk::barrier_if_dirty();
    VkRenderingAttachmentInfo colors[8]{};
    for (unsigned i = 0; i < 8; ++i) {
        auto& a = colors[i];
        a.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        if (!fb.colors[i]) continue;
        a.imageView = attachment_view(*fb.colors[i]);
        a.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, stencil{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    if (fb.depth) {
        depth.imageView = depth_attachment_view(*fb.depth);
        depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (fb.depth->stencil) stencil = depth;
    }
    VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
    info.renderArea = {{0, 0}, {w, h}};
    info.layerCount = 1;
    info.colorAttachmentCount = streamout_only?0:8;
    info.pColorAttachments = streamout_only?nullptr:colors;
    info.pDepthAttachment = fb.depth && !streamout_only ? &depth : nullptr;
    info.pStencilAttachment = fb.depth && fb.depth->stencil && !streamout_only ? &stencil : nullptr;
    vkCmdBeginRendering(cb, &info);
    s_rendering = true;
    s_streamout_only = streamout_only;
    s_active_fb = fb;
    s_area = info.renderArea;
    ++C.rendering_instances;
    query_rendering_begin(cb);
    return true;
}

// ---------------------------------------------------------------------------
// Dummy textures: GPU returns (0,0,0,1) for incomplete textures and for
// targets never bound on a unit (e.g. sampler1D units).
// ---------------------------------------------------------------------------
namespace {
struct Dummy { gx2vk::Image image; TexObject tex; };
std::map<uint32_t, Dummy> s_dummies;
}
TexObject* gx2r::dummy_texture(uint32_t dim, bool integer, bool sint, bool shadow) {
    const uint32_t key = dim | (integer ? 0x10 : 0) | (sint ? 0x20 : 0) | (shadow ? 0x40 : 0);
    auto [it, added] = s_dummies.try_emplace(key);
    auto& d = it->second;
    if (added) {
        gx2vk::ImageDesc desc;
        desc.type = dim == 1 ? VK_IMAGE_TYPE_1D : dim == 5 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
        desc.format = shadow ? VK_FORMAT_D32_SFLOAT : integer ? (sint ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_UINT) : VK_FORMAT_R8G8B8A8_UNORM;
        desc.layers = dim == 4 ? 6 : 1;
        if (dim == 4) desc.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        desc.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        gx2vk::create_image(d.image, desc);
        if (shadow) clear_image_depth(d.image, 0.0f, 0);
        else {
            VkClearColorValue v{};
            if (integer) v.uint32[3] = 1; else v.float32[3] = 1.0f;
            clear_image_color(d.image, v);
        }
        d.tex.image = &d.image;
        d.tex.target = dim == 3 ? wire::TEXTURE_2D_ARRAY : dim == 4 ? wire::TEXTURE_CUBE_MAP_ARRAY : dim == 5 ? wire::TEXTURE_3D : dim == 1 ? wire::TEXTURE_1D : wire::TEXTURE_2D;
        d.tex.view_format = desc.format;
        d.tex.levels = 1; d.tex.layers = desc.layers;
        d.tex.integer = integer; d.tex.depth = shadow;
        d.tex.params.min_filter = d.tex.params.mag_filter = wire::NEAREST;
        d.tex.params.max_level = 0;
    }
    return &d.tex;
}

// SAMPLE_C on a single-channel color texture: Latte compares the reference
// against the red channel, so GPU-rendered light depth in an R32F/R16 color
// target (or a color-format texture over it) is a valid shadow map. Vulkan
// depth comparison needs a depth format: keep a depth copy of the live texels
// (same bits: R32_SFLOAT -> D32_SFLOAT, R16_UNORM -> D16_UNORM), refreshed
// whenever the source image's content generation changes. Substituting the
// zero dummy instead shadows every receiver inside the light projection.
namespace {
struct ShadowCopy { gx2vk::Image image; gx2vk::Buffer staging; TexObject tex; uint64_t source_generation = ~0ull; };
std::map<const gx2vk::Image*, ShadowCopy> s_shadow_copies;
}
static TexObject* shadow_depth_copy(const TexObject& source) {
    gx2vk::Image& src = *source.image;
    const auto& sd = src.desc;
    const VkFormat format = sd.format == VK_FORMAT_R32_SFLOAT ? VK_FORMAT_D32_SFLOAT :
                            sd.format == VK_FORMAT_R16_UNORM ? VK_FORMAT_D16_UNORM : VK_FORMAT_UNDEFINED;
    if (format == VK_FORMAT_UNDEFINED || sd.type != VK_IMAGE_TYPE_2D || (sd.flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT)) return nullptr;
    const uint32_t bpp = format == VK_FORMAT_D32_SFLOAT ? 4 : 2;
    auto& c = s_shadow_copies[&src];
    const auto& cd = c.image.desc;
    if (!c.image.image || cd.format != format || cd.width != sd.width || cd.height != sd.height ||
        cd.layers != sd.layers || cd.levels != sd.levels) {
        if (c.image.image) gx2vk::destroy_image(c.image);
        if (c.staging.buffer) gx2vk::destroy_buffer(c.staging);
        gx2vk::ImageDesc desc;
        desc.format = format;
        desc.width = sd.width; desc.height = sd.height; desc.layers = sd.layers; desc.levels = sd.levels;
        desc.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!gx2vk::create_image(c.image, desc)) return nullptr;
        VkDeviceSize bytes = 0;
        for (uint32_t l = 0; l < sd.levels; ++l)
            bytes += VkDeviceSize(std::max(1u, sd.width >> l)) * std::max(1u, sd.height >> l) * sd.layers * bpp;
        c.staging = gx2vk::create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                         gx2vk::MemoryUse::DeviceLocal);
        if (!c.staging.buffer) { gx2vk::destroy_image(c.image); return nullptr; }
        c.source_generation = ~0ull;
        if (!c.tex.id) c.tex.id = S.next_object_id++;
    }
    const uint32_t id = c.tex.id;
    c.tex = source;
    c.tex.id = id;
    c.tex.image = &c.image;
    c.tex.view_format = format;
    c.tex.depth = true; c.tex.integer = c.tex.compressed = false;
    if (c.source_generation != src.content_generation) {
        end_rendering();
        const auto cb = gx2vk::cmd();
        gx2vk::barrier_if_dirty();
        std::vector<VkBufferImageCopy> regions;
        VkDeviceSize offset = 0;
        for (uint32_t l = 0; l < sd.levels; ++l) {
            const uint32_t w = std::max(1u, sd.width >> l), h = std::max(1u, sd.height >> l);
            VkBufferImageCopy r{};
            r.bufferOffset = offset;
            r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, sd.layers};
            r.imageExtent = {w, h, 1};
            regions.push_back(r);
            offset += VkDeviceSize(w) * h * sd.layers * bpp;
        }
        vkCmdCopyImageToBuffer(cb, src.image, VK_IMAGE_LAYOUT_GENERAL, c.staging.buffer, uint32_t(regions.size()), regions.data());
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
        for (auto& r : regions) r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        vkCmdCopyBufferToImage(cb, c.staging.buffer, c.image.image, VK_IMAGE_LAYOUT_GENERAL, uint32_t(regions.size()), regions.data());
        gx2vk::note_gpu_write(c.image);
        c.source_generation = src.content_generation;
        if (shadow_log())
            RBREW_LOGF(stderr, "[shadow] SAMPLE_C on color texture: depth copy %ux%ux%u fmt %d -> %d\n", sd.width, sd.height,
                         sd.layers, int(sd.format), int(format));
    }
    return &c.tex;
}

static VkComponentSwizzle swizzle_of(uint32_t sel) {
    static const VkComponentSwizzle map[6] = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B,
                                              VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE};
    return sel < 6 ? map[sel] : VK_COMPONENT_SWIZZLE_ZERO;
}

static VkFormat integer_view_format(VkFormat f, bool sint) {
    switch (f) {
    case VK_FORMAT_R8_UNORM: return sint ? VK_FORMAT_R8_SINT : VK_FORMAT_R8_UINT;
    case VK_FORMAT_R8G8_UNORM: return sint ? VK_FORMAT_R8G8_SINT : VK_FORMAT_R8G8_UINT;
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: return sint ? VK_FORMAT_R8G8B8A8_SINT : VK_FORMAT_R8G8B8A8_UINT;
    case VK_FORMAT_R16_SFLOAT: return sint ? VK_FORMAT_R16_SINT : VK_FORMAT_R16_UINT;
    case VK_FORMAT_R16G16_SFLOAT: return sint ? VK_FORMAT_R16G16_SINT : VK_FORMAT_R16G16_UINT;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return sint ? VK_FORMAT_R16G16B16A16_SINT : VK_FORMAT_R16G16B16A16_UINT;
    case VK_FORMAT_R32_SFLOAT: return sint ? VK_FORMAT_R32_SINT : VK_FORMAT_R32_UINT;
    case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT:
        return sint ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_UINT;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    default: return VK_FORMAT_UNDEFINED;
    }
}

VkImageView gx2r::tex_object_view(TexObject& tex, uint32_t dim, bool integer_sampler, bool sint_sampler) {
    gx2vk::ViewDesc v;
    v.type = dim == 1 ? VK_IMAGE_VIEW_TYPE_1D : dim == 3 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : dim == 4 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY :
             dim == 5 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    v.format = tex.view_format;
    if (integer_sampler && !tex.integer) v.format = integer_view_format(tex.view_format, sint_sampler);
    else if (integer_sampler && tex.integer) {
        // usampler vs isampler of a 128-bit integer target
        if (sint_sampler && v.format == VK_FORMAT_R32G32B32A32_UINT) v.format = VK_FORMAT_R32G32B32A32_SINT;
        if (!sint_sampler && v.format == VK_FORMAT_R32G32B32A32_SINT) v.format = VK_FORMAT_R32G32B32A32_UINT;
    }
    const uint32_t w4 = tex.swizzle;
    v.swizzle = {swizzle_of((w4 >> 16) & 7), swizzle_of((w4 >> 19) & 7), swizzle_of((w4 >> 22) & 7), swizzle_of((w4 >> 25) & 7)};
    const uint32_t base = std::min(tex.params.base_level, tex.levels ? tex.levels - 1 : 0);
    const uint32_t last = std::min(tex.levels ? tex.levels - 1 : 0, tex.params.max_level);
    v.base_level = base;
    v.levels = last >= base ? last - base + 1 : 1;
    v.base_layer = 0;
    v.layers = v.type == VK_IMAGE_VIEW_TYPE_3D ? 1 : (v.type == VK_IMAGE_VIEW_TYPE_2D || v.type == VK_IMAGE_VIEW_TYPE_1D) ? 1 : std::max(1u, tex.layers);
    if (tex.depth) v.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    return gx2vk::image_view(*tex.image, v);
}

static VkSamplerAddressMode vk_wrap(uint32_t wrap) {
    switch (wrap) {
    case wire::REPEAT: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case wire::MIRRORED_REPEAT: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case wire::MIRROR_CLAMP_TO_EDGE:
        return gx2vk::caps().mirror_clamp_to_edge ? VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case wire::CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    // GX2 mirror-once border modes: GPU uses MIRROR_CLAMP_TO_BORDER_EXT, which
    // Vulkan lacks; clamp-to-border is identical for coordinates in [0, 1]
    // (Cemu's Vulkan mapping). Documented limitation.
    case wire::MIRROR_CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

// GPU sampler/texture parameters -> VkSamplerCreateInfo (custom border colors
// use the *_CUSTOM_EXT enum; tex_object_sampler supplies the value).
void gx2r_sampler_info(const TexParams& p, bool shadow, bool integer, VkSamplerCreateInfo& info) {
    info = VkSamplerCreateInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = integer ? VK_FILTER_NEAREST : gl_filter(p.mag_filter);
    info.minFilter = integer ? VK_FILTER_NEAREST : gl_filter(p.min_filter);
    const bool mipmapped = p.min_filter >= wire::NEAREST_MIPMAP_NEAREST && p.min_filter <= wire::LINEAR_MIPMAP_LINEAR;
    info.mipmapMode = (p.min_filter == wire::NEAREST_MIPMAP_LINEAR || p.min_filter == wire::LINEAR_MIPMAP_LINEAR) ?
                      VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = vk_wrap(p.wrap_s); info.addressModeV = vk_wrap(p.wrap_t); info.addressModeW = vk_wrap(p.wrap_r);
    const float max_bias = gx2vk::caps().limits.maxSamplerLodBias;
    info.mipLodBias = std::clamp(p.lod_bias, -max_bias, max_bias);
    info.anisotropyEnable = p.anisotropy > 1.0f && gx2vk::caps().anisotropy && !integer;
    info.maxAnisotropy = info.anisotropyEnable ? std::min(p.anisotropy, gx2vk::caps().max_anisotropy) : 1.0f;
    info.compareEnable = shadow;
    info.compareOp = shadow ? gl_compare_to_vk(p.compare_func) : VK_COMPARE_OP_NEVER;
    if (mipmapped) {
        info.minLod = std::max(-1000.0f, p.min_lod);
        info.maxLod = std::max(info.minLod, std::min(1000.0f, p.max_lod));
    } else {
        // GPU NEAREST/LINEAR minification samples the base level only.
        info.minLod = 0.0f; info.maxLod = 0.25f;
    }
    const auto& b = p.border;
    if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0) info.borderColor = integer ? VK_BORDER_COLOR_INT_TRANSPARENT_BLACK : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    else if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 1) info.borderColor = integer ? VK_BORDER_COLOR_INT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    else if (b[0] == 1 && b[1] == 1 && b[2] == 1 && b[3] == 1) info.borderColor = integer ? VK_BORDER_COLOR_INT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    else if (gx2vk::caps().custom_border_color) info.borderColor = integer ? VK_BORDER_COLOR_INT_CUSTOM_EXT : VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
    else info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
}

VkSampler gx2r::tex_object_sampler(const TexParams& p, bool shadow, bool integer) {
    VkSamplerCreateInfo info;
    gx2r_sampler_info(p, shadow, integer, info);
    if (info.borderColor == VK_BORDER_COLOR_FLOAT_CUSTOM_EXT || info.borderColor == VK_BORDER_COLOR_INT_CUSTOM_EXT) {
        VkClearColorValue color{};
        std::memcpy(color.float32, p.border.data(), 16);
        return gx2vk::sampler(info, &color);
    }
    return gx2vk::sampler(info);
}

// ---------------------------------------------------------------------------
// Programs
// ---------------------------------------------------------------------------
namespace {
bool async_shader_compile() {
    static const bool enabled = [] { const char* v = std::getenv("RBREW_ASYNC_SHADER_COMPILE"); return !(v && v[0] == '0'); }();
    return enabled;
}
std::unique_ptr<gx2vk::CompileQueue> s_compile_queue;
#ifdef RBREW_GX2_QUEUE_TESTS
std::function<void()> s_compile_test_gate;
uint64_t s_pixel_translation_posts = 0;
uint64_t s_exact_key_memo_hits = 0, s_last_exact_key = 0;
bool s_test_key_memo_enabled = true;
#endif
template<class F> auto post_compile(F&& fn, bool background = false) {
    using T = std::invoke_result_t<F>;
#ifdef RBREW_GX2_QUEUE_TESTS
    auto task_fn = [fn = std::forward<F>(fn), gate = s_compile_test_gate]() mutable -> T {
        if (gate) gate();
        return fn();
    };
#else
    auto task_fn = std::forward<F>(fn);
#endif
    if (async_shader_compile()) {
        if (!s_compile_queue) {
            const unsigned cores = std::thread::hardware_concurrency();
            s_compile_queue = std::make_unique<gx2vk::CompileQueue>(std::clamp(cores > 2 ? cores - 2 : 1u, 1u, 4u));
        }
        return s_compile_queue->post(std::move(task_fn), background);
    }
    std::packaged_task<T()> task(std::move(task_fn));
    gx2vk::CompileQueue::Job<T> job{task.get_future().share(), {}};
    task();
    return job;
}
template<class T> T await_compile(const gx2vk::CompileQueue::Job<T>& job) {
    if (job.ready()) return job.result.get();
    return s_compile_queue ? s_compile_queue->get(job) : job.result.get();
}
struct Program {
    uint32_t id = 0;
    std::array<R700TranslationInfo, 2> info;
    std::string vs_source, ps_source, gs_source;
    uint32_t rect = 0;               // 0 none, 1 RECT, 2 QUADS/QUAD_STRIP
    bool points = false;
    VkShaderModule vs = VK_NULL_HANDLE, gs = VK_NULL_HANDLE;
    gx2vk::CompileQueue::Job<bool> compiled;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    bool push = false;
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    bool valid = false;
    std::string ufblock_cache[2];      // last uploaded uniform-variable block per stage
    std::string ufblock_scratch[2];    // reusable packing storage; never GPU-resident
    uint64_t warm_key = 0;             // an exact program key that links to this program (boot warm-up)
    gx2vk::Span ufblock_span[2];
};
std::unordered_map<uint64_t, Program*> s_prog_cache;
std::unordered_map<uint64_t, std::unique_ptr<Program>> s_prog_by_source;
uint32_t s_next_program = 1;
unsigned s_draws_requested = 0, s_draws_no_program = 0, s_draws_no_shader_set = 0;
std::map<uint32_t, unsigned> s_draws_per_fbo;
uint32_t s_frame_draw_index = 0;
VkDescriptorPool s_descriptor_pool = VK_NULL_HANDLE;   // fallback when a set exceeds the push limit
uint64_t s_descriptor_pool_serial = 0;
std::vector<std::pair<uint64_t, VkDescriptorPool>> s_retired_pools;

GX2ShaderProgram active_shader_program(uint8_t* mem, bool pixel) {
    if (pixel ? S.ps_from_regs : S.vs_from_regs) {
        const uint32_t start = pixel ? 0xA210 : 0xA216;
        GX2ShaderProgram p{};
        p.address = S.regs[start] << 8; p.size = S.regs[start + 1] << 3;
        if (!p.address || !p.size || !rbrew_memory_accessible(mem, p.address, p.size)) return {};
        return p;
    }
    const uint32_t descriptor = pixel ? S.cur_ps : S.cur_vs;
    return descriptor ? gx2_shader_program({mem, WIIU_MEM_SIZE}, descriptor, pixel) : GX2ShaderProgram{};
}

void trace_shader(uint32_t addr, bool pixel, uint8_t* mem) {
    static const std::string configured = [] { const char* v = rbrew_diagnostic_env("RBREW_SHADER_DUMP_DIR"); return std::string(v ? v : ""); }();
    if (configured.empty()) return;
    static std::unordered_map<uint64_t, bool> seen;
    uint64_t key = (uint64_t(pixel) << 32) | addr;
    // Bounded, but high enough that the level's own materials are still dumped
    // after the boot/menu shaders (a frame trace is usually taken in-game).
    if (seen.size() >= 128 || !seen.emplace(key, true).second) return;
    auto p = gx2_shader_program({mem, WIIU_MEM_SIZE}, addr, pixel);
    RBREW_LOGF(stderr, "[shader] %s descriptor=%08X program=%08X bytes=%u mode=%u attributes=%u\n", pixel ? "PS" : "VS", addr, p.address, p.size, p.mode, p.attribute_count);
    if (!p) return;
    char path[1024];
    std::snprintf(path, sizeof(path), "%s/%s_%08X.bin", configured.c_str(), pixel ? "ps" : "vs", addr);
    if (FILE* f = std::fopen(path, "wb")) { std::fwrite(mem + p.address, 1, p.size, f); std::fclose(f); }
    std::snprintf(path, sizeof(path), "%s/%s_%08X.descriptor", configured.c_str(), pixel ? "ps" : "vs", addr);
    if (FILE* f = std::fopen(path, "wb")) { std::fwrite(mem + addr, 1, pixel ? 0xE8 : 0x134, f); std::fclose(f); }
}

// ProgramKeyHash (duplicate of the one in gx2_render_vk.cpp, same algorithm).
struct KeyHash {
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

void trace_program_key_miss(uint32_t vs, uint32_t ps, bool reused) {
#ifndef RBREW_DISABLE_LOGGING
    static const bool on = rbrew_diagnostic_env("RBREW_PROGRAM_KEY_TRACE") != nullptr;
    static unsigned lines;
    if (!on || lines >= 400) return;
    static std::map<std::pair<uint32_t, uint32_t>, std::vector<uint32_t>> previous;
    std::vector<uint32_t> now(S.regs.begin() + 0xA000, S.regs.begin() + 0xA400);
    for (unsigned base : {0xE000u, 0xE460u}) for (unsigned slot = 0; slot < 18; ++slot) { now.push_back(S.regs[base + slot * 7] & 7); now.push_back(S.regs[base + slot * 7 + 4]); }
    auto& last = previous[{vs, ps}];
    if (!last.empty()) {
        ++lines;
        RBREW_LOGF(stderr, "[keymiss] VS=%08X PS=%08X %s:", vs, ps, reused ? "reused" : "linked");
        unsigned shown = 0;
        for (size_t i = 0; i < now.size() && shown < 12; ++i) if (now[i] != last[i]) {
            ++shown;
            if (i < 0x400) RBREW_LOGF(stderr, " %04X:%08X->%08X", unsigned(0xA000 + i), last[i], now[i]);
            else RBREW_LOGF(stderr, " tex%zu:%08X->%08X", i - 0x400, last[i], now[i]);
        }
        RBREW_LOGF(stderr, "\n");
    }
    last = std::move(now);

#endif
}

bool create_program_layouts(Program& p) {
    const auto& caps = gx2vk::caps();
    // One descriptor set (set 0) holds both stages: the emitter's native
    // Vulkan mode gives each stage its own binding range.
    auto& bindings = p.bindings;
    bindings.clear();
    for (unsigned stage = 0; stage < 2; ++stage) {
        const auto& vk = p.info[stage].vk;
        const VkShaderStageFlags flags = stage == 0 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
        if (vk.uniform_vars_binding >= 0)
            bindings.push_back({uint32_t(vk.uniform_vars_binding), VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, flags, nullptr});
        for (unsigned b = 0; b < 16; ++b)
            if (vk.uniform_buffer_bindings[b] >= 0)
                bindings.push_back({uint32_t(vk.uniform_buffer_bindings[b]), VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, flags, nullptr});
        for (unsigned t = 0; t < 18; ++t)
            if (vk.texture_bindings[t] >= 0)
                bindings.push_back({uint32_t(vk.texture_bindings[t]), VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, flags, nullptr});
    }
    p.push = caps.push_descriptor && bindings.size() <= caps.max_push_descriptors;
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.flags = p.push ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0;
    info.bindingCount = uint32_t(bindings.size());
    info.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(gx2vk::device(), &info, nullptr, &p.set_layout) != VK_SUCCESS) return false;
    VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT, 0, 4};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &p.set_layout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &range;
    return vkCreatePipelineLayout(gx2vk::device(), &layout, nullptr, &p.layout) == VK_SUCCESS;
}

// Persistent translation cache. The in-memory program cache already treats
// `key` as the complete identity of a translation; this keeps those results
// across sessions so a scene does not pay for r700_to_glsl again. A stable
// directory uses compatibility/clip-space prefixes, independent of executable
// size, location or timestamp. Any unreadable entry is ignored.
static_assert(std::is_trivially_copyable_v<R700VulkanInterface> && std::is_trivially_copyable_v<R700UniformEntry>);
// Compatibility changes only when translation semantics, exact-key inputs or
// serialized metadata change. Ordinary game/renderer rebuilds reuse entries.
constexpr uint32_t kTranslationCompatibility=1;
std::string translation_cache_prefix() {
    return "v"+std::to_string(kTranslationCompatibility)+"_c"+std::to_string(unsigned(gx2vk::caps().negative_one_to_one))+"_";
}
// Read per lookup: the cache directory and clip-space capability belong to
// the current renderer initialisation, which can change within a process.
std::string translation_cache_dir() {
    static const bool disabled = std::getenv("RBREW_NO_TRANSLATION_CACHE") != nullptr;
    const std::string& base = gx2vk::shader_cache_dir();
    if (base.empty() || disabled) return {};
    const auto dir = (std::filesystem::path(base) / "translated").string();
    static std::set<std::string> created;
    if (created.insert(dir).second) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) { created.erase(dir); return {}; }
    }
    return dir;
}
std::string translation_cache_path(uint64_t key) {
    char name[64]; std::snprintf(name, sizeof(name), "%s%016llx.tl", translation_cache_prefix().c_str(), (unsigned long long)key);
    return (std::filesystem::path(translation_cache_dir()) / name).string();
}
constexpr uint32_t kTranslationMagic = 0x4C544252u, kTranslationVersion = 2;  // "RBTL"

// Owner request (use the RAM): the whole translation cache directory is read
// into memory on a background thread at startup. Lookups use the in-memory
// copy; once it is complete a key it lacks is not on disk either (entries this
// process stores are added to it), so a miss no longer probes the disk.
//
// Opening tens of thousands of small .tl files costs more than reading them
// (file-system and antivirus work per open), so the same bytes are also kept
// in one derived pack file, read sequentially. The .tl files stay the source
// of truth: the directory listing decides which keys exist, pack blobs are
// validated before use, and keys missing from the pack are read in parallel.
// The pack is rewritten in the background (low I/O priority) when it is
// missing or out of date by many entries. RBREW_TRANSLATION_PACK=0 ignores it.
//
// Memory: exact keys that differ only in inputs the translator ignores share
// one translation, so most blobs differ only in their stored key (owner cache
// 2026-10-17: 69,080 keys, 2,030 MiB, but 2,539 distinct bodies, 86 MiB).
// Entries therefore hold an interned body: the blob with its key field zeroed,
// shared by every key with the same bytes. A body is released once its key
// was loaded (its program is in s_prog_cache); a released key stays listed so
// a later lookup reads its file instead of concluding it is absent.
struct TranslationPreload {
    std::mutex mutex;
    std::string dir, prefix;
    std::unordered_map<uint64_t, std::shared_ptr<const std::vector<uint8_t>>> entries;
    std::unordered_map<uint64_t, std::vector<std::weak_ptr<const std::vector<uint8_t>>>> bodies;  // content hash -> interned bodies
    std::atomic<bool> done{false};
    std::atomic<bool> finished{false};             // the loading pass ended (done or over budget)
    std::atomic<uint64_t> expected{0}, loaded{0};  // progress of the loading pass
};
TranslationPreload& translation_preload() { static auto* preload = new TranslationPreload; return *preload; }  // lives until exit
// Version 2 stores each distinct body once: {magic, 2, keys, bodies}, then
// keys x {u64 key, u32 body}, bodies x {u32 size}, then the body bytes.
// Version 1 ({magic, 1, keys}, keys x {u64 key, u32 size}, whole blobs) is
// still read, and rewritten as version 2.
constexpr uint32_t kTranslationPackMagic = 0x50544252u, kTranslationPackVersion = 2;  // "RBTP"
bool translation_blob_valid(uint64_t key, const std::vector<uint8_t>& b) {
    if (b.size() < 20) return false;
    uint32_t head, version, tail; uint64_t stored;
    std::memcpy(&head, b.data(), 4); std::memcpy(&version, b.data() + 4, 4);
    std::memcpy(&stored, b.data() + 8, 8); std::memcpy(&tail, b.data() + b.size() - 4, 4);
    return head == kTranslationMagic && version == kTranslationVersion && stored == key && tail == kTranslationMagic;
}
// A blob's identity apart from its key field (bytes 8..15).
uint64_t translation_body_hash(const std::vector<uint8_t>& b) {
    KeyHash h;
    h.add(b.data(), std::min<size_t>(b.size(), 8));
    if (b.size() > 16) h.add(b.data() + 16, b.size() - 16);
    return h.finish();
}
// Caller holds preload.mutex. `blob` is a valid blob of any key; returns the
// shared body with the same bytes, interning `blob` (key zeroed) if new.
std::shared_ptr<const std::vector<uint8_t>> intern_translation_body(TranslationPreload& preload, std::vector<uint8_t>&& blob,
                                                                    uint64_t hash, bool* added = nullptr) {
    std::memset(blob.data() + 8, 0, 8);
    auto& candidates = preload.bodies[hash];
    for (auto it = candidates.begin(); it != candidates.end();) {
        auto body = it->lock();
        if (!body) { it = candidates.erase(it); continue; }
        if (*body == blob) { if (added) *added = false; return body; }
        ++it;
    }
    auto body = std::make_shared<const std::vector<uint8_t>>(std::move(blob));
    candidates.push_back(body);
    if (added) *added = true;
    return body;
}
// The blob of `key` as stored on disk, from its interned body.
std::vector<uint8_t> translation_blob(uint64_t key, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> blob = body;
    if (blob.size() >= 16) std::memcpy(blob.data() + 8, &key, 8);
    return blob;
}
bool translation_pack_enabled() {
    static const bool enabled = [] { const char* v = std::getenv("RBREW_TRANSLATION_PACK"); return !(v && v[0] == '0'); }();
    return enabled;
}
std::shared_ptr<std::vector<uint8_t>> read_whole_file(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return nullptr;
    auto data = std::make_shared<std::vector<uint8_t>>();
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size > 0) { data->resize(size_t(size)); if (std::fread(data->data(), 1, data->size(), f) != data->size()) data->clear(); }
    std::fclose(f);
    return data->empty() ? nullptr : data;
}
// Sorted (key, body) pairs of the valid entries. The loading pass takes it
// before it reports completion, so the warm-up cannot release bodies first.
using TranslationSnapshot = std::vector<std::pair<uint64_t, std::shared_ptr<const std::vector<uint8_t>>>>;
TranslationSnapshot translation_snapshot(TranslationPreload& preload) {
    TranslationSnapshot snapshot;
    {
        std::lock_guard lock(preload.mutex);
        snapshot.reserve(preload.entries.size());
        for (const auto& [key, body] : preload.entries)
            if (body && translation_blob_valid(0, *body)) snapshot.emplace_back(key, body);
    }
    std::sort(snapshot.begin(), snapshot.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return snapshot;
}
// Written to a temporary file and renamed over the pack; each interned body
// once, in order of its first key.
bool write_translation_pack(const TranslationSnapshot& snapshot, const std::string& path) {
    std::unordered_map<const std::vector<uint8_t>*, uint32_t> body_ids;
    std::vector<const std::vector<uint8_t>*> bodies;
    std::vector<uint32_t> index;
    index.reserve(snapshot.size());
    uint64_t body_bytes = 0;
    for (const auto& [key, body] : snapshot) {
        const auto [it, added] = body_ids.try_emplace(body.get(), uint32_t(bodies.size()));
        if (added) { bodies.push_back(body.get()); body_bytes += body->size(); }
        index.push_back(it->second);
    }
    const std::string temporary = path + ".tmp";
    std::FILE* f = std::fopen(temporary.c_str(), "wb");
    if (!f) return false;
    std::setvbuf(f, nullptr, _IOFBF, 8u << 20);
    const uint32_t header[4] = {kTranslationPackMagic, kTranslationPackVersion, uint32_t(snapshot.size()), uint32_t(bodies.size())};
    bool ok = std::fwrite(header, 4, 4, f) == 4;
    for (size_t i = 0; i < snapshot.size(); ++i)
        ok = ok && std::fwrite(&snapshot[i].first, 8, 1, f) == 1 && std::fwrite(&index[i], 4, 1, f) == 1;
    for (const auto* body : bodies) { const uint32_t size = uint32_t(body->size()); ok = ok && std::fwrite(&size, 4, 1, f) == 1; }
    for (const auto* body : bodies) ok = ok && std::fwrite(body->data(), 1, body->size(), f) == body->size();
    ok = std::fclose(f) == 0 && ok;
    std::error_code ec;
    if (ok) std::filesystem::rename(temporary, path, ec);
    if (!ok || ec) std::filesystem::remove(temporary, ec);
    RBREW_LOGF(stderr, "[shader] translation pack %s: %zu entries, %zu bodies (%llu MB)\n", ok && !ec ? "written" : "write failed",
               snapshot.size(), bodies.size(), (unsigned long long)(body_bytes >> 20));
    return ok && !ec;
}
struct PreloadStats { size_t on_disk = 0, from_pack = 0, read = 0, stale = 0; bool complete = false, pack_written = false; };
PreloadStats run_translation_preload(TranslationPreload& preload, const std::string& dir, const std::string& prefix) {
    RBREW_LOGF(stderr, "[shader] translation preload started: %s\n", dir.c_str());
    const auto start = std::chrono::steady_clock::now();
    const uint64_t budget = gx2_host_total_ram() ? gx2_host_total_ram() / 8 : (1ull << 30);
    std::atomic<uint64_t> bytes{0};     // held: distinct bodies only
    std::atomic<size_t> distinct{0};
    // The listing is the set of keys that exist (deleted files stay deleted).
    std::unordered_map<uint64_t, std::string> pending;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const auto& path = it->path();
        const std::string name = path.filename().string();
        if (name.size() != prefix.size()+19 || !name.starts_with(prefix) || path.extension() != ".tl") continue;
        pending.emplace(std::strtoull(name.substr(prefix.size(), 16).c_str(), nullptr, 16), path.string());
    }
    preload.expected = pending.size();
    const size_t on_disk = pending.size();
    // A valid blob (any key field), held once per distinct body.
    auto add = [&](uint64_t key, std::vector<uint8_t>&& blob) {
        const uint64_t hash = translation_body_hash(blob);
        bool added = false;
        std::lock_guard lock(preload.mutex);
        auto body = intern_translation_body(preload, std::move(blob), hash, &added);
        if (added) { bytes += body->size(); ++distinct; }
        preload.entries.try_emplace(key, std::move(body));
    };
    // 1. The pack: one sequential read.
    const std::string pack = (std::filesystem::path(dir) / (prefix + "pack.bin")).string();
    size_t from_pack = 0, stale = 0;
    bool have_pack = false;
    uint32_t pack_version = 0;
    if (translation_pack_enabled()) if (std::FILE* f = std::fopen(pack.c_str(), "rb")) {
        std::setvbuf(f, nullptr, _IOFBF, 8u << 20);
        uint32_t header[3]{};
        if (std::fread(header, 4, 3, f) == 3 && header[0] == kTranslationPackMagic && (header[1] == 1 || header[1] == 2) &&
            header[2] <= (1u << 24)) {
            pack_version = header[1];
            uint32_t body_count = 0;
            bool ok = pack_version == 1 || (std::fread(&body_count, 4, 1, f) == 1 && body_count <= header[2]);
            // Version 1: {key, blob size}; version 2: {key, body index}.
            std::vector<std::pair<uint64_t, uint32_t>> index(ok ? header[2] : 0);
            for (auto& [key, value] : index)
                ok = ok && std::fread(&key, 8, 1, f) == 1 && std::fread(&value, 4, 1, f) == 1 &&
                     (pack_version == 1 ? value <= (64u << 20) : value < body_count);
            if (pack_version == 1) {
                have_pack = ok;
                for (const auto& [key, size] : index) {
                    if (!ok || bytes > budget) break;
                    std::vector<uint8_t> data(size);
                    if (std::fread(data.data(), 1, size, f) != size) { ok = false; break; }
                    const auto it = pending.find(key);
                    if (it == pending.end() || !translation_blob_valid(key, data)) { ++stale; continue; }
                    pending.erase(it);
                    add(key, std::move(data));
                    ++from_pack; ++preload.loaded;
                }
            } else {
                std::vector<uint32_t> sizes(ok ? body_count : 0);
                for (auto& size : sizes) ok = ok && std::fread(&size, 4, 1, f) == 1 && size <= (64u << 20);
                have_pack = ok;
                std::vector<std::vector<uint64_t>> users(ok ? sizes.size() : 0);
                for (const auto& [key, body] : index) if (ok) users[body].push_back(key);
                for (uint32_t b = 0; ok && b < sizes.size(); ++b) {
                    if (bytes > budget) break;
                    std::vector<uint8_t> data(sizes[b]);
                    if (std::fread(data.data(), 1, data.size(), f) != data.size()) { ok = false; break; }
                    const bool valid = translation_blob_valid(0, data);
                    std::shared_ptr<const std::vector<uint8_t>> body;
                    for (const uint64_t key : users[b]) {
                        const auto it = pending.find(key);
                        if (it == pending.end() || !valid) { ++stale; continue; }
                        pending.erase(it);
                        if (!body) {
                            add(key, std::move(data));
                            std::lock_guard lock(preload.mutex);
                            body = preload.entries[key];
                        } else {
                            std::lock_guard lock(preload.mutex);
                            preload.entries.try_emplace(key, body);
                        }
                        ++from_pack; ++preload.loaded;
                    }
                }
            }
        }
        std::fclose(f);
    }
    // 2. Files the pack lacks, in parallel.
    std::vector<std::pair<uint64_t, std::string>> files(pending.begin(), pending.end());
    std::atomic<size_t> next{0};
    std::atomic<bool> over_budget{bytes > budget};
    const unsigned workers = std::clamp(std::thread::hardware_concurrency() / 2, 2u, 8u);
    std::vector<std::thread> pool;
    for (unsigned w = 0; w < workers && !files.empty(); ++w) pool.emplace_back([&] {
        for (size_t i; !over_budget && (i = next++) < files.size();) {
            // An invalid file stays out: a lookup then treats the key as
            // absent, the result its failed parse would have had.
            if (auto data = read_whole_file(files[i].second); data && translation_blob_valid(files[i].first, *data))
                add(files[i].first, std::move(*data));
            ++preload.loaded;
            if (bytes > budget) over_budget = true;
        }
    });
    for (auto& t : pool) t.join();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (over_budget) {   // too large to hold: lookups keep probing the disk
        RBREW_LOGF(stderr, "[shader] translation preload stopped at %llu MB budget\n", (unsigned long long)(budget >> 20));
        return {on_disk, from_pack, files.size(), stale, false, false};
    }
    // 3. Refresh the pack when that saves many opens next time (or it is an
    // older version).
    const size_t slack = std::max<size_t>(1000, on_disk / 20);
    const bool write_pack = translation_pack_enabled() &&
                            (!have_pack || pack_version != kTranslationPackVersion || files.size() > slack || stale > slack);
    TranslationSnapshot snapshot;
    if (write_pack) snapshot = translation_snapshot(preload);
    preload.done.store(true, std::memory_order_release);
    preload.finished = true; preload.finished.notify_all();
    RBREW_LOGF(stderr, "[shader] translation preload: %zu files (%zu from pack, %zu read), %zu distinct, %llu MB in %.2f s\n", on_disk,
               from_pack, files.size(), distinct.load(), (unsigned long long)(bytes >> 20), seconds);
    bool pack_written = false;
    if (write_pack) {
#if defined(_WIN32)
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
#endif
        pack_written = write_translation_pack(snapshot, pack);
        snapshot = {};   // bodies the warm-up already released are freed here
#if defined(_WIN32)
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
#endif
    }
    return {on_disk, from_pack, files.size(), stale, true, pack_written};
}
void start_translation_preload(const std::string& dir) {
    static std::once_flag once;
    std::call_once(once, [&] {
        static const bool disabled = [] { const char* v = std::getenv("RBREW_TRANSLATION_PRELOAD"); return v && v[0] == '0'; }();
        if (disabled) return;
        auto& preload = translation_preload();
        const std::string prefix=translation_cache_prefix();
        preload.dir = dir;preload.prefix=prefix;
        std::thread([&preload, dir, prefix] {
            try { run_translation_preload(preload, dir, prefix); }
            catch (...) {}
            preload.finished = true; preload.finished.notify_all();
        }).detach();
    });
}
// Blocks until the loading pass of `dir` ended (no-op when none runs), calling
// `progress(loaded fraction)` about every 50 ms.
void wait_translation_preload(const std::string& dir, void (*progress)(double)) {
    auto& preload = translation_preload();
    if (dir.empty() || preload.dir != dir) return;
    while (!preload.finished.load(std::memory_order_acquire)) {
        if (progress) {
            const uint64_t expected = preload.expected.load(), loaded = preload.loaded.load();
            progress(expected ? double(std::min(loaded, expected)) / double(expected) : 0.0);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(progress ? 50 : 2));
    }
}
// The preloaded bytes of `key` (null if absent); `absent` is set when the
// complete preload proves the key is not cached.
std::shared_ptr<const std::vector<uint8_t>> preloaded_translation(const std::string& dir, uint64_t key, bool& absent) {
    absent = false;
    auto& preload = translation_preload();
    if (preload.dir != dir || preload.prefix != translation_cache_prefix()) return nullptr;
    const bool done = preload.done.load(std::memory_order_acquire);
    std::lock_guard lock(preload.mutex);
    if (auto it = preload.entries.find(key); it != preload.entries.end()) return it->second;
    absent = done;
    return nullptr;
}
void remember_translation(const std::string& dir, uint64_t key, const std::vector<uint8_t>& bytes) {
    auto& preload = translation_preload();
    if (preload.dir != dir || preload.prefix != translation_cache_prefix()) return;
    const uint64_t hash = translation_body_hash(bytes);
    std::lock_guard lock(preload.mutex);
    if (preload.entries.count(key)) return;
    preload.entries.emplace(key, intern_translation_body(preload, std::vector<uint8_t>(bytes), hash));
}
// The program of `key` is linked and cached: its bytes are not needed again.
// The key stays listed (null), so a lookup reads its file, never "absent".
void release_preloaded_translation(const std::string& dir, uint64_t key) {
    auto& preload = translation_preload();
    if (preload.dir != dir || preload.prefix != translation_cache_prefix()) return;
    std::shared_ptr<const std::vector<uint8_t>> released;   // freed outside the lock
    std::lock_guard lock(preload.mutex);
    if (auto it = preload.entries.find(key); it != preload.entries.end()) released = std::move(it->second);
}

bool load_translation(uint64_t key, char*& vs, char*& ps, std::array<R700TranslationInfo, 2>& info) {
    const std::string dir = translation_cache_dir();
    if (dir.empty()) return false;
    start_translation_preload(dir);
    bool absent = false;
    std::shared_ptr<const std::vector<uint8_t>> bytes = preloaded_translation(dir, key, absent);
    if (absent) return false;
    const bool preloaded = bytes != nullptr;   // an interned body: its key field is zero
    if (!bytes) {
        std::FILE* f = std::fopen(translation_cache_path(key).c_str(), "rb");
        if (!f) return false;
        auto data = std::make_shared<std::vector<uint8_t>>();
        uint8_t chunk[65536];
        for (size_t n; (n = std::fread(chunk, 1, sizeof(chunk), f)) > 0;) data->insert(data->end(), chunk, chunk + n);
        std::fclose(f);
        bytes = std::move(data);
    }
    size_t position = 0;
    bool ok = true;
    auto read = [&](void* p, size_t n) {
        if (!ok || !n) return;
        if (position + n > bytes->size()) { ok = false; return; }
        std::memcpy(p, bytes->data() + position, n); position += n;
    };
    auto read_u32 = [&] { uint32_t v = 0; read(&v, 4); return v; };
    auto read_string = [&]() -> char* {
        const uint32_t n = read_u32();
        if (!ok || n > (64u << 20)) { ok = false; return nullptr; }
        char* s = static_cast<char*>(std::malloc(size_t(n) + 1));
        if (!s) { ok = false; return nullptr; }
        read(s, n); s[n] = 0; return s;
    };
    uint64_t stored_key = 0;
    if (read_u32() != kTranslationMagic || read_u32() != kTranslationVersion) ok = false;
    read(&stored_key, 8);
    if (stored_key != (preloaded ? 0 : key)) ok = false;
    char* v = ok ? read_string() : nullptr;
    char* p = ok ? read_string() : nullptr;
    std::array<R700TranslationInfo, 2> loaded;
    for (auto& i : loaded) {
        if (!ok) break;
        const uint32_t remapped = read_u32();
        if (remapped > 65536) { ok = false; break; }
        i.remapped.resize(remapped);
        read(i.remapped.data(), remapped * sizeof(R700UniformEntry));
        read(i.attribute_locations.data(), sizeof(i.attribute_locations));
        read(i.buffer_bindings.data(), sizeof(i.buffer_bindings));
        i.uniform_mode = read_u32(); i.register_count = read_u32();
        read(i.texture_compare.data(), sizeof(i.texture_compare));
        i.streamout_mask = read_u32();
        read(i.streamout_strides.data(), sizeof(i.streamout_strides));
        for (auto& words : i.streamout_written_words) {
            const uint32_t n = read_u32();
            if (n > (1u << 20)) { ok = false; break; }
            words.resize(n); read(words.data(), n);
        }
        read(&i.vk, sizeof(i.vk));
    }
    uint32_t end = read_u32();
    if (end != kTranslationMagic) ok = false;
    if (!ok) { std::free(v); std::free(p); return false; }
    vs = v; ps = p; info = std::move(loaded);
    // Every caller links the result and caches the program under this key.
    if (preloaded) release_preloaded_translation(dir, key);
    return true;
}

// Disk writes run on a background thread: a scene can produce thousands of
// new translation keys, and writing them on the render thread stalled it.
// Each file is written to a temporary name and renamed, so an exit with
// writes still queued never leaves a partial entry.
struct TranslationWriter {
    std::mutex mutex;
    std::condition_variable ready, idle;
    std::deque<std::pair<std::string, std::vector<uint8_t>>> queue;
    bool writing = false;
    TranslationWriter() {
        std::thread([this] {
            for (;;) {
                std::pair<std::string, std::vector<uint8_t>> job;
                {
                    std::unique_lock lock(mutex);
                    writing = false;
                    idle.notify_all();
                    ready.wait(lock, [this] { return !queue.empty(); });
                    job = std::move(queue.front());
                    queue.pop_front();
                    writing = true;
                }
                const std::string temp = job.first + ".tmp";
                std::FILE* f = std::fopen(temp.c_str(), "wb");
                if (!f) continue;
                bool ok = std::fwrite(job.second.data(), 1, job.second.size(), f) == job.second.size();
                ok = std::fclose(f) == 0 && ok;
                std::error_code ec;
                if (ok) std::filesystem::rename(temp, job.first, ec);
                if (!ok || ec) std::filesystem::remove(temp, ec);
            }
        }).detach();
    }
};
TranslationWriter& translation_writer() { static auto* writer = new TranslationWriter; return *writer; }  // lives until process exit
void translation_writer_post(std::string path, std::vector<uint8_t> bytes) {
    auto* writer = &translation_writer();
    {
        std::lock_guard lock(writer->mutex);
        if (writer->queue.size() >= (1u << 16)) return;  // owner request: keep (was 4096) - a backlog only costs RAM
        writer->queue.emplace_back(std::move(path), std::move(bytes));
    }
    writer->ready.notify_one();
}

void store_translation(uint64_t key, const char* vs, const char* ps, const std::array<R700TranslationInfo, 2>& info) {
    if (!vs || !ps) return;
    const std::string dir = translation_cache_dir();
    if (dir.empty()) return;
    std::vector<uint8_t> out;
    auto write = [&](const void* p, size_t n) { const auto* b = static_cast<const uint8_t*>(p); out.insert(out.end(), b, b + n); };
    auto write_u32 = [&](uint32_t v) { write(&v, 4); };
    auto write_string = [&](const char* s) { const uint32_t n = uint32_t(std::strlen(s)); write_u32(n); write(s, n); };
    write_u32(kTranslationMagic); write_u32(kTranslationVersion); write(&key, 8);
    write_string(vs); write_string(ps);
    for (const auto& i : info) {
        write_u32(uint32_t(i.remapped.size()));
        write(i.remapped.data(), i.remapped.size() * sizeof(R700UniformEntry));
        write(i.attribute_locations.data(), sizeof(i.attribute_locations));
        write(i.buffer_bindings.data(), sizeof(i.buffer_bindings));
        write_u32(i.uniform_mode); write_u32(i.register_count);
        write(i.texture_compare.data(), sizeof(i.texture_compare));
        write_u32(i.streamout_mask);
        write(i.streamout_strides.data(), sizeof(i.streamout_strides));
        for (const auto& words : i.streamout_written_words) { write_u32(uint32_t(words.size())); write(words.data(), words.size()); }
        write(&i.vk, sizeof(i.vk));
    }
    write_u32(kTranslationMagic);
    remember_translation(dir, key, out);
    translation_writer_post(translation_cache_path(key), std::move(out));
}

// Parallel translation (RBREW_PARALLEL_TRANSLATE, default on; "0" disables).
// A program-key miss translates both stages. The pixel stage is handed to one
// helper thread while this thread probes the disk cache and translates the
// vertex stage. r700_to_glsl is a pure function of its inputs (all of its
// translator state is per thread), and the job owns copies of exactly what
// the serial call reads - the bytecode and the 0x10000 context words - taken
// at the same point of the draw stream, so its result is the serial result.
// Results are used in the serial order; a disk hit discards the job's result
// like the serial path never ran it. Nothing else about the draw changes.
// "verify" also runs the serial pixel translation, logs any difference and
// uses the serial result (diagnostic; no speedup).
bool parallel_translation() {
    static const bool on = [] {
        const char* value = std::getenv("RBREW_PARALLEL_TRANSLATE");
        return !(value && value[0] == '0');
    }();
    return on;
}
bool parallel_translation_verify() {
#ifdef RBREW_DISABLE_LOGGING
    return false;
#else
    static const bool on = [] {
        const char* value = std::getenv("RBREW_PARALLEL_TRANSLATE");
        return value && std::strcmp(value, "verify") == 0;
    }();
    return on;
#endif
}
bool same_translation(const char* a, const R700TranslationInfo& ia, const char* b, const R700TranslationInfo& ib) {
    if (!a || !b) return a == b;
    return std::strcmp(a, b) == 0 && ia.remapped.size() == ib.remapped.size() &&
           std::memcmp(ia.remapped.data(), ib.remapped.data(), ia.remapped.size() * sizeof(R700UniformEntry)) == 0 &&
           ia.attribute_locations == ib.attribute_locations && ia.buffer_bindings == ib.buffer_bindings &&
           ia.uniform_mode == ib.uniform_mode && ia.register_count == ib.register_count && ia.texture_compare == ib.texture_compare &&
           ia.streamout_mask == ib.streamout_mask && ia.streamout_strides == ib.streamout_strides &&
           ia.streamout_written_words == ib.streamout_written_words && std::memcmp(&ia.vk, &ib.vk, sizeof(ia.vk)) == 0;
}

struct TranslateJob {
    std::vector<uint8_t> code;
    std::vector<uint32_t> regs;
    char* glsl = nullptr;
    R700TranslationInfo info;
    R700TranslationReads reads;
    std::exception_ptr error;
    std::atomic<uint32_t> done{0};
    ~TranslateJob() { std::free(glsl); }
    void run() {
        try {
            // The serial pixel call: no attributes, default clip convention.
            R700TranslateInput ti{};
            ti.data = code.data(); ti.size = uint32_t(code.size()); ti.is_pixel = true;
            ti.context_registers = regs.data(); ti.info = &info; ti.vulkan = true;
            glsl = r700_to_glsl(ti);
            reads = r700_last_translation_reads();
        } catch (...) { error = std::current_exception(); }
        done.store(1, std::memory_order_release);
        done.notify_all();
    }
    // Waits for the result and hands it over (the caller frees the source).
    char* take(R700TranslationInfo& out, R700TranslationReads& out_reads) {
        if (!done.load(std::memory_order_acquire)) {
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
            for (unsigned n = 1; !done.load(std::memory_order_acquire); ++n) {
                if (n % 64 == 0 && std::chrono::steady_clock::now() >= until) {
                    while (!done.load(std::memory_order_acquire)) done.wait(0, std::memory_order_acquire);
                    break;
                }
                _mm_pause();
            }
        }
        if (error) std::rethrow_exception(error);
        out = std::move(info);
        out_reads = reads;
        return std::exchange(glsl, nullptr);
    }
};

// One helper: the render thread waits for each miss, so misses never overlap.
class TranslationHelper {
public:
    static TranslationHelper& get() { static auto* helper = new TranslationHelper; return *helper; }  // lives until exit
    void post(std::shared_ptr<TranslateJob> job) {
        {
            std::lock_guard lock(mutex_);
            queue_.push_back(std::move(job));
            posted_.fetch_add(1, std::memory_order_release);
        }
        ready_.notify_one();
    }
private:
    TranslationHelper() { std::thread([this] { loop(); }).detach(); }
    void loop() {
#if defined(_WIN32)
        // The render thread waits on this result (same priority as it).
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
        for (;;) {
            std::shared_ptr<TranslateJob> job;
            uint32_t seen;
            {
                std::unique_lock lock(mutex_);
                if (!queue_.empty()) { job = std::move(queue_.front()); queue_.pop_front(); }
                seen = posted_.load(std::memory_order_relaxed);
            }
            if (job) { job->run(); continue; }
            // Misses come in bursts: stay awake briefly before sleeping.
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
            bool posted = false;
            for (unsigned n = 1; !posted; ++n) {
                if (posted_.load(std::memory_order_acquire) != seen) posted = true;
                else if (n % 64 == 0 && std::chrono::steady_clock::now() >= until) break;
                else _mm_pause();
            }
            if (!posted) {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [this] { return !queue_.empty(); });
            }
        }
    }
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::shared_ptr<TranslateJob>> queue_;
    std::atomic<uint32_t> posted_{0};
};

std::shared_ptr<TranslateJob> post_pixel_translation(const uint8_t* code, uint32_t size) {
#ifdef RBREW_GX2_QUEUE_TESTS
    ++s_pixel_translation_posts;
#endif
    auto job = std::make_shared<TranslateJob>();
    job->code.assign(code, code + size);
    job->regs.assign(S.regs.begin(), S.regs.end());
    TranslationHelper::get().post(job);
    return job;
}

// Context words (offsets from 0xA000) the program key hashes as zero.
constexpr uint16_t kDynamicKeyRegisters[] = {
    0x090, 0x091,                              // PA_SC_GENERIC_SCISSOR_TL/BR
    0x10F, 0x110, 0x111, 0x112, 0x113, 0x114}; // PA_CL_VPORT_X/Y/Z SCALE/OFFSET
// RBREW_SHADER_KEY_DYNAMIC=0 keys them again (previous full key).
bool dynamic_key_registers_excluded() {
    static const bool on = [] { const char* v = std::getenv("RBREW_SHADER_KEY_DYNAMIC"); return !(v && v[0] == '0'); }();
    return on;
}
// Exact program key (RBREW_SHADER_KEY_EXACT, default on; "0" restores the
// previous key): only what the translator reads (gx2_shader_key.h).
constexpr unsigned kExactKeyRegisterCount = gx2_translator_register_count();
constexpr uint32_t kExactKeyTag = 0x45584B33u;  // "EXK3": disk entries never alias another key layout
// Index of register r in the kGx2TranslatorRegisters word list.
constexpr unsigned kExactWordIndex(uint32_t r) {
    unsigned i = 0;
    for (const auto& range : kGx2TranslatorRegisters) {
        if (r >= range.first && r <= range.last) return i + (r - range.first);
        i += range.last - range.first + 1u;
    }
    return ~0u;
}
static_assert(kExactWordIndex(0xA18E) - kExactWordIndex(0xA185) == 9 && kExactWordIndex(0xA1B0) - kExactWordIndex(0xA191) == 31);
// Per exact base key: the VS parameters and texture units the translation
// read (r700_translation_reads.h). Until a translation with this base has
// been seen here, every such word stays in the key.
struct ExactReads { uint32_t vs_parameter_mask, vs_texture_mask, ps_texture_mask; };
std::unordered_map<uint64_t, ExactReads> s_exact_reads;

// Reuse the last exact key only after comparing every input the existing
// algorithm hashes. Code bytes stay live: no address-only or write-watch
// shortcut, and no new disk identity. Read masks only grow through emplace;
// their count invalidates a key computed before a mask was discovered.
struct ExactKeyInputs {
    std::array<uint32_t, kExactKeyRegisterCount> words{};
    std::array<uint32_t, 36> texture_bits{};
    uint64_t fetch_hash = 0;
    uint32_t rect = 0, points = 0, vs_attribs = 0;
    bool operator==(const ExactKeyInputs&) const = default;
};
struct ExactKeyMemo {
    ExactKeyInputs inputs;
    std::vector<uint8_t> vs, ps;
    size_t masks = 0;
    uint64_t key = 0, base = 0, canon = 0;
    bool valid = false;
    bool matches(const ExactKeyInputs& current, const uint8_t* vertex, size_t vertex_size,
                 const uint8_t* pixel, size_t pixel_size) const {
        return valid && masks == s_exact_reads.size() && inputs == current &&
            vs.size() == vertex_size && ps.size() == pixel_size &&
            std::memcmp(vs.data(), vertex, vertex_size) == 0 && std::memcmp(ps.data(), pixel, pixel_size) == 0;
    }
    void capture(const ExactKeyInputs& current, const uint8_t* vertex, size_t vertex_size,
                 const uint8_t* pixel, size_t pixel_size) {
        valid = false;
        vs.assign(vertex, vertex + vertex_size); ps.assign(pixel, pixel + pixel_size);
        inputs = current; masks = s_exact_reads.size();
    }
};
ExactKeyMemo s_exact_key_memo;

// Canonical translation identity: the exact key's inputs with every register
// word reduced to the bits the translator reads (gx2_translator_register_bits)
// and the fetch layout to the attribute fields it reads (semantic, format,
// endian, destination select, offset & 3, buffer >= 16, order). Exact keys
// with one canonical key translate to identical output: entering a new area
// produced ~6,400 exact-key misses for ~520 distinct translations (owner
// cache 2026-10-17), each translated synchronously on the render thread.
// A new exact key whose canonical key is known reuses that program and
// stores its translation under the new key (disk cache and boot warm-up
// stay keyed exactly as before). RBREW_SHADER_KEY_CANONICAL=0 disables.
constexpr uint32_t kCanonicalKeyTag = 0x43414E31u;  // "CAN1"
constexpr std::array<uint32_t, kExactKeyRegisterCount> kCanonicalWordBits = [] {
    std::array<uint32_t, kExactKeyRegisterCount> bits{};
    unsigned i = 0;
    for (const auto& range : kGx2TranslatorRegisters)
        for (uint32_t r = range.first; r <= range.last; ++r) bits[i++] = gx2_translator_register_bits(r);
    return bits;
}();
struct CanonicalEntry { Program* program = nullptr; ExactReads reads{}; bool reads_valid = false; };
std::unordered_map<uint64_t, CanonicalEntry> s_canonical;
uint64_t s_canonical_reuses = 0;
bool canonical_key_enabled() {
    static const bool on = [] { const char* v = std::getenv("RBREW_SHADER_KEY_CANONICAL"); return !(v && v[0] == '0'); }();
    return on;
}
bool exact_key_memo_enabled() {
    static const bool enabled = [] { const char* v = std::getenv("RBREW_SHADER_KEY_MEMO"); return !(v && v[0] == '0'); }();
#ifdef RBREW_GX2_QUEUE_TESTS
    return enabled && s_test_key_memo_enabled;
#else
    return enabled;
#endif
}
// Miss diagnostics: which key part changed since the last miss of the same
// VS/PS pair, logged every 1000 misses (misses only; no effect on the key).
void note_exact_key_miss(uint32_t vs, uint32_t ps, const uint8_t* mem, uint32_t vs_size, uint32_t ps_size,
                         const uint32_t* words, const uint32_t* texture_bits, uint64_t fetch_hash, uint32_t misc) {
#ifndef RBREW_DISABLE_LOGGING
    struct Last { uint64_t code = 0, fetch = 0; uint32_t misc = 0; std::array<uint32_t, kExactKeyRegisterCount> words{}; std::array<uint32_t, 36> tex{}; };
    static std::map<std::pair<uint32_t, uint32_t>, Last> last;
    static uint64_t misses = 0, first = 0, code = 0, regs = 0, tex = 0, fetch = 0, other = 0;
    static std::map<uint32_t, uint64_t> by_register;
    KeyHash c; c.add(mem + vs, vs_size); c.add(mem + ps, ps_size);
    const uint64_t code_hash = c.finish();
    auto [entry, inserted] = last.try_emplace({vs, ps});
    Last& l = entry->second;
    ++misses;
    if (inserted) ++first;
    else {
        if (l.code != code_hash) ++code;
        if (l.fetch != fetch_hash) ++fetch;
        if (l.misc != misc) ++other;
        if (std::memcmp(l.tex.data(), texture_bits, sizeof(l.tex)) != 0) ++tex;
        bool reg_changed = false;
        unsigned i = 0;
        for (const auto& range : kGx2TranslatorRegisters) for (uint32_t r = range.first; r <= range.last; ++r, ++i)
            if (l.words[i] != words[i]) { reg_changed = true; ++by_register[r]; }
        if (reg_changed) ++regs;
    }
    l.code = code_hash; l.fetch = fetch_hash; l.misc = misc;
    std::copy_n(words, kExactKeyRegisterCount, l.words.data());
    std::copy_n(texture_bits, 36, l.tex.data());
    if (misses % 1000 == 0) {
        std::vector<std::pair<uint64_t, uint32_t>> top;
        for (const auto& [r, n] : by_register) top.push_back({n, r});
        std::sort(top.rbegin(), top.rend());
        RBREW_LOGF(stderr, "[shader] key misses=%llu new_pair=%llu changed: code=%llu regs=%llu tex=%llu fetch=%llu other=%llu | top regs:",
                     (unsigned long long)misses, (unsigned long long)first, (unsigned long long)code, (unsigned long long)regs,
                     (unsigned long long)tex, (unsigned long long)fetch, (unsigned long long)other);
        for (size_t k = 0; k < top.size() && k < 8; ++k) RBREW_LOGF(stderr, " %04X:%llu", top[k].second, (unsigned long long)top[k].first);
        RBREW_LOGF(stderr, "\n");
    }

#endif
}
bool exact_shader_key() {
    static const bool on = [] { const char* v = std::getenv("RBREW_SHADER_KEY_EXACT"); return !(v && v[0] == '0'); }();
    return on;
}
bool shader_key_verify() {
    static const bool on = [] { const char* v = rbrew_diagnostic_env("RBREW_SHADER_KEY_VERIFY"); return v && v[0] == '1'; }();
    return on;
}

// Boot warm-up (defined after the pipeline code).
struct ExactReads;
Program* link_program(const char* vs_glsl, const char* ps_glsl, const std::array<R700TranslationInfo, 2>& info,
                      uint32_t rect, uint32_t points, bool count, bool& reused, bool background = false);
void warm_start();
// Startup splash: warm-up progress (0..1) while it runs ahead of the first frame.
void (*s_warm_progress)(double) = nullptr;
void warm_record_program(uint64_t key, uint32_t rect, uint32_t points);
void warm_record_masks(uint64_t base, const ExactReads& reads);

Program* resolve_active_program_impl(uint8_t* mem) {
    Gx2PerfScope perf_scope(kPerfShader);
    warm_start();   // first draw: rebuild the recorded programs and pipelines
    auto vs_program = active_shader_program(mem, false);
    auto ps_program = active_shader_program(mem, true);
    if (!vs_program || !ps_program) { ++s_draws_no_shader_set; return nullptr; }
    if (!S.vs_from_regs) trace_shader(S.cur_vs, false, mem);
    if (!S.ps_from_regs) trace_shader(S.cur_ps, true, mem);
    std::vector<R700FetchAttribute> attributes;
    const std::vector<R700FetchAttribute>* fetch_attributes = nullptr;
    Gx2PerfScope key_scope(kPerfShaderKey);
    const unsigned primitive = S.regs[0x2256] & 31;
    const uint32_t rect = primitive == 0x11 ? 1 : primitive == 0x13 || primitive == 0x14 ? 2 : 0;
    // Vulkan: a points program must write gl_PointSize (from uf_pointSize),
    // so point primitives are part of the translation identity here.
    const uint32_t points = primitive == 0x01;
    uint32_t texture_state[2 * 18 * 2];
    unsigned texture_words = 0;
    for (unsigned base : {0xE000u, 0xE460u}) for (unsigned slot = 0; slot < 18; ++slot) {
        texture_state[texture_words++] = S.regs[base + slot * 7] & 7;
        texture_state[texture_words++] = S.regs[base + slot * 7 + 4];
    }
    if (auto fs = S.fetch_shaders.find(S.active_fetch); fs != S.fetch_shaders.end()) fetch_attributes = &fs->second.attributes;
    auto hash_fetch = [&](KeyHash& h) {
        if (fetch_attributes) for (const auto& a : *fetch_attributes) {
            const uint32_t data[] = {a.semantic, a.buffer, a.offset, a.format, a.divisor, a.endian, a.destination_select, unsigned(a.per_instance)};
            h.add(data, sizeof(data));
        }
    };
    // Previous key (RBREW_SHADER_KEY_EXACT=0): bytecode, descriptor bytes
    // and nearly the whole context range.
    auto legacy_key = [&](bool exclude_dynamic) {
        KeyHash h;
        h.add(mem + vs_program.address, vs_program.size); h.add(mem + ps_program.address, ps_program.size);
        h.add(&rect, sizeof(rect)); h.add(&points, sizeof(points));
        if (!S.vs_from_regs) h.add(mem + S.cur_vs, 0x134);
        if (!S.ps_from_regs) h.add(mem + S.cur_ps, 0xE8);
        // SX_ALPHA_REF and stream-out buffer sizes/addresses/counters are
        // live state, not shader identity. Viewport scale/offset and the
        // generic scissor (kDynamicKeyRegisters) are excluded when asked.
        std::array<uint32_t, 0x400> context;
        std::copy_n(S.regs.data() + 0xA000, context.size(), context.data());
        if (exclude_dynamic) for (uint16_t r : kDynamicKeyRegisters) context[r] = 0;
        h.add(context.data(), 0x10E * 4);
        h.add(context.data() + 0x10F, (0x2B4 - 0x10F) * 4);
        for (unsigned i = 0; i < 4; ++i) h.add(context.data() + 0x2B5 + i * 4, 4);
        h.add(context.data() + 0x2C4, (0x400 - 0x2C4) * 4);
        h.add(texture_state, sizeof(texture_state));
        hash_fetch(h);
        return h.finish();
    };
    const bool exact = exact_shader_key();
    uint64_t key, exact_base = 0, canon = 0;
    bool fresh_key = false;   // computed now, not taken from the memo
    if (exact) {
        ExactKeyInputs inputs;
        inputs.rect = rect; inputs.points = points;
        const uint32_t vs_attribs = S.cur_vs_attribs;
        inputs.vs_attribs = vs_attribs;
        auto& words = inputs.words;
        unsigned n = 0;
        for (const auto& range : kGx2TranslatorRegisters) for (uint32_t r = range.first; r <= range.last; ++r) words[n++] = S.regs[r];
        // SPI_PS_INPUT_CNTL_f is read only for f < SPI_PS_IN_CONTROL_0 & 0x3F
        // (clamped to 32): the one reader is LatteShader_CreatePSInputTable.
        const uint32_t ps_inputs = std::min<uint32_t>(S.regs[0xA1B3] & 0x3F, 32);
        for (uint32_t f = ps_inputs; f < 32; ++f) words[kExactWordIndex(0xA191 + f)] = 0;
        // Texture words: the translator reads DIM (word0 bits 0-2) and
        // NUM_FORMAT_ALL (word4 bits 8-9) of each unit it samples.
        auto& texture_bits = inputs.texture_bits;
        unsigned t = 0;
        for (unsigned base : kGx2TranslatorTextureBases) for (unsigned slot = 0; slot < 18; ++slot)
            texture_bits[t++] = (S.regs[base + slot * 7] & 7) | ((S.regs[base + slot * 7 + 4] >> 8) & 3) << 3;
        KeyHash fetch;
        hash_fetch(fetch);
        const uint64_t fetch_hash = fetch.finish();
        inputs.fetch_hash = fetch_hash;
        if (exact_key_memo_enabled() && s_exact_key_memo.matches(inputs, mem + vs_program.address, vs_program.size,
                                                               mem + ps_program.address, ps_program.size)) {
            key = s_exact_key_memo.key; exact_base = s_exact_key_memo.base; canon = s_exact_key_memo.canon;
#ifdef RBREW_GX2_QUEUE_TESTS
            ++s_exact_key_memo_hits;
#endif
        } else {
            const bool memo = exact_key_memo_enabled();
            if (memo) s_exact_key_memo.capture(inputs, mem + vs_program.address, vs_program.size,
                                              mem + ps_program.address, ps_program.size);
            KeyHash h;
            h.add(&kExactKeyTag, sizeof(kExactKeyTag));
            // Hash exactly the bytes retained for future comparisons, rather than
            // copying guest code a second time after computing its identity.
            h.add(memo ? s_exact_key_memo.vs.data() : mem + vs_program.address, vs_program.size);
            h.add(memo ? s_exact_key_memo.ps.data() : mem + ps_program.address, ps_program.size);
            gx2_perf_bytes(kBytesShaderKey, vs_program.size + ps_program.size);
            h.add(&rect, sizeof(rect)); h.add(&points, sizeof(points));
            h.add(&vs_attribs, sizeof(vs_attribs));
            const KeyHash code = h;   // tag, bytecode, primitive kind, attribute count
            // Base: everything except the per-parameter VS output IDs and the
            // per-unit texture words, whose read sets the translation reports.
            KeyHash base = h;
            for (unsigned i = 0; i < n; ++i)
                if (i < kExactWordIndex(0xA185) || i > kExactWordIndex(0xA18E)) base.add(&words[i], 4);
            base.add(&fetch_hash, sizeof(fetch_hash));
            exact_base = base.finish();
            uint32_t known = 0;
            if (auto reads = s_exact_reads.find(exact_base); reads != s_exact_reads.end()) {
                known = 1;
                const ExactReads& r = reads->second;
                for (unsigned k = 0; k < 10; ++k)
                    if (k >= 8 || ((r.vs_parameter_mask >> (k * 4)) & 0xF) == 0) words[kExactWordIndex(0xA185 + k)] = 0;
                for (unsigned slot = 0; slot < 18; ++slot) {
                    if (!((r.ps_texture_mask >> slot) & 1)) texture_bits[slot] = 0;        // kGx2TranslatorTextureBases[0]: PS
                    if (!((r.vs_texture_mask >> slot) & 1)) texture_bits[18 + slot] = 0;   // [1]: VS
                }
            }
            h.add(&exact_base, sizeof(exact_base));
            h.add(&known, sizeof(known));
            h.add(words.data() + kExactWordIndex(0xA185), 10 * 4);
            h.add(texture_bits.data(), sizeof(texture_bits));
            key = h.finish();
            if (canonical_key_enabled()) {
                KeyHash c = code;
                c.add(&kCanonicalKeyTag, sizeof(kCanonicalKeyTag));
                for (unsigned i = 0; i < n; ++i) { const uint32_t w = words[i] & kCanonicalWordBits[i]; c.add(&w, sizeof(w)); }
                c.add(texture_bits.data(), sizeof(texture_bits));
                const uint32_t attribute_count = fetch_attributes ? uint32_t(fetch_attributes->size()) : 0u;
                c.add(&attribute_count, sizeof(attribute_count));
                if (fetch_attributes) for (const auto& a : *fetch_attributes) {
                    const uint32_t data[] = {a.semantic, a.format, a.endian, a.destination_select, a.offset & 3u, a.buffer >= 16 ? 1u : 0u};
                    c.add(data, sizeof(data));
                }
                canon = c.finish();
            }
            fresh_key = true;
            if (memo) s_exact_key_memo.canon = canon;
            if (s_prog_cache.find(key) == s_prog_cache.end()) note_exact_key_miss(vs_program.address, ps_program.address, mem, vs_program.size,
                                                                                  ps_program.size, words.data(), texture_bits.data(), fetch_hash, rect | points << 2 | vs_attribs << 3);
            if (memo) { s_exact_key_memo.key = key; s_exact_key_memo.base = exact_base; s_exact_key_memo.valid = true; }
        }
    } else {
        key = legacy_key(dynamic_key_registers_excluded());
        gx2_perf_bytes(kBytesShaderKey, vs_program.size + ps_program.size + 0x400 * 4);
    }
    auto it = s_prog_cache.find(key);
#ifdef RBREW_GX2_QUEUE_TESTS
    if (exact) s_last_exact_key = key;
#endif
    key_scope.stop();
    if (it != s_prog_cache.end()) {
        // Programs linked by the boot warm-up or earlier draws seed the
        // canonical map as soon as their inputs are seen.
        if (fresh_key && canon && it->second) s_canonical.try_emplace(canon, CanonicalEntry{it->second});
        if (it->second && !await_compile(it->second->compiled)) return nullptr;
        if (shader_key_verify() && it->second && (exact || dynamic_key_registers_excluded())) {
            // A hit the original full key would have missed: translate as
            // the miss path would, and require the program it would reuse.
            KeyHash pair;
            const uint64_t full = legacy_key(false);
            pair.add(&key, sizeof(key)); pair.add(&full, sizeof(full));
            static std::unordered_set<uint64_t> verified;
            if (verified.insert(pair.finish()).second) {
                std::vector<R700FetchAttribute> verify_attributes;
                if (fetch_attributes) verify_attributes = *fetch_attributes;
                std::array<R700TranslationInfo, 2> verify_info;
                R700TranslateInput vi{};
                vi.data = mem + vs_program.address; vi.size = vs_program.size; vi.is_pixel = false; vi.num_attribs = S.cur_vs_attribs;
                vi.context_registers = S.regs.data(); vi.attributes = verify_attributes; vi.info = &verify_info[0]; vi.vulkan = true;
                vi.vulkan_negative_one_to_one = gx2vk::caps().negative_one_to_one;
                char* vs = r700_to_glsl(vi);
                R700TranslateInput pi{};
                pi.data = mem + ps_program.address; pi.size = ps_program.size; pi.is_pixel = true;
                pi.context_registers = S.regs.data(); pi.info = &verify_info[1]; pi.vulkan = true;
                char* ps = r700_to_glsl(pi);
                const Program& p = *it->second;
                static const bool solid_ps = std::getenv("RBREW_DEBUG_SOLID_PS") != nullptr;
                const bool same = same_translation(vs, verify_info[0], p.vs_source.c_str(), p.info[0]) &&
                                  (solid_ps || same_translation(ps, verify_info[1], p.ps_source.c_str(), p.info[1]));
                static uint64_t checked = 0, different = 0;
                ++checked;
                if (!same) {
                    ++different;
                    RBREW_LOGF(stderr, "[shader] key verify DIFFERS program=%u VS=%08X PS=%08X (%llu of %llu)\n", p.id, vs_program.address,
                                 ps_program.address, (unsigned long long)different, (unsigned long long)checked);
                } else if (checked % 100 == 0) {
                    RBREW_LOGF(stderr, "[shader] key verify %llu identical, %llu different\n",
                                 (unsigned long long)(checked - different), (unsigned long long)different);
                }
                std::free(vs); std::free(ps);
            }
        }
        return it->second;
    }
    if (exact && canon) if (auto known = s_canonical.find(canon); known != s_canonical.end() && known->second.program) {
        // Same translation as an existing program: no translation, no new
        // program. Recorded under this key for the disk cache and warm-up.
        Program* prog = known->second.program;
        s_prog_cache[key] = prog;
        ++s_canonical_reuses;
        if (!await_compile(prog->compiled)) return nullptr;
        if (known->second.reads_valid && s_exact_reads.try_emplace(exact_base, known->second.reads).second)
            warm_record_masks(exact_base, known->second.reads);
        static const bool solid_ps = std::getenv("RBREW_DEBUG_SOLID_PS") != nullptr;
        if (!solid_ps) store_translation(key, prog->vs_source.c_str(), prog->ps_source.c_str(), prog->info);
        if (!prog->warm_key) prog->warm_key = key;
        warm_record_program(key, rect, points);
        return prog;
    }
    if (fetch_attributes) attributes = *fetch_attributes;

    // Disk identity: the same inputs as `key` minus what changes between
    // launches without changing the translation. Shader descriptors reach the
    // translator only through registers (r700_shader_registers writes into the
    // hashed context range) and the attribute count, so their bytes - which
    // hold guest pointers - are replaced by that count. Registers that hold
    // memory addresses are zeroed; the translator reads none of them.
    // The exact key already holds no addresses: it is the disk identity too.
    uint64_t disk_key = key;
    if (!exact) {
        KeyHash disk;
        disk.add(mem + vs_program.address, vs_program.size); disk.add(mem + ps_program.address, ps_program.size);
        disk.add(&rect, sizeof(rect)); disk.add(&points, sizeof(points));
        const uint32_t vs_attribs = S.cur_vs_attribs;
        disk.add(&vs_attribs, sizeof(vs_attribs));
        std::array<uint32_t, 0x400> context;
        std::copy_n(S.regs.data() + 0xA000, context.size(), context.data());
        static constexpr uint16_t kAddressRegisters[] = {
            0x003, 0x005,                                            // DB_DEPTH_BASE, DB_HTILE_DATA_BASE
            0x010, 0x011, 0x012, 0x013, 0x014, 0x015, 0x016, 0x017,  // CB_COLOR*_BASE
            0x030, 0x031, 0x032, 0x033, 0x034, 0x035, 0x036, 0x037,  // CB_COLOR*_TILE
            0x038, 0x039, 0x03A, 0x03B, 0x03C, 0x03D, 0x03E, 0x03F,  // CB_COLOR*_FRAG
            0x1F9, 0x1FA, 0x1FE,                                     // VGT_DMA_BASE(_HI), VGT_EVENT_ADDRESS
            0x210, 0x216, 0x21B, 0x220, 0x225,                       // SQ_PGM_START_*
            0x2C4, 0x2C5, 0x2C6, 0x2C7, 0x2D1, 0x2D2, 0x2D3, 0x2D4}; // VGT_STRMOUT_BASE_OFFSET(_HI)_*
        for (uint16_t r : kAddressRegisters) context[r] = 0;
        if (dynamic_key_registers_excluded()) for (uint16_t r : kDynamicKeyRegisters) context[r] = 0;
        disk.add(context.data(), 0x10E * 4);
        disk.add(context.data() + 0x10F, (0x2B4 - 0x10F) * 4);
        for (unsigned i = 0; i < 4; ++i) disk.add(context.data() + 0x2B5 + i * 4, 4);
        disk.add(context.data() + 0x2C4, (0x400 - 0x2C4) * 4);
        disk.add(texture_state, sizeof(texture_state));
        if (fetch_attributes) for (const auto& a : *fetch_attributes) {
            const uint32_t data[] = {a.semantic, a.buffer, a.offset, a.format, a.divisor, a.endian, a.destination_select, unsigned(a.per_instance)};
            disk.add(data, sizeof(data));
        }
        disk_key = disk.finish();
    }

    std::array<R700TranslationInfo, 2> info;
    char* vs_glsl = nullptr;
    char* ps_glsl = nullptr;
#ifndef RBREW_DISABLE_LOGGING
    const auto translate_start = std::chrono::steady_clock::now();
#endif
    std::shared_ptr<TranslateJob> ps_job;
    R700TranslationReads vs_reads, ps_reads;
    const bool from_disk = load_translation(disk_key, vs_glsl, ps_glsl, info);
    if (!from_disk) {
    // A cache hit already owns both translated stages. Only a miss needs the
    // helper, its bytecode/register snapshot and the eventual compiler work.
    if (parallel_translation()) ps_job = post_pixel_translation(mem + ps_program.address, ps_program.size);
    {
        R700TranslateInput ti{};
        ti.data = mem + vs_program.address; ti.size = vs_program.size; ti.is_pixel = false; ti.num_attribs = S.cur_vs_attribs;
        ti.context_registers = S.regs.data(); ti.attributes = attributes; ti.info = &info[0]; ti.vulkan = true;
        ti.vulkan_negative_one_to_one = gx2vk::caps().negative_one_to_one;
        vs_glsl = r700_to_glsl(ti);
        vs_reads = r700_last_translation_reads();
    }
    if (ps_job) {
        ps_glsl = ps_job->take(info[1], ps_reads);
        ps_job.reset();
    }
    if (!parallel_translation() || parallel_translation_verify()) {
        R700TranslationInfo serial_info;
        R700TranslateInput ti{};
        ti.data = mem + ps_program.address; ti.size = ps_program.size; ti.is_pixel = true;
        ti.context_registers = S.regs.data(); ti.info = &serial_info; ti.vulkan = true;
        char* serial = r700_to_glsl(ti);
        ps_reads = r700_last_translation_reads();
        if (parallel_translation()) {
            static uint64_t checked = 0, different = 0;
            ++checked;
            if (!same_translation(ps_glsl, info[1], serial, serial_info)) {
                ++different;
                RBREW_LOGF(stderr, "[shader] parallel translation DIFFERS PS=%08X (%llu of %llu)\n", ps_program.address,
                             (unsigned long long)different, (unsigned long long)checked);
            } else if (checked % 100 == 0) {
                RBREW_LOGF(stderr, "[shader] parallel translation verified %llu identical, %llu different\n",
                             (unsigned long long)(checked - different), (unsigned long long)different);
            }
        }
        std::free(ps_glsl);
        ps_glsl = serial;
        info[1] = std::move(serial_info);
    }
    store_translation(disk_key, vs_glsl, ps_glsl, info);
    if (exact && vs_glsl && ps_glsl && vs_reads.valid && ps_reads.valid)
        if (s_exact_reads.try_emplace(exact_base, ExactReads{vs_reads.vs_parameter_mask, vs_reads.texture_unit_mask, ps_reads.texture_unit_mask}).second)
            warm_record_masks(exact_base, s_exact_reads[exact_base]);
    }
#ifndef RBREW_DISABLE_LOGGING
    {
        static uint64_t hits = 0, misses = 0, miss_ns = 0;
        ++(from_disk ? hits : misses);
        if (!from_disk)
            miss_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - translate_start).count());
        if ((hits + misses) % 1000 == 0)
            RBREW_LOGF(stderr, "[shader] translation cache hits=%llu misses=%llu parallel=%d miss_us=%.1f\n",
                         (unsigned long long)hits, (unsigned long long)misses, int(parallel_translation()),
                         misses ? miss_ns / 1e3 / double(misses) : 0.0);
    }
#endif
    if (const char* directory = rbrew_diagnostic_env("RBREW_SHADER_DUMP_DIR"); directory && vs_glsl && ps_glsl) {
        char path[1024];
        std::snprintf(path, sizeof(path), "%s/native_%llX.vs.glsl", directory, (unsigned long long)key);
        if (FILE* f = std::fopen(path, "wb")) { std::fwrite(vs_glsl, 1, std::strlen(vs_glsl), f); std::fclose(f); }
        std::snprintf(path, sizeof(path), "%s/native_%llX.ps.glsl", directory, (unsigned long long)key);
        if (FILE* f = std::fopen(path, "wb")) { std::fwrite(ps_glsl, 1, std::strlen(ps_glsl), f); std::fclose(f); }
        if (rect) {
            const auto gs = r700_rect_geometry(vs_glsl, rect == 2);
            std::snprintf(path, sizeof(path), "%s/native_%llX.gs.glsl", directory, (unsigned long long)key);
            if (FILE* f = std::fopen(path, "wb")) { std::fwrite(gs.data(), 1, gs.size(), f); std::fclose(f); }
        }
    }
    bool reused = false;
    Program* prog = link_program(vs_glsl, ps_glsl, info, rect, points, true, reused);
    if (prog && exact) {
        if (!prog->warm_key) prog->warm_key = key;
        warm_record_program(key, rect, points);
        if (canon) {
            auto& entry = s_canonical[canon];
            if (!entry.program) entry.program = prog;
            if (!entry.reads_valid && vs_reads.valid && ps_reads.valid) {
                entry.reads = ExactReads{vs_reads.vs_parameter_mask, vs_reads.texture_unit_mask, ps_reads.texture_unit_mask};
                entry.reads_valid = true;
            }
        }
    }
    std::free(vs_glsl);
    std::free(ps_glsl);
    if (!reused) RBREW_LOGF(stderr, "[shader] native program=%u VS=%08X%s PS=%08X%s\n", prog ? prog->id : 0, vs_program.address,
                              S.vs_from_regs ? "(regs)" : "", ps_program.address, S.ps_from_regs ? "(regs)" : "");
    trace_program_key_miss(vs_program.address, ps_program.address, reused);
    s_prog_cache[key] = prog;
    return prog;
}

// Translation -> program: identical sources share one Program (and its
// Vulkan objects). `count` feeds the link/reuse counters (not for warm-up).
Program* link_program(const char* vs_glsl, const char* ps_glsl, const std::array<R700TranslationInfo, 2>& info,
                      uint32_t rect, uint32_t points, bool count, bool& reused, bool background) {
    static const bool s_solid_ps = std::getenv("RBREW_DEBUG_SOLID_PS") != nullptr;
    static const char* s_solid_ps_src = "#version 450\nlayout(location = 0) out vec4 o0;\nvoid main() { o0 = vec4(1.0); }\n";
    Program* prog = nullptr;
    reused = false;
    if (vs_glsl && ps_glsl) {
        const char* ps_source = s_solid_ps ? s_solid_ps_src : ps_glsl;
        KeyHash source;
        source.add(&rect, sizeof(rect)); source.add(&points, sizeof(points));
        source.add(vs_glsl, std::strlen(vs_glsl)); source.add(ps_source, std::strlen(ps_source));
        for (const auto& stage : info) {
            for (const auto& entry : stage.remapped) {
                const uint32_t words[] = {entry.is_register, entry.bank, entry.index, entry.mapped_index};
                source.add(words, sizeof(words));
            }
            source.add(stage.attribute_locations.data(), stage.attribute_locations.size());
            source.add(stage.buffer_bindings.data(), stage.buffer_bindings.size());
            const uint32_t modes[] = {stage.uniform_mode, stage.register_count};
            source.add(modes, sizeof(modes));
        }
        auto& linked = s_prog_by_source[source.finish()];
        reused = linked != nullptr;
        if (!reused) {
            linked = std::make_unique<Program>();
            auto& p = *linked;
            p.id = s_next_program++;
            p.info = info;
            if (s_solid_ps) { p.info[1].vk = R700VulkanInterface{}; p.info[1].vk.set = 1; p.info[1].vk.pixel_outputs = 1; p.info[1].uniform_mode = 0; }
            p.vs_source = vs_glsl; p.ps_source = ps_source;
            p.rect = rect; p.points = points != 0;
            if (rect) p.gs_source = r700_rect_geometry(vs_glsl, rect == 2);
            ++C.uniform_lookups;
            p.compiled = post_compile([program = &p, background] {
                auto& p = *program;
                // A draw waits for a new program: compile its pixel stage
                // beside the vertex/geometry stages (shader_module is thread
                // safe) so it waits for the slowest stage, not their sum.
                // Warm-up programs (SPIR-V mostly on disk) stay on the worker.
                std::future<VkShaderModule> pixel;
                if (!background)
                    pixel = std::async(std::launch::async, [&p] { return gx2vk::shader_module(VK_SHADER_STAGE_FRAGMENT_BIT, p.ps_source); });
                p.vs = gx2vk::shader_module(VK_SHADER_STAGE_VERTEX_BIT, p.vs_source);
                if (p.rect) p.gs = gx2vk::shader_module(VK_SHADER_STAGE_GEOMETRY_BIT, p.gs_source);
                const auto ps = pixel.valid() ? pixel.get() : gx2vk::shader_module(VK_SHADER_STAGE_FRAGMENT_BIT, p.ps_source);
                p.valid = p.vs && ps && (!p.rect || p.gs) && create_program_layouts(p);
                return p.valid;
            }, background);
        }
        prog = background || await_compile(linked->compiled) ? linked.get() : nullptr;
        if (count) ++(reused ? C.programs_reused : C.programs_linked);
    }
    return prog;
}

Program* resolve_active_program(uint8_t* mem) {
    Program* p = resolve_active_program_impl(mem);
#ifndef RBREW_DISABLE_LOGGING
    ++s_draws_requested;
    if (!p) ++s_draws_no_program; else ++s_draws_per_fbo[S.current_fbo_id];
#endif
    return p;
}

// ---------------------------------------------------------------------------
// Pipelines
// ---------------------------------------------------------------------------
struct VertexInput {
    uint32_t count = 0;
    struct Attr { uint32_t location; VkFormat format; uint32_t stride; uint32_t rate; uint32_t divisor; } attrs[kMaxVertexInputs];
};
// The warm-up file keeps its original 16-input record; pipelines with more
// inputs are built at their first draw instead.
struct WarmVertexInput {
    uint32_t count = 0;
    VertexInput::Attr attrs[16];
};
// Every attribute has its own binding.
uint32_t vertex_input_limit() {
    const auto& limits = gx2vk::caps().limits;
    const uint32_t device = std::min(limits.maxVertexInputAttributes, limits.maxVertexInputBindings);
    return device ? std::min(kMaxVertexInputs, device) : 16u;
}
struct PipelineKeyHash {
    using is_transparent=void;
    size_t operator()(std::string_view key) const noexcept { return std::hash<std::string_view>{}(key); }
};
std::unordered_map<std::string, gx2vk::CompileQueue::Job<VkPipeline>,PipelineKeyHash,std::equal_to<>> s_pipelines;

VkBlendFactor vk_blend(uint32_t f) {
    switch (f) {
    case wire::ZERO: return VK_BLEND_FACTOR_ZERO; case wire::ONE: return VK_BLEND_FACTOR_ONE;
    case wire::SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR; case wire::ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case wire::SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA; case wire::ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case wire::DST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA; case wire::ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case wire::DST_COLOR: return VK_BLEND_FACTOR_DST_COLOR; case wire::ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case wire::SRC_ALPHA_SATURATE: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case wire::CONSTANT_COLOR: return VK_BLEND_FACTOR_CONSTANT_COLOR; case wire::ONE_MINUS_CONSTANT_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case wire::CONSTANT_ALPHA: return VK_BLEND_FACTOR_CONSTANT_ALPHA; case wire::ONE_MINUS_CONSTANT_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}
VkBlendOp vk_blend_op(uint32_t e) {
    switch (e) {
    case wire::FUNC_SUBTRACT: return VK_BLEND_OP_SUBTRACT; case wire::FUNC_REVERSE_SUBTRACT: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case wire::MIN: return VK_BLEND_OP_MIN; case wire::MAX: return VK_BLEND_OP_MAX; default: return VK_BLEND_OP_ADD;
    }
}
VkStencilOp vk_stencil_op(uint32_t op) {
    switch (op) {
    case wire::ZERO: return VK_STENCIL_OP_ZERO; case wire::REPLACE: return VK_STENCIL_OP_REPLACE;
    case wire::INCR: return VK_STENCIL_OP_INCREMENT_AND_CLAMP; case wire::DECR: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case wire::INVERT: return VK_STENCIL_OP_INVERT; case wire::INCR_WRAP: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case wire::DECR_WRAP: return VK_STENCIL_OP_DECREMENT_AND_WRAP; default: return VK_STENCIL_OP_KEEP;
    }
}

struct PipelineRequest {
    Program* program; VkPrimitiveTopology topology; bool xfb;
    VertexInput input;
    VkFormat colors[8]; VkFormat depth; bool stencil;
    VkPipelineColorBlendAttachmentState blend[8];
    uint32_t ps_uint_mask = 0, ps_sint_mask = 0;   // integer-output pixel variant of r.ps
};

// Register-derived pipeline switches (from S.regs at the draw).
struct PipelineSwitches { bool discard, half_z, z_clip; };
PipelineSwitches pipeline_switches(const Program& p) {
    PipelineSwitches s;
    s.discard = p.info[0].streamout_mask && (S.regs[0xA2AC] & 1) && (S.regs[0xA204] & (1u << 22));
    s.half_z = (S.regs[0xA204] & (1u << 19)) != 0;
    // PA_CL_CLIP_CNTL ZCLIP_FAR_DISABLE (bit 27); Cemu treats near/far as one
    // switch. Shadow casters rely on it to pancake geometry outside the light
    // frustum's depth range instead of being clipped away.
    s.z_clip = (S.regs[0xA204] & (1u << 27)) == 0;
    return s;
}
// Preserve the exact existing byte key, but keep cache-hit lookups on the
// stack. Only a new pipeline needs an owned string in the map.
struct PipelineKey {
    static constexpr size_t kCapacity=16+5*sizeof(bool)+4+sizeof(VertexInput::Attr)*kMaxVertexInputs+sizeof(VkFormat)*9+sizeof(VkPipelineColorBlendAttachmentState)*8;
    std::array<char,kCapacity> bytes;
    size_t size=0;
    std::string_view view() const { return {bytes.data(),size}; }
};
PipelineKey pipeline_key(const PipelineRequest& r, const PipelineSwitches& s) {
    PipelineKey key{};
    auto put = [&](const void* p, size_t n) { std::memcpy(key.bytes.data()+key.size,p,n);key.size+=n; };
    put(&r.program->id, 4); put(&r.ps_uint_mask, 4); put(&r.ps_sint_mask, 4); put(&r.topology, 4); put(&r.xfb, 1);
    put(&s.discard, sizeof(s.discard));
    put(&s.half_z, sizeof(s.half_z));
    put(&s.z_clip, sizeof(s.z_clip));
    put(&r.input.count, 4); put(r.input.attrs, sizeof(VertexInput::Attr) * r.input.count);
    put(r.colors, sizeof(r.colors)); put(&r.depth, 4); put(&r.stencil, 1);
    put(r.blend, sizeof(r.blend));
    return key;
}
VkPipeline create_pipeline(const PipelineRequest& r, const PipelineSwitches& s);
void warm_record_pipeline(const PipelineRequest& r, const PipelineSwitches& s);

VkPipeline get_pipeline(const PipelineRequest& r) {
    const PipelineSwitches s = pipeline_switches(*r.program);
    const auto key=pipeline_key(r,s);
    const auto found=s_pipelines.find(key.view());
    if(found!=s_pipelines.end())return await_compile(found->second);
    (void)gx2vk::pipeline_cache();
    auto [inserted,added]=s_pipelines.emplace(std::string(key.view()),post_compile([r,s]{return create_pipeline(r,s);}));
    ++C.pipelines;
    if(!r.xfb)warm_record_pipeline(r,s);
    return await_compile(inserted->second);
}

// Thread-safe for non-xfb requests: Vulkan objects only (the boot warm-up
// creates pipelines on worker threads).
VkPipeline create_pipeline(const PipelineRequest& r, const PipelineSwitches& s) {
    const bool discard = s.discard, half_z = s.half_z, z_clip = s.z_clip;
    VkPipeline pipeline = VK_NULL_HANDLE;
    auto& p = *r.program;
    VkPipelineShaderStageCreateInfo stages[3]{};
    uint32_t stage_count = 0;
    auto stage = [&](VkShaderStageFlagBits s, VkShaderModule m) {
        stages[stage_count] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[stage_count].stage = s; stages[stage_count].module = m; stages[stage_count].pName = "main";
        ++stage_count;
    };
    VkShaderModule vs = p.vs, gs = p.gs;
    if (r.xfb) {
        // Replay variant: capture gl_Position of the last vertex stage.
        if (p.rect) gs = gx2vk::shader_module(VK_SHADER_STAGE_GEOMETRY_BIT, r700_vulkan_capture_position(p.gs_source));
        else vs = gx2vk::shader_module(VK_SHADER_STAGE_VERTEX_BIT, r700_vulkan_capture_position(p.vs_source));
        if ((p.rect && !gs) || (!p.rect && !vs)) return VK_NULL_HANDLE;   // compile error already reported
    }
    stage(VK_SHADER_STAGE_VERTEX_BIT, vs);
    if (p.rect) stage(VK_SHADER_STAGE_GEOMETRY_BIT, gs);
    if (!discard) {
        const auto ps = gx2vk::shader_module(VK_SHADER_STAGE_FRAGMENT_BIT,
            r700_vulkan_integer_outputs(p.ps_source, r.ps_uint_mask, r.ps_sint_mask));
        if (!ps) return VK_NULL_HANDLE;
        stage(VK_SHADER_STAGE_FRAGMENT_BIT, ps);
    }
    VkVertexInputBindingDescription bindings[kMaxVertexInputs];
    VkVertexInputAttributeDescription attrs[kMaxVertexInputs];
    VkVertexInputBindingDivisorDescriptionKHR divisors[kMaxVertexInputs];
    uint32_t divisor_count = 0;
    for (uint32_t i = 0; i < r.input.count; ++i) {
        const auto& a = r.input.attrs[i];
        bindings[i] = {i, a.stride, a.rate ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
        attrs[i] = {a.location, i, a.format, 0};
        if (a.rate && a.divisor != 1 && gx2vk::caps().vertex_divisor) divisors[divisor_count++] = {i, a.divisor};
    }
    VkPipelineVertexInputDivisorStateCreateInfoKHR divisor_state{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_KHR};
    divisor_state.vertexBindingDivisorCount = divisor_count;
    divisor_state.pVertexBindingDivisors = divisors;
    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex.vertexBindingDescriptionCount = r.input.count;
    vertex.pVertexBindingDescriptions = bindings;
    vertex.vertexAttributeDescriptionCount = r.input.count;
    vertex.pVertexAttributeDescriptions = attrs;
    if (divisor_count) vertex.pNext = &divisor_state;
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = r.topology;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1; viewport.scissorCount = 1;
    VkPipelineViewportDepthClipControlCreateInfoEXT depth_clip{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT};
    depth_clip.negativeOneToOne = half_z ? VK_FALSE : VK_TRUE;
    if (gx2vk::caps().negative_one_to_one) viewport.pNext = &depth_clip;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.lineWidth = 1.0f;
    // Latte always clamps window depth to the viewport range; Z clipping is
    // controlled separately (Cemu: depthClampEnable = TRUE, depthClipEnable =
    // !ZCLIP_FAR_DISABLE). Without VK_EXT_depth_clip_enable, Vulkan ties
    // clipping to !depthClampEnable, so clamp only where clipping is off.
    VkPipelineRasterizationDepthClipStateCreateInfoEXT clip_state{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT};
    clip_state.depthClipEnable = z_clip;
    const void* raster_tail = nullptr;
    if (gx2vk::caps().depth_clamp) {
        if (gx2vk::caps().depth_clip_enable) {
            raster.depthClampEnable = VK_TRUE;
            raster_tail = &clip_state;
        } else {
            raster.depthClampEnable = z_clip ? VK_FALSE : VK_TRUE;
        }
    }
    // gx2.rpl GX2GetPolygonControlReg initializes PA_SU_SC_MODE_CNTL bit 19
    // (PROVOKING_VTX_LAST): flat interpolation takes the last vertex.
    VkPipelineRasterizationProvokingVertexStateCreateInfoEXT provoking{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT};
    provoking.provokingVertexMode = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
    provoking.pNext = raster_tail;
    raster.pNext = gx2vk::caps().provoking_vertex_last ? &provoking : raster_tail;
    // The replay position pass rasterizes with every write masked (see
    // issue_draw): rasterizer discard would drop the fragment and attachment
    // state that must match the open rendering instance.
    raster.rasterizerDiscardEnable = discard;
    VkPipelineRasterizationStateStreamCreateInfoEXT stream{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_STREAM_CREATE_INFO_EXT};
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 8;
    blend.pAttachments = r.blend;
    const VkDynamicState dynamic_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
        VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_DEPTH_BIAS,
        VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = uint32_t(std::size(dynamic_states));
    dynamic.pDynamicStates = dynamic_states;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = discard?0:8;
    rendering.pColorAttachmentFormats = discard?nullptr:r.colors;
    rendering.depthAttachmentFormat = discard?VK_FORMAT_UNDEFINED:r.depth;
    rendering.stencilAttachmentFormat = !discard && r.stencil ? r.depth : VK_FORMAT_UNDEFINED;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = stage_count;
    info.pStages = stages;
    info.pVertexInputState = &vertex;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = discard?nullptr:&multisample;
    info.pDepthStencilState = discard?nullptr:&depth;
    info.pColorBlendState = discard?nullptr:&blend;
    info.pDynamicState = &dynamic;
    info.layout = p.layout;
    if (vkCreateGraphicsPipelines(gx2vk::device(), gx2vk::pipeline_cache(), 1, &info, nullptr, &pipeline) != VK_SUCCESS) {
        TXDIAG(txdiag::event("pipeline_create_failed","\"program\":%u,\"layout\":%llu",p.id,txdiag::handle(p.layout)));
        RBREW_LOGF(stderr, "[vk] pipeline creation failed for program %u\n", p.id);
        pipeline = VK_NULL_HANDLE;
    }
    (void)stream;
    return pipeline;
}

// ---------------------------------------------------------------------------
// Boot warm-up (Cemu-style shader/pipeline cache precompile; owner request).
// Every exact program key, translator read mask and pipeline the game uses
// is appended to warm.bin in the translation cache directory. On the first
// draw of the next session, program builds are queued from cached
// translations. Completed programs feed background pipeline builds; demand
// draws promote only their dependency. Renderer maps remain owner-thread only. RBREW_SHADER_WARMUP=0 disables it (recording and rebuild).
// ---------------------------------------------------------------------------
constexpr uint32_t kWarmMagic = 0x4D574252u, kWarmVersion = 1;  // "RBWM"
enum WarmTag : uint32_t { kWarmProgram = 1, kWarmMasks = 2, kWarmPipeline = 3 };
struct WarmProgramRecord { uint64_t key; uint32_t rect, points; };
struct WarmMasksRecord { uint64_t base; ExactReads reads; };
struct WarmPipelineRecord {
    uint64_t program_key;
    uint32_t ps_uint_mask, ps_sint_mask;
    VkPrimitiveTopology topology;
    WarmVertexInput input;
    VkFormat colors[8]; VkFormat depth;
    uint32_t stencil, discard, half_z, z_clip;
    VkPipelineColorBlendAttachmentState blend[8];
};
static_assert(std::is_trivially_copyable_v<WarmPipelineRecord> && std::is_trivially_copyable_v<WarmMasksRecord>);
bool warmup_enabled() {
    static const bool on = [] { const char* v = std::getenv("RBREW_SHADER_WARMUP"); return !(v && v[0] == '0'); }();
    return on && exact_shader_key();
}
struct WarmState {
    std::string dir;                      // translation cache directory the file belongs to
    std::FILE* file = nullptr;            // append handle
    bool started = false;
    std::deque<WarmPipelineRecord> pending;
    std::unordered_set<uint64_t> programs, masks, pipelines;   // already recorded (hash of record)
};
WarmState s_warm;
uint64_t warm_hash(const void* data, size_t bytes) { KeyHash h; h.add(data, bytes); return h.finish(); }
void warm_append(uint32_t tag, const void* data, uint32_t bytes) {
    if (!s_warm.file) return;
    std::fwrite(&tag, 4, 1, s_warm.file); std::fwrite(&bytes, 4, 1, s_warm.file);
    std::fwrite(data, 1, bytes, s_warm.file);
    std::fflush(s_warm.file);
}
void warm_record_program(uint64_t key, uint32_t rect, uint32_t points) {
    if (!s_warm.file || !s_warm.programs.insert(key).second) return;
    const WarmProgramRecord record{key, rect, points};
    warm_append(kWarmProgram, &record, sizeof(record));
}
void warm_record_masks(uint64_t base, const ExactReads& reads) {
    if (!s_warm.file || !s_warm.masks.insert(base).second) return;
    WarmMasksRecord record{};
    record.base = base; record.reads = reads;
    warm_append(kWarmMasks, &record, sizeof(record));
}
WarmPipelineRecord warm_pipeline_record(const PipelineRequest& r, const PipelineSwitches& s) {
    WarmPipelineRecord record;
    std::memset(&record, 0, sizeof(record));   // padding included: records hash and compare as bytes
    record.program_key = r.program->warm_key;
    record.ps_uint_mask = r.ps_uint_mask; record.ps_sint_mask = r.ps_sint_mask;
    record.topology = r.topology;
    record.input.count = r.input.count;
    std::copy_n(r.input.attrs, r.input.count, record.input.attrs);
    std::copy_n(r.colors, 8, record.colors); record.depth = r.depth; record.stencil = r.stencil;
    record.discard = s.discard; record.half_z = s.half_z; record.z_clip = s.z_clip;
    std::copy_n(r.blend, 8, record.blend);
    return record;
}
void warm_record_pipeline(const PipelineRequest& r, const PipelineSwitches& s) {
    if (!s_warm.file || !r.program->warm_key || r.input.count > std::size(WarmVertexInput{}.attrs)) return;
    const WarmPipelineRecord record = warm_pipeline_record(r, s);
    if (!s_warm.pipelines.insert(warm_hash(&record, sizeof(record))).second) {
        // Recorded earlier yet created at a draw: the warm-up did not cover it.
        static uint64_t missed = 0;
        if (++missed <= 10 || missed % 1000 == 0)
            RBREW_LOGF(stderr, "[warm] recorded pipeline built at draw time (%llu)\n", (unsigned long long)missed);
        return;
    }
    warm_append(kWarmPipeline, &record, sizeof(record));
}

// Publish background pipeline jobs only after their program is ready. No worker
// waits on a job from the same pool, and only the render owner touches maps.
void warm_poll() {
    for (auto it = s_warm.pending.begin(); it != s_warm.pending.end();) {
        const auto& w = *it;
        auto found = s_prog_cache.find(w.program_key);
        if (found == s_prog_cache.end() || !found->second) { it = s_warm.pending.erase(it); continue; }
        Program& prog = *found->second;
        if (!prog.compiled.ready()) { ++it; continue; }
        if (!await_compile(prog.compiled)) { it = s_warm.pending.erase(it); continue; }
        PipelineRequest r{};
        r.program = &prog;
        r.ps_uint_mask = w.ps_uint_mask; r.ps_sint_mask = w.ps_sint_mask;
        r.topology = w.topology;
        r.input.count = w.input.count; std::copy_n(w.input.attrs, w.input.count, r.input.attrs);
        std::copy_n(w.colors, 8, r.colors); r.depth = w.depth; r.stencil = w.stencil != 0;
        std::copy_n(w.blend, 8, r.blend);
        const PipelineSwitches switches{w.discard != 0, w.half_z != 0, w.z_clip != 0};
        const auto key = pipeline_key(r, switches);
        if (!s_pipelines.count(key.view())) {
            (void)gx2vk::pipeline_cache();
            s_pipelines.emplace(std::string(key.view()), post_compile([r, switches] { return create_pipeline(r, switches); }, true));
        }
        it = s_warm.pending.erase(it);
    }
}
// Read cache records once. Shader/module and pipeline creation run in the
// background; a first-use draw promotes and waits for only its dependency.
void warm_start() {
    if (s_warm.started) { warm_poll(); return; }
    if (!warmup_enabled()) return;
    s_warm.started = true;
    const std::string dir = translation_cache_dir();
    if (dir.empty()) return;
    s_warm.dir = dir;
    const std::string path = (std::filesystem::path(dir) / (translation_cache_prefix()+"warm.bin")).string();
    std::vector<WarmProgramRecord> programs;
    std::vector<WarmPipelineRecord> pipelines;
    bool valid_file = false;
    if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
        uint32_t header[2]{};
        valid_file = std::fread(header, 4, 2, f) == 2 && header[0] == kWarmMagic && header[1] == kWarmVersion;
        for (uint32_t rec[2]; valid_file && std::fread(rec, 4, 2, f) == 2;) {
            if (rec[1] > (1u << 20)) break;
            std::vector<uint8_t> data(rec[1]);
            if (std::fread(data.data(), 1, data.size(), f) != data.size()) break;   // torn tail: keep what was read
            if (rec[0] == kWarmProgram && rec[1] == sizeof(WarmProgramRecord)) {
                WarmProgramRecord p; std::memcpy(&p, data.data(), sizeof(p));
                if (s_warm.programs.insert(p.key).second) programs.push_back(p);
            } else if (rec[0] == kWarmMasks && rec[1] == sizeof(WarmMasksRecord)) {
                WarmMasksRecord m; std::memcpy(&m, data.data(), sizeof(m));
                if (s_warm.masks.insert(m.base).second) { s_exact_reads.try_emplace(m.base, m.reads); }
            } else if (rec[0] == kWarmPipeline && rec[1] == sizeof(WarmPipelineRecord)) {
                WarmPipelineRecord p; std::memcpy(&p, data.data(), sizeof(p));
                if (p.input.count <= 16 && p.ps_uint_mask <= 0xFF && p.ps_sint_mask <= 0xFF &&
                    s_warm.pipelines.insert(warm_hash(&p, sizeof(p))).second) pipelines.push_back(p);
            }
        }
        std::fclose(f);
    }
    // Programs: cached translation -> the same link the first draw would do.
    const auto warm_clock = std::chrono::steady_clock::now();
    auto warm_seconds = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - warm_clock).count(); };
    // Take the translations from the preload (one pass over the cache)
    // instead of opening each file again while it runs.
    start_translation_preload(dir);
    wait_translation_preload(dir, s_warm_progress ? +[](double f) { s_warm_progress(0.5 * f); } : nullptr);
    RBREW_LOGF(stderr, "[warm] translation preload ready after %.2f s\n", warm_seconds());
    double load_seconds = 0, link_seconds = 0, last_progress = 0;
    size_t loaded = 0, linked = 0, visited = 0;
    // Keys with byte-identical translations share one interned body and link
    // to the same program (link_program dedups on exactly these inputs), so
    // each body is parsed and linked once per (rect, points).
    struct BodyLink { std::shared_ptr<const std::vector<uint8_t>> body; uint32_t rect, points; Program* prog; };
    std::unordered_map<const void*, std::vector<BodyLink>> body_links;
    for (const auto& p : programs) {
        ++visited;
        if (s_warm_progress && warm_seconds() - last_progress >= 0.05) {
            last_progress = warm_seconds();
            s_warm_progress(0.5 + 0.5 * double(visited) / double(programs.size()));
        }
        if (s_prog_cache.count(p.key)) continue;
        bool absent = false;
        auto body = preloaded_translation(dir, p.key, absent);
        Program* prog = nullptr;
        if (body) if (auto links = body_links.find(body.get()); links != body_links.end())
            for (const auto& link : links->second) if (link.rect == p.rect && link.points == p.points) prog = link.prog;
        if (prog) {
            ++loaded; ++linked;
            release_preloaded_translation(dir, p.key);
            if (!prog->warm_key) prog->warm_key = p.key;
            s_prog_cache[p.key] = prog;
            continue;
        }
        std::array<R700TranslationInfo, 2> info;
        char* vs = nullptr; char* ps = nullptr;
        const double load_start = warm_seconds();
        const bool found = load_translation(p.key, vs, ps, info);
        load_seconds += warm_seconds() - load_start;
        if (!found) continue;
        ++loaded;
        bool reused = false;
        const double link_start = warm_seconds();
        prog = link_program(vs, ps, info, p.rect, p.points, false, reused, true);
        link_seconds += warm_seconds() - link_start;
        std::free(vs); std::free(ps);
        if (!prog) continue;
        ++linked;
        if (body) body_links[body.get()].push_back({body, p.rect, p.points, prog});
        if (!prog->warm_key) prog->warm_key = p.key;
        s_prog_cache[p.key] = prog;
    }
    body_links.clear();   // releases the last references to linked bodies
    RBREW_LOGF(stderr, "[warm] %zu programs listed, %zu loaded (%.2f s), %zu linked (%.2f s), loop %.2f s\n",
               programs.size(), loaded, load_seconds, linked, link_seconds, warm_seconds());
    for (const auto& p : pipelines) s_warm.pending.push_back(p);
    warm_poll();
    RBREW_LOGF(stderr, "[warm] %zu pipelines queued, total %.2f s\n", pipelines.size(), warm_seconds());
    // Rewrite the file without duplicates, then append to it from now on.
    bool rewritten = false;
    if (std::FILE* f = std::fopen((path + ".tmp").c_str(), "wb")) {
        const uint32_t header[2] = {kWarmMagic, kWarmVersion};
        std::fwrite(header, 4, 2, f);
        s_warm.file = f;
        for (const auto& p : programs) warm_append(kWarmProgram, &p, sizeof(p));
        for (const auto& [base, reads] : s_exact_reads) { WarmMasksRecord m{}; m.base = base; m.reads = reads; warm_append(kWarmMasks, &m, sizeof(m)); }
        for (const auto& p : pipelines) warm_append(kWarmPipeline, &p, sizeof(p));
        std::fclose(f);
        s_warm.file = nullptr;
        std::error_code ec;
        std::filesystem::rename(path + ".tmp", path, ec);
        rewritten = !ec;
        if (ec) std::filesystem::remove(path + ".tmp", ec);
    }
    // Append only to a file that starts with a valid header.
    if (rewritten || valid_file) s_warm.file = std::fopen(path.c_str(), "ab");

}
void warm_reset() {
    if (s_warm.file) std::fclose(s_warm.file);
    s_warm = WarmState{};
}

// ---------------------------------------------------------------------------
// Vertex buffers (write-watch cached like the GPU VBO cache)
// ---------------------------------------------------------------------------
struct CachedVertexBuffer {
    gx2vk::Buffer buffer;
    const uint8_t* mem = nullptr;
    uint64_t written = 0, last_use = 0, last_serial = 0;
    bool valid = false;
};
std::unordered_map<uint64_t, CachedVertexBuffer> s_vertex_buffers;
uint64_t s_vertex_clock = 0;
uint64_t s_vertex_bytes = 0;        // bytes held by cached mesh copies
size_t s_vertex_evict_bucket = 0;   // next bucket an over-budget cache inspects
std::vector<uint64_t> s_vertex_stale;
// Sized from system RAM (owner request: use what the machine has). A full
// cache drops entries unused for `window` lookups; at most that many can be
// recent, so a count-triggered scan frees at least half.
struct VertexCacheLimits { size_t entries; uint64_t window, bytes; };
#ifdef RBREW_GX2_QUEUE_TESTS
std::optional<VertexCacheLimits> s_test_vertex_limits;
#endif
const VertexCacheLimits& vertex_cache_limits() {
#ifdef RBREW_GX2_QUEUE_TESTS
    if (s_test_vertex_limits) return *s_test_vertex_limits;
#endif
    static const VertexCacheLimits limits = [] {
        const uint64_t ram = gx2_host_total_ram();
        const size_t entries = ram >= (24ull << 30) ? 262144 : ram >= (12ull << 30) ? 131072 : 65536;
        const uint64_t bytes = ram ? ram / 4 : (1ull << 30);   // a quarter of physical RAM
        RBREW_LOGF(stderr, "[mem] vertex cache: %zu entries, %llu MB budget (system RAM %llu MB)\n", entries,
                     (unsigned long long)(bytes >> 20), (unsigned long long)(ram >> 20));
        return VertexCacheLimits{entries, entries / 2, bytes};
    }();
    return limits;
}
size_t vertex_cache_entries() { return s_vertex_buffers.size(); }
uint64_t vertex_cache_bytes() { return s_vertex_bytes; }
gx2vk::Buffer s_null_vertex;   // (0,0,0,0x3F800000): GPU's default generic attribute bits

struct BufferRef { VkBuffer buffer = VK_NULL_HANDLE; VkDeviceSize offset = 0, size = 0; };
struct IndexKey {
    const uint8_t* mem = nullptr;
    uint64_t range = 0; // guest address and byte count
    uint32_t type = 0;  // native width and source endian
    bool operator==(const IndexKey&) const = default;
};
struct IndexKeyHash {
    size_t operator()(const IndexKey& k) const noexcept {
        return size_t(k.range ^ (k.range >> 29) ^ uintptr_t(k.mem) ^ (uint64_t(k.type) * 0x9E3779B97F4A7C15ull));
    }
};
struct CachedIndices { gx2vk::Span span; uint64_t written = 0; };
std::unordered_map<IndexKey, CachedIndices, IndexKeyHash> s_index_spans;
uint64_t s_index_chunk = 0;

#ifndef RBREW_DISABLE_LOGGING
// RBREW_CACHE_VERIFY=1 (diagnostic copies): every write-watch cache hit is
// compared with guest memory. "pending" = the OS still holds an uncollected
// write (data changed after the last query); otherwise the write was missed.
bool cache_verify_enabled() {
    static const bool on = [] { const char* v = rbrew_diagnostic_env("RBREW_CACHE_VERIFY"); return v && v[0] == '1'; }();
    return on;
}
void cache_verify(const char* kind, const uint8_t* mem, uint32_t addr, uint32_t size, const uint8_t* cached,
                  const uint8_t* current, uint64_t cached_written, uint64_t written) {
    static uint64_t hits = 0, stale_pending = 0, stale_missed = 0, logged = 0;
    ++hits;
    if (std::memcmp(cached, current, size) != 0) {
        uint32_t first = 0, differ = 0;
        while (first < size && cached[first] == current[first]) ++first;
        for (uint32_t i = first; i < size; ++i) differ += cached[i] != current[i];
        const bool pending = gx2_write_watch_pending(mem, addr, size);
        (pending ? stale_pending : stale_missed)++;
        if (logged++ < 400)
            RBREW_LOGF(stderr, "[cache-verify] STALE %s addr=%08X size=%u first_diff=%u differ=%u cached_gen=%llu gen=%llu pending=%d serial=%llu\n",
                       kind, addr, size, first, differ, (unsigned long long)cached_written, (unsigned long long)written, int(pending),
                       (unsigned long long)gx2vk::recording_serial());
    }
    if (hits % 200000 == 0)
        RBREW_LOGF(stderr, "[cache-verify] hits=%llu stale_pending=%llu stale_missed=%llu\n", (unsigned long long)hits,
                   (unsigned long long)stale_pending, (unsigned long long)stale_missed);
}
#endif
BufferRef cached_vertex_buffer(uint8_t* mem, uint32_t addr, uint32_t size,uint64_t written) {
    const uint64_t key=uint64_t(addr)<<32|size;
    auto found=s_vertex_buffers.find(key);
    if(found==s_vertex_buffers.end()) {
        // Make room only for a new entry. Evicting on cache hits throws away
        // half of a full, otherwise reusable mesh set every traversal.
        // Over budget, a bounded slice of buckets is visited per new entry
        // instead of the whole map: one pass over up to 262k entries, and
        // destroying everything it freed at the next completion poll, stalled
        // the render thread at once. The eviction rule is unchanged.
        const auto& limits = vertex_cache_limits();
        if (s_vertex_buffers.size() >= limits.entries || s_vertex_bytes > limits.bytes) {
            constexpr size_t kEvictBuckets = 2048;
            const size_t buckets = s_vertex_buffers.bucket_count();
            s_vertex_stale.clear();
            for (size_t n = 0; n < kEvictBuckets && n < buckets; ++n) {
                const size_t bucket = s_vertex_evict_bucket++ % buckets;
                for (auto it = s_vertex_buffers.begin(bucket); it != s_vertex_buffers.end(bucket); ++it)
                    if (it->second.last_use + limits.window < s_vertex_clock) s_vertex_stale.push_back(it->first);
            }
            for (const uint64_t stale : s_vertex_stale) {
                auto it = s_vertex_buffers.find(stale);
                if (it->second.buffer.buffer) s_vertex_bytes -= uint32_t(it->first);
                gx2vk::destroy_buffer(it->second.buffer); s_vertex_buffers.erase(it);
            }
        }
        found=s_vertex_buffers.try_emplace(key).first;
    }
    auto& b=found->second;
    b.last_use = ++s_vertex_clock;
    if (b.valid && b.mem == mem && written != kGx2WriteUntracked && written <= b.written) {
#ifndef RBREW_DISABLE_LOGGING
        if (cache_verify_enabled() && b.buffer.memory.mapped)
            cache_verify("vertex", mem, addr, size, static_cast<const uint8_t*>(b.buffer.memory.mapped), mem + addr, b.written, written);
#endif
        b.last_serial = gx2vk::recording_serial();
        return {b.buffer.buffer, 0, size};
    }
    gx2_perf_bytes(kBytesVertex, size);
    ++C.vertex_uploads;
    if (written == kGx2WriteUntracked) {
        // Untracked arena: stream every draw (GPU STREAM_DRAW re-upload).
        auto span = gx2vk::upload(size, 16);
        if (!span.data) return {};
        std::memcpy(span.data, mem + addr, size);
        b.valid = false;
        return {span.buffer, span.offset, size};
    }
    // Reuse mapped storage only when every recorded read has completed.
    // Otherwise orphan it so earlier draws retain their original bytes.
    // The cached completion is conservative and adds no polling or GPU wait.
    if (!b.buffer.buffer || b.last_serial > gx2vk::known_completed_serial()) {
        if (b.buffer.buffer) s_vertex_bytes -= size;
        gx2vk::destroy_buffer(b.buffer);
        b.buffer = gx2vk::create_buffer(size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, gx2vk::MemoryUse::Upload);
        if (b.buffer.buffer) s_vertex_bytes += size;
    }
    if (!b.buffer.buffer || !b.buffer.memory.mapped) { b.valid = false; return {}; }
    std::memcpy(b.buffer.memory.mapped, mem + addr, size);
    b.valid = true; b.mem = mem; b.written = written; b.last_serial = gx2vk::recording_serial();
    return {b.buffer.buffer, 0, size};
}

VkFormat attribute_format(uint32_t type, uint32_t components) {
    switch (type) {
    case 0x1401: return components == 1 ? VK_FORMAT_R8_UINT : components == 2 ? VK_FORMAT_R8G8_UINT : components == 3 ? VK_FORMAT_R8G8B8_UINT : VK_FORMAT_R8G8B8A8_UINT;
    case 0x1403: return components == 1 ? VK_FORMAT_R16_UINT : components == 2 ? VK_FORMAT_R16G16_UINT : components == 3 ? VK_FORMAT_R16G16B16_UINT : VK_FORMAT_R16G16B16A16_UINT;
    default: return components == 1 ? VK_FORMAT_R32_UINT : components == 2 ? VK_FORMAT_R32G32_UINT : components == 3 ? VK_FORMAT_R32G32B32_UINT : VK_FORMAT_R32G32B32A32_UINT;
    }
}

struct VertexBindings {
    VertexInput input;
    VkBuffer buffers[kMaxVertexInputs]{};
    VkDeviceSize offsets[kMaxVertexInputs]{};
};

// gx2_setup_vertex_attribs: fetch attributes -> one binding per location.
void setup_vertex_attribs(uint8_t* mem, Program& prog, VertexBindings& out) {
    Gx2PerfScope perf_scope(kPerfVertex);
    out.input.count = 0;
    const auto& mapping = prog.info[0].attribute_locations;
    // Locations the vertex shader reads: every semantic with a location, up
    // to the device's vertex inputs (an unbound location reads undefined data).
    const uint32_t input_limit = vertex_input_limit();
    auto it = S.fetch_shaders.find(S.active_fetch);
    if (g_trace_active)
        RBREW_LOGF(stderr, "[ftrace]   fetch program=%08X known=%d attributes=%zu\n", S.active_fetch, int(it != S.fetch_shaders.end()),
                     it != S.fetch_shaders.end() ? it->second.attributes.size() : size_t(0));
    std::array<BufferRef, 32> vbo{};
    std::vector<R700FetchAttribute> none;
    const auto& attributes = it != S.fetch_shaders.end() ? it->second.attributes : none;
    uint32_t used_slots = 0;
    // Fetch shaders describe the mesh's streams, including ones this vertex
    // program never reads (e.g. normals/UVs in a position-only depth pass).
    // Only those with a live shader location need write-watch checks/uploads.
    // The decompiler's location map also declares unmapped fetch semantics;
    // attribute import uses SQ_VTX_SEMANTIC_0..31 to select actual VS inputs.
    std::array<bool,256> imported{};
    for(unsigned i=0;i<32;++i)if(S.regs[0xA0E0+i]<255)imported[S.regs[0xA0E0+i]]=true;
    for (const auto& attr : attributes)
        if (attr.buffer < 32 && attr.semantic < 256 && imported[attr.semantic] && mapping[attr.semantic] >= 0 && uint32_t(mapping[attr.semantic]) < input_limit)
            used_slots |= 1u << attr.buffer;
    std::array<Gx2WriteRange,32> checks{};
    std::array<unsigned,32> checked_slots{};unsigned check_count=0;
    for (uint32_t slot = 0; slot < 32; ++slot) {
        const auto& ab = S.attrib[slot];
        if (!(used_slots & (1u << slot)) || ab.guest_addr == 0 || ab.size == 0) continue;
        if (!rbrew_memory_accessible(mem, ab.guest_addr, ab.size)) continue;
        checks[check_count]={ab.guest_addr,ab.size};checked_slots[check_count++]=slot;
    }
    gx2_write_generations(mem,checks.data(),check_count);
    for(unsigned i=0;i<check_count;++i)
        vbo[checked_slots[i]]=cached_vertex_buffer(mem,checks[i].address,checks[i].size,checks[i].generation);
    struct Bound { uint32_t format_type, components, buffer, offset, stride, rate, divisor; };
    std::array<std::optional<Bound>, kMaxVertexInputs> bound{};
    for (const auto& attr : attributes) {
        uint32_t location = attr.semantic;
        const uint32_t buf_slot = attr.buffer;
        uint32_t offset = attr.offset;
        if (g_trace_active)
            RBREW_LOGF(stderr, "[ftrace]   attr sem=%u loc=%d buf=%u off=%u fmt=%X vbo_addr=%08X size=%u stride=%u\n", location,
                         location < 256 ? mapping[location] : -2, buf_slot, offset, attr.format, buf_slot < 32 ? S.attrib[buf_slot].guest_addr : 0,
                         buf_slot < 32 ? S.attrib[buf_slot].size : 0, buf_slot < 32 ? S.attrib[buf_slot].stride : 0);
        if (buf_slot >= 32 || location >= 256 || mapping[location] < 0) continue;
        location = unsigned(mapping[location]);
        if (location >= input_limit || !vbo[buf_slot].buffer) continue;
        const auto gl = r700_native_attribute(attr.format);
        if (!gl.components) break;   // the GPU loop aborts the remaining attributes
        const auto& ab = S.attrib[buf_slot];
        if (attr.per_instance && S.single_instance_fetch) offset += (S.instance_uniform / std::max(1u, attr.divisor)) * ab.stride;
        const uint32_t divisor = attr.per_instance ? (S.single_instance_fetch ? 1u : std::max(1u, attr.divisor)) : 0;
        bound[location] = Bound{gl.type, gl.components, buf_slot, offset, ab.stride, attr.per_instance ? 1u : 0u, divisor};
    }
    // Every location the shader reads needs a binding in Vulkan; GPU supplies
    // the current generic attribute (0,0,0,1.0f) for disabled arrays.
    for (unsigned semantic = 0; semantic < 256; ++semantic) {
        const int location = mapping[semantic];
        if (location < 0 || uint32_t(location) >= input_limit) continue;
        auto& a = out.input.attrs[out.input.count];
        auto& buffer = out.buffers[out.input.count];
        auto& offset = out.offsets[out.input.count];
        if (bound[location]) {
            const auto& b = *bound[location];
            a = {uint32_t(location), attribute_format(b.format_type, b.components), b.stride, b.rate, std::max(1u, b.divisor)};
            buffer = vbo[b.buffer].buffer;
            offset = vbo[b.buffer].offset + b.offset;
        } else {
            if (!s_null_vertex.buffer) {
                s_null_vertex = gx2vk::create_buffer(16, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, gx2vk::MemoryUse::Upload);
                const uint32_t words[4] = {0, 0, 0, 0x3F800000u};
                std::memcpy(s_null_vertex.memory.mapped, words, 16);
            }
            a = {uint32_t(location), VK_FORMAT_R32G32B32A32_UINT, 0, 0, 1};
            buffer = s_null_vertex.buffer;
            offset = 0;
        }
        ++out.input.count;
        if (out.input.count == input_limit) break;
    }
}

// ---------------------------------------------------------------------------
// Uniforms and descriptors (bind_ubos_to_program)
// ---------------------------------------------------------------------------
gx2vk::Span s_zero_ubo;

gx2vk::Span zero_buffer() {
    if (!gx2vk::span_alive(s_zero_ubo)) {
        s_zero_ubo = gx2vk::upload(65536, 256);
        if (s_zero_ubo.data) std::memset(s_zero_ubo.data, 0, 65536);
    }
    return s_zero_ubo;
}

struct StageDescriptors {
    static constexpr uint32_t kMaxWrites = 1 + 16 + 18;
    static constexpr uint32_t kMaxBuffers = 1 + 16;
    static constexpr uint32_t kMaxImages = 18;
    std::array<VkWriteDescriptorSet, kMaxWrites> writes;
    std::array<VkDescriptorBufferInfo, kMaxBuffers> buffers;
    std::array<VkDescriptorImageInfo, kMaxImages> images;
    uint32_t write_count = 0;
    uint32_t buffer_count = 0;
    uint32_t image_count = 0;
};

bool build_stage(uint8_t* mem, Program& prog, unsigned stage, StageDescriptors& d) {
    const auto& info = prog.info[stage];
    const auto& vk = info.vk;
    d.write_count = d.buffer_count = d.image_count = 0;
    const VkDeviceSize ubo_align = std::max<VkDeviceSize>(gx2vk::caps().limits.minUniformBufferOffsetAlignment, 16);
    for (unsigned slot = 0; slot < 18; ++slot) gx2_render_set_sampler_comparison((stage == 0 ? 32 : 0) + slot, info.texture_compare[slot]);
    // Uniform-variable block (uniforms that GPU keeps on the program).
    if (vk.uniform_vars_binding >= 0) {
        auto& block = prog.ufblock_scratch[stage];
        block.assign(std::max<uint32_t>(vk.uniform_block_bytes, 16), '\0');
        auto put = [&](int32_t offset, const void* data, size_t bytes) {
            if (offset >= 0 && size_t(offset) + bytes <= block.size()) std::memcpy(block.data() + offset, data, bytes);
        };
        if (info.uniform_mode == 1 && vk.offset_remapped >= 0) {
            const size_t remapped_bytes=info.remapped.size()*16;
            // Zero-filled holes and the complete-range check match the old
            // temporary vector plus put(). Pack directly into the final block.
            if(size_t(vk.offset_remapped)<=block.size() && remapped_bytes<=block.size()-size_t(vk.offset_remapped)) {
                char* values=block.data()+vk.offset_remapped;
                for(const auto& entry:info.remapped) {
                    if(entry.mapped_index>=info.remapped.size())continue;
                    char* dst=values+entry.mapped_index*16;
                    if(entry.is_register && entry.index<256)
                        std::memcpy(dst,S.uniform_words[stage].data()+entry.index*4,16);
                    else if(!entry.is_register && entry.bank<16) {
                        const auto& source=S.uniform_blocks[stage][entry.bank];
                        const uint64_t offset=uint64_t(entry.index)*16;
                        if(!source.address || offset+16>source.size)continue;
                        std::memcpy(dst,mem+source.address+uint32_t(offset),16);
                    }
                }
                if(stage==1 && g_trace_active && remapped_bytes) {
                    float c[4];std::memcpy(c,values,16);
                    RBREW_LOGF(stderr,"[ftrace] PS remapped c0=(%g %g %g %g)\n",c[0],c[1],c[2],c[3]);
                }
            }
        } else if (info.uniform_mode == 2 && vk.offset_registers >= 0) {
            put(vk.offset_registers, S.uniform_words[stage].data(), size_t(std::min(vk.register_vec4s, 256)) * 16);
        }
        if (vk.offset_alpha_ref >= 0) { const uint32_t w = S.regs[0xA10E]; put(vk.offset_alpha_ref, &w, 4); }
        if (vk.offset_point_size >= 0) put(vk.offset_point_size, &S.raster.point_size, 4);
        // Window-space vertex exports (viewport scaling disabled) convert
        // window coordinates through 2/viewportSize, taken from the guest
        // viewport scale registers (Cemu VulkanRendererCore semantics).
        if (vk.offset_window_to_clip >= 0) {
            float xs, ys; std::memcpy(&xs, &S.regs[0xA10F], 4); std::memcpy(&ys, &S.regs[0xA111], 4);
            const float t[2] = {xs != 0.0f ? 1.0f / xs : 0.0f, ys != 0.0f ? -1.0f / ys : 0.0f};
            put(vk.offset_window_to_clip, t, 8);
        }
        // Native framebuffer coordinates keep console size; both back-ends
        // write uf_fragCoordScale = (1,1).
        if (vk.offset_frag_coord_scale >= 0) { const float s[4] = {1, 1, 0, 0}; put(vk.offset_frag_coord_scale, s, 16); }
        // Texel-coordinate fetches (guest SAMPLE with unnormalized coordinates
        // or LD) multiply the guest's raw texel coordinates by this scale.
        // Without a texture-pack replacement the bound texture keeps the
        // console resolution, so the factor is 1:1 (Cemu's
        // LatteTexture_getEffectiveTextureScale; the GPU back-end binds the
        // same value). A zero factor collapses every such fetch onto texel 0,0.
        for (unsigned t = 0; t < 18; ++t)
            if (vk.offset_tex_scale[t] >= 0) { const float scale[2] = {1, 1}; put(vk.offset_tex_scale[t], scale, 8); }
        auto& cached = prog.ufblock_cache[stage];
        auto& span = prog.ufblock_span[stage];
        const bool changed=cached!=block;
        if (changed || !gx2vk::span_alive(span)) {
            if (changed) ++C.uniform_uploads;
            span = gx2vk::upload(block.size(), ubo_align);
            if (!span.data) return false;
            std::memcpy(span.data, block.data(), block.size());
            cached.swap(block);
        }
        auto& buffer = d.buffers[d.buffer_count++];
        buffer = {span.buffer, span.offset, cached.size()};
        auto& w = d.writes[d.write_count++];
        w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstBinding = uint32_t(vk.uniform_vars_binding);
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.pBufferInfo = &buffer;
    }
    // Guest uniform blocks (FULL_CBANK).
    std::array<Gx2WriteRange,16> block_checks{};
    std::array<unsigned,16> checked_banks{};unsigned block_check_count=0;
    std::array<uint64_t,16> block_generations{};
    for(unsigned bank=0;bank<16;++bank) {
        const auto& block=S.uniform_blocks[stage][bank];const int binding=info.buffer_bindings[bank];
        if(vk.uniform_buffer_bindings[bank]<0 || binding<0 || binding>=48 || !block.address || !block.size ||
           !rbrew_memory_accessible(mem,block.address,block.size))continue;
        block_checks[block_check_count]={block.address,block.size};checked_banks[block_check_count++]=bank;
    }
    gx2_write_generations(mem,block_checks.data(),block_check_count);
    for(unsigned i=0;i<block_check_count;++i)block_generations[checked_banks[i]]=block_checks[i].generation;
    for (unsigned bank = 0; bank < 16; ++bank) {
        if (vk.uniform_buffer_bindings[bank] < 0) continue;
        const int gl_binding = info.buffer_bindings[bank];
        auto& block = S.uniform_blocks[stage][bank];
        bool bound_now = false;
        if (gl_binding >= 0 && gl_binding < 48 && block.address && block.size && rbrew_memory_accessible(mem, block.address, block.size)) {
            const uint64_t written = block_generations[bank];
            const bool same_binding = block.uploaded && block.uploaded_mem == mem && block.uploaded_address == block.address && block.uploaded_size == block.size;
            const bool verify = !same_binding || written == kGx2WriteUntracked || block.written == kGx2WriteUntracked || written > block.written;
#ifndef RBREW_DISABLE_LOGGING
            if (!verify && cache_verify_enabled() && block.uploaded_bytes.size() == block.size)
                cache_verify("uniform", mem, block.address, block.size, block.uploaded_bytes.data(), mem + block.address, block.written, written);
#endif
            if (verify) {
                const bool changed = !same_binding || block.uploaded_bytes.size() != block.size ||
                                     std::memcmp(block.uploaded_bytes.data(), mem + block.address, block.size) != 0;
                if (changed) {
                    gx2_perf_bytes(kBytesUniformBlock, block.size); ++C.uniform_buffer_uploads;
                    block.uploaded_bytes.assign(mem + block.address, mem + block.address + block.size);
                    block.span = {};
                }
                block.uploaded = true; block.uploaded_mem = mem; block.uploaded_address = block.address; block.uploaded_size = block.size;
                block.written = written;
            }
            if (!gx2vk::span_alive(block.span)) {
                // Resident copy of the uploaded bytes (re-staged when its ring chunk recycled).
                const VkDeviceSize declared = VkDeviceSize(vk.uniform_buffer_vec4s[bank]) * 16;
                const VkDeviceSize bytes = std::max<VkDeviceSize>(std::max<VkDeviceSize>(block.uploaded_bytes.size(), declared), 16);
                block.span = gx2vk::upload(bytes, ubo_align);
                if (!block.span.data) return false;
                std::memcpy(block.span.data, block.uploaded_bytes.data(), block.uploaded_bytes.size());
                if (bytes > block.uploaded_bytes.size()) std::memset(block.span.data + block.uploaded_bytes.size(), 0, bytes - block.uploaded_bytes.size());
            }
            S.ubo_binding_points[gl_binding] = block.span;
            bound_now = true;
        }
        // GPU leaves the binding point unchanged when the block is unset.
        gx2vk::Span span = gl_binding >= 0 && gl_binding < 48 ? S.ubo_binding_points[gl_binding] : gx2vk::Span{};
        if (!bound_now && !gx2vk::span_alive(span)) span = zero_buffer();
        if (!span.buffer) return false;
        const VkDeviceSize range = std::min<VkDeviceSize>(std::max<VkDeviceSize>(span.size, 16), gx2vk::caps().limits.maxUniformBufferRange);
        auto& buffer = d.buffers[d.buffer_count++];
        buffer = {span.buffer, span.offset, range};
        auto& w = d.writes[d.write_count++];
        w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstBinding = uint32_t(vk.uniform_buffer_bindings[bank]);
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.pBufferInfo = &buffer;
    }
    // Textures: GPU unit = stage base + unit, object of the shader's target.
    for (unsigned t = 0; t < 18; ++t) {
        if (vk.texture_bindings[t] < 0) continue;
        const uint32_t unit = (stage == 0 ? 32 : 0) + t;
        const uint32_t dim = vk.texture_dim[t];
        const bool integer = vk.texture_integer[t], shadow = vk.texture_shadow[t];
        refresh_bound_render_texture(unit, mem);
        const uint32_t target_index = dim == 3 ? 1 : dim == 5 ? 2 : dim == 4 ? 3 : 0;
        TexObject* tex = dim == 1 || dim == 0 ? nullptr : S.units[unit][target_index];
#ifdef RBREW_TEXTURE_DIAGNOSTICS
        TexObject* const diagnostic_original=tex;
#endif
        // Sampler slots follow the native slot order: PS 0-17, VS 18-35, GS 36-53.
        const auto& sampler = S.samplers[(stage == 0 ? 18 : 0) + t];
        TexParams params = tex ? tex->params : TexParams{};
        if (tex && sampler.valid) params = sampler.params;   // a bound GPU sampler object overrides texture parameters
        bool complete = tex && tex->image && tex->image->image && tex->complete_storage;
        // GPU: integer textures are incomplete unless both filters are NEAREST.
        if (complete && tex->integer &&
            (params.mag_filter != wire::NEAREST || (params.min_filter != wire::NEAREST && params.min_filter != wire::NEAREST_MIPMAP_NEAREST)))
            complete = false;
        if (complete && shadow && !tex->depth) {
            if (TexObject* copy = shadow_depth_copy(*tex)) tex = copy;
            else complete = false;
        }
        if (complete && integer && !tex->integer && tex->compressed) complete = false;
        if (complete && !integer && tex->integer) complete = false;
        if (!complete) { tex = dummy_texture(dim ? dim : 2, integer, false, shadow); params = tex->params; }
        VkImageView view = tex_object_view(*tex, dim ? dim : 2, integer, false);
        if (!view) { tex = dummy_texture(dim ? dim : 2, integer, false, shadow); params = tex->params; view = tex_object_view(*tex, dim ? dim : 2, integer, false); }
        const VkSampler s = tex_object_sampler(params, shadow, integer || tex->integer);
        TXDIAG(txdiag::binding(prog.id,stage,unit,uint32_t(vk.texture_bindings[t]),dim,complete,diagnostic_original,tex,txdiag::handle(view),txdiag::handle(s)));
        auto& image = d.images[d.image_count++];
        image = {s, view, VK_IMAGE_LAYOUT_GENERAL};
        auto& w = d.writes[d.write_count++];
        w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstBinding = uint32_t(vk.texture_bindings[t]);
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &image;
    }
    return true;
}

void bind_descriptors(VkCommandBuffer cb, Program& prog, VkWriteDescriptorSet* writes, uint32_t write_count) {
    if (!write_count) { s_bindings.descriptor_layout = VK_NULL_HANDLE; return; }
    if (same_descriptors(prog.layout, writes, write_count)) { TXDIAG(txdiag::count(txdiag::DescriptorHits));return; }
    TXDIAG(txdiag::count(txdiag::DescriptorUpdates));
#ifdef RBREW_TEXTURE_DIAGNOSTICS
    const bool diagnostic_update=txdiag::sample("descriptor_update",prog.id);
    auto diagnostic_descriptors=[&](VkDescriptorSet set) {
        if(!diagnostic_update)return;
        const auto update_id=txdiag::next_id();
        txdiag::event("descriptor_update","\"update_id\":%llu,\"program\":%u,\"push\":%s,\"command_buffer\":%llu,\"set\":%llu,\"layout\":%llu,\"pool\":%llu,\"count\":%u",update_id,prog.id,prog.push?"true":"false",txdiag::handle(cb),txdiag::handle(set),txdiag::handle(prog.layout),prog.push?0:txdiag::handle(s_descriptor_pool),write_count);
        for(unsigned i=0;i<write_count;++i) {
            const auto& w=writes[i];
            if(w.pImageInfo)txdiag::event("descriptor_image","\"update_id\":%llu,\"binding\":%u,\"view_id\":%llu,\"sampler_id\":%llu,\"layout\":%u",update_id,w.dstBinding,txdiag::resource_id("view",txdiag::handle(w.pImageInfo->imageView)),txdiag::resource_id("sampler",txdiag::handle(w.pImageInfo->sampler)),uint32_t(w.pImageInfo->imageLayout));
            if(w.pBufferInfo)txdiag::event("descriptor_buffer","\"update_id\":%llu,\"binding\":%u,\"buffer\":%llu,\"offset\":%llu,\"range\":%llu",update_id,w.dstBinding,txdiag::handle(w.pBufferInfo->buffer),w.pBufferInfo->offset,w.pBufferInfo->range);
        }
    };
#endif
    if (prog.push) {
        TXDIAG(diagnostic_descriptors(VK_NULL_HANDLE));
        vkCmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, prog.layout, 0, write_count, writes);
        remember_descriptors(prog.layout, writes, write_count);
        return;
    }
    // Fallback for sets above maxPushDescriptors: per-submission pools.
    if (!s_descriptor_pool || s_descriptor_pool_serial != gx2vk::recording_serial()) {
        if (s_descriptor_pool) s_retired_pools.push_back({s_descriptor_pool_serial, s_descriptor_pool});
        const uint64_t done = gx2vk::completed_serial();
        for (auto it = s_retired_pools.begin(); it != s_retired_pools.end();) {
            if (it->first <= done) {
                TXDIAG(txdiag::event("descriptor_pool_destroy","\"pool\":%llu,\"last_serial\":%llu,\"gpu_completed\":%llu",txdiag::handle(it->second),it->first,done));
                vkDestroyDescriptorPool(gx2vk::device(), it->second, nullptr); it = s_retired_pools.erase(it);
            }
            else ++it;
        }
        const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4096}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096}};
        VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        info.maxSets = 1024; info.poolSizeCount = 2; info.pPoolSizes = sizes;
        vkCreateDescriptorPool(gx2vk::device(), &info, nullptr, &s_descriptor_pool);
        TXDIAG(txdiag::event("descriptor_pool_create","\"id\":%llu,\"pool\":%llu",txdiag::next_id(),txdiag::handle(s_descriptor_pool)));
        s_descriptor_pool_serial = gx2vk::recording_serial();
    }
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = s_descriptor_pool;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &prog.set_layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(gx2vk::device(), &alloc, &set) != VK_SUCCESS) {
        TXDIAG(txdiag::event("descriptor_allocation_failed","\"pool\":%llu,\"program\":%u",txdiag::handle(s_descriptor_pool),prog.id));
        // Pool exhausted within one submission: start another.
        s_retired_pools.push_back({s_descriptor_pool_serial, s_descriptor_pool});
        s_descriptor_pool = VK_NULL_HANDLE;
        bind_descriptors(cb, prog, writes, write_count);
        return;
    }
    for (uint32_t i = 0; i < write_count; ++i) writes[i].dstSet = set;
    TXDIAG(diagnostic_descriptors(set));
    vkUpdateDescriptorSets(gx2vk::device(), write_count, writes, 0, nullptr);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, prog.layout, 0, 1, &set, 0, nullptr);
    remember_descriptors(prog.layout, writes, write_count);
}

// ---------------------------------------------------------------------------
// Dynamic state from the mirrored GPU state
// ---------------------------------------------------------------------------
bool is_triangle_topology(VkPrimitiveTopology t) {
    return t == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST || t == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP || t == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN ||
           t == VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;   // QUADS assemble into triangles in the GS
}

bool apply_dynamic_state(VkCommandBuffer cb, VkPrimitiveTopology topology, bool indexed_restart) {
    const auto& g = S.raster;
    const auto area = s_area;
    // Viewport: GPU UPPER_LEFT (guest top at row 0) = negative-height Vulkan
    // viewport; LOWER_LEFT (guest h < 0) = positive height.
    if (g.viewport[2] <= 0 || g.viewport[3] <= 0) return false;
    VkViewport vp{};
    if (g.clip_origin == wire::UPPER_LEFT) { vp.x = float(g.viewport[0]); vp.y = float(g.viewport[1] + g.viewport[3]); vp.width = float(g.viewport[2]); vp.height = -float(g.viewport[3]); }
    else { vp.x = float(g.viewport[0]); vp.y = float(g.viewport[1]); vp.width = float(g.viewport[2]); vp.height = float(g.viewport[3]); }
    vp.minDepth = float(g.depth_range[0]); vp.maxDepth = float(g.depth_range[1]);
    if (!s_dyn.viewport_valid || std::memcmp(&vp, &s_dyn.viewport, sizeof(vp)) != 0) { vkCmdSetViewport(cb, 0, 1, &vp); s_dyn.viewport = vp; s_dyn.viewport_valid = true; }
    VkRect2D sc = area;
    if (g.scissor_test) {
        const int32_t x0 = std::max(g.scissor[0], 0), y0 = std::max(g.scissor[1], 0);
        const int32_t x1 = std::min<int32_t>(g.scissor[0] + g.scissor[2], int32_t(area.extent.width));
        const int32_t y1 = std::min<int32_t>(g.scissor[1] + g.scissor[3], int32_t(area.extent.height));
        if (x1 <= x0 || y1 <= y0) return false;   // empty scissor box: nothing rasterizes
        sc = {{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
    }
    if (!s_dyn.scissor_valid || std::memcmp(&sc, &s_dyn.scissor, sizeof(sc)) != 0) { vkCmdSetScissor(cb, 0, 1, &sc); s_dyn.scissor = sc; s_dyn.scissor_valid = true; }
    const VkCullModeFlags cull = !g.cull ? VK_CULL_MODE_NONE : g.cull_face == wire::FRONT ? VK_CULL_MODE_FRONT_BIT :
                                 g.cull_face == wire::BACK ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_FRONT_AND_BACK;
    if (cull != s_dyn.cull) { vkCmdSetCullMode(cb, cull); s_dyn.cull = cull; }
    // GPU facing does not depend on the clip origin (probe); Vulkan's flips
    // with the viewport's sign, so invert it for the positive-height case.
    bool ccw = g.front_face == wire::CCW;
    if (g.clip_origin != wire::UPPER_LEFT) ccw = !ccw;
    const VkFrontFace front = ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    if (front != s_dyn.front) { vkCmdSetFrontFace(cb, front); s_dyn.front = front; }
    const int depth_test = g.depth_test ? 1 : 0, depth_write = g.depth_write ? 1 : 0;
    if (depth_test != s_dyn.depth_test) { vkCmdSetDepthTestEnable(cb, depth_test); s_dyn.depth_test = depth_test; }
    if (depth_write != s_dyn.depth_write) { vkCmdSetDepthWriteEnable(cb, depth_write); s_dyn.depth_write = depth_write; }
    const VkCompareOp depth_op = gl_compare_to_vk(g.depth_func);
    if (depth_op != s_dyn.depth_op) { vkCmdSetDepthCompareOp(cb, depth_op); s_dyn.depth_op = depth_op; }
    const int stencil = g.stencil_test ? 1 : 0;
    if (stencil != s_dyn.stencil_test) { vkCmdSetStencilTestEnable(cb, stencil); s_dyn.stencil_test = stencil; }
    for (unsigned f = 0; f < 2; ++f) {
        const auto& s = f ? g.back : g.front;
        DynamicState::Face face{vk_stencil_op(s.fail), vk_stencil_op(s.zpass), vk_stencil_op(s.zfail), gl_compare_to_vk(s.func), s.mask, s.write_mask, s.ref};
        auto& old = s_dyn.face[f];
        const VkStencilFaceFlags flag = f ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
        if (!s_dyn.face_valid || old.fail != face.fail || old.pass != face.pass || old.zfail != face.zfail || old.op != face.op)
            vkCmdSetStencilOp(cb, flag, face.fail, face.pass, face.zfail, face.op);
        if (!s_dyn.face_valid || old.compare != face.compare) vkCmdSetStencilCompareMask(cb, flag, face.compare);
        if (!s_dyn.face_valid || old.write != face.write) vkCmdSetStencilWriteMask(cb, flag, face.write);
        if (!s_dyn.face_valid || old.ref != face.ref) vkCmdSetStencilReference(cb, flag, face.ref);
        old = face;
    }
    s_dyn.face_valid = true;
    // GL_POLYGON_OFFSET_FILL applies to filled polygons only.
    const int bias = g.polygon_offset_fill && is_triangle_topology(topology) ? 1 : 0;
    if (bias != s_dyn.depth_bias) { vkCmdSetDepthBiasEnable(cb, bias); s_dyn.depth_bias = bias; }
    const float clamp = gx2vk::caps().depth_bias_clamp ? g.offset_clamp : 0.0f;
    if (s_dyn.bias[0] != g.offset_units || s_dyn.bias[1] != clamp || s_dyn.bias[2] != g.offset_factor) {
        vkCmdSetDepthBias(cb, g.offset_units, clamp, g.offset_factor);
        s_dyn.bias[0] = g.offset_units; s_dyn.bias[1] = clamp; s_dyn.bias[2] = g.offset_factor;
    }
    if (std::memcmp(s_dyn.blend, g.blend_color.data(), 16) != 0) { vkCmdSetBlendConstants(cb, g.blend_color.data()); std::memcpy(s_dyn.blend, g.blend_color.data(), 16); }
    const int restart = indexed_restart ? 1 : 0;
    if (restart != s_dyn.restart) { vkCmdSetPrimitiveRestartEnable(cb, restart); s_dyn.restart = restart; }
    return true;
}

VkPrimitiveTopology vk_topology(uint32_t prim) {
    switch (prim & 31) {
    case 0x01: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case 0x02: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case 0x03: case 0x0D: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;   // LINE_LOOP closes through its index list
    case 0x05: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    case 0x06: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case 0x13: case 0x14: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;   // native assembly GS
    default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;   // TRIANGLES and RECT (GS)
    }
}
bool list_topology(VkPrimitiveTopology t) {
    return t == VK_PRIMITIVE_TOPOLOGY_POINT_LIST || t == VK_PRIMITIVE_TOPOLOGY_LINE_LIST || t == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST ||
           t == VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
}

// ---------------------------------------------------------------------------
// Frame trace and per-draw diagnostic queries
// ---------------------------------------------------------------------------
void frame_trace_begin_query(VkCommandBuffer cb) {
    if (s_trace_query_open || !s_open_queries.empty()) return;   // native queries already measure these draws
    if (s_trace_query == UINT32_MAX) s_trace_query = allocate_query();
    vkResetQueryPool(gx2vk::device(), query_pool(s_trace_query), s_trace_query % kQueriesPerPool, 1);
    vkCmdBeginQuery(cb, query_pool(s_trace_query), s_trace_query % kQueriesPerPool, gx2vk::caps().precise_occlusion ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    s_trace_query_open = true;
}
uint32_t frame_trace_end_query(VkCommandBuffer cb) {
    if (!s_trace_query_open) return ~0u;
    vkCmdEndQuery(cb, query_pool(s_trace_query), s_trace_query % kQueriesPerPool);
    s_trace_query_open = false;
    gx2vk::finish();
    uint64_t passed = 0;
    vkGetQueryPoolResults(gx2vk::device(), query_pool(s_trace_query), s_trace_query % kQueriesPerPool, 1, 8, &passed, 8,
                          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    return uint32_t(passed);
}

void frame_trace_draw(Program& prog) {
    if (!g_trace_active) return;
    const auto& g = S.raster;
    RBREW_LOGF(stderr, "[ftrace] draw#%u fbo=%u prog=%u vs=%08X ps=%08X vp=%d,%d,%dx%d blend=%d cw=%d depth=%d\n", s_frame_draw_index - 1, S.current_fbo_id, prog.id,
                 S.cur_vs, S.cur_ps,
                 g.viewport[0], g.viewport[1], g.viewport[2], g.viewport[3], int(g.blend), int(S.color_write), int(g.depth_test));
    // Raw sampler words of every unit the pixel stage reads: word0 carries the
    // filters and the depth-compare function, word1 the LOD clamps and word2
    // FORCE_DEGAMMA/high-precision flags. These identify a material's sampling
    // contract without a capture.
    for (unsigned t = 0; t < 18; ++t) if (prog.info[1].vk.texture_bindings[t] >= 0) {
        const uint32_t* w = &S.regs[0xF000 + t * 3];
        RBREW_LOGF(stderr, "[ftrace]   ps tex%u sampler=%08X %08X %08X compare=%d\n", t, w[0], w[1], w[2], int(S.sampler_compare[t]));
    }
    RBREW_LOGF(stderr, "[ftrace]   depth_func=%X write=%d alpha_control=%X\n", g.depth_func, int(g.depth_write), S.regs[0xA104]);
    RBREW_LOGF(stderr, "[ftrace]   scissor=%u %d,%d,%dx%d masks=%08X depth_size=%ux%u\n", unsigned(g.scissor_test), g.scissor[0], g.scissor[1],
                 g.scissor[2], g.scissor[3], S.target_masks, S.depth_target ? S.depth_target->width : 0, S.depth_target ? S.depth_target->height : 0);
    for (const auto& [key, e] : S.cb_map) for (unsigned i = 0; i < 8; ++i) if (S.color_targets[i] == &e && (S.target_masks & (15u << (4 * i))))
        RBREW_LOGF(stderr, "[ftrace]   target%u image=%08X fmt=%X size=%ux%u mip=%u slice=%u aa=%u\n", i, key[0], key[1], e.width, e.height, key[5], key[6], key[8]);
    for (unsigned i = 0; i < S.bound_textures.size(); ++i) if (S.bound_textures[i].address) {
        const auto* d = S.bound_textures[i].descriptor.data();
        RBREW_LOGF(stderr, "[ftrace]   texture%u image=%08X fmt=%X size=%ux%u mip=%u slice=%u\n", i, descriptor_word(d, GX2SURF_IMAGE),
                     descriptor_word(d, GX2SURF_FORMAT), descriptor_word(d, GX2SURF_WIDTH), descriptor_word(d, GX2SURF_HEIGHT),
                     descriptor_word(d, GX2TEX_VIEW_MIP), descriptor_word(d, GX2TEX_VIEW_FIRST_SLICE));
    }
}

void frame_trace_after_draw(uint32_t prim, uint32_t count, uint32_t fragments) {
    if (!g_trace_active) return;
    s_last_trace_fragments = fragments;
    const auto& g = S.raster;
    auto* target = S.framebuffer.colors[0];
    if (!target || g.viewport[2] <= 0 || g.viewport[3] <= 0) return;
    end_rendering();
    std::vector<uint8_t> raw;
    if (!gx2vk::read_image(target->image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, target->width, target->height, raw)) return;
    const uint32_t texel = gx2vk::format_block_bytes(target->image.desc.format, VK_IMAGE_ASPECT_COLOR_BIT);
    size_t rgb = 0, alpha = 0, samples = 0, bright = 0; uint64_t luma = 0;
    const int x0 = std::max(0, g.viewport[0]), y0 = std::max(0, g.viewport[1]);
    const int x1 = std::min<int>(g.viewport[0] + g.viewport[2], int(target->width)), y1 = std::min<int>(g.viewport[1] + g.viewport[3], int(target->height));
    for (int y = y0; y < y1; y += 4) for (int x = x0; x < x1; x += 4) {
        const uint8_t* p = raw.data() + (size_t(y) * target->width + x) * texel;
        ++samples; rgb += (texel >= 3 ? (p[0] | p[1] | p[2]) : p[0]) != 0; alpha += texel >= 4 && p[3] != 0;
        luma += uint32_t(p[0]) + p[1] + p[2];
        bright += (texel >= 3 ? (p[0] | p[1] | p[2]) : p[0]) > 64;
    }
    // mean/bright separate "the material was drawn but shaded dark" from
    // "the draw wrote nothing" - the question a dark-surface report needs.
    RBREW_LOGF(stderr, "[ftrace]   -> prim=%X count=%u fragments=%u nonblack=%zu/%zu alpha>0=%zu mean=%u bright=%zu\n", prim, count, fragments, rgb, samples, alpha,
                 samples ? unsigned(luma / (samples * 3)) : 0u, bright);
}

// ---------------------------------------------------------------------------
// The draw itself
// ---------------------------------------------------------------------------
struct IndexStream { bool indexed = false; gx2vk::Span span; VkIndexType type = VK_INDEX_TYPE_UINT32; uint32_t count = 0; bool restart = false; };

// Native transform feedback for guest stream exports. Copy GPU-produced
// bytes into guest storage at completion so the recompiled CPU, subsequent
// vertex uploads and texture reads observe the same data. No shader values
// are synthesized on the host. A synchronous boundary is conservative;
// asynchronous residency can be added once the original dependency is met.
// The current CPU publication boundary completes each export before the
// next draw, so its scratch storage can be reused safely. Do not allocate,
// initialize and destroy four native buffers on every particle update.
gx2vk::Buffer s_streamout_data[4],s_streamout_counter[4];
VkPhysicalDeviceTransformFeedbackPropertiesEXT s_streamout_limits{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_PROPERTIES_EXT};
bool s_streamout_limits_valid=false;
struct GuestStreamOut {
    gx2vk::Buffer data[4], counter[4];
    VkBuffer counters[4]{};
    VkDeviceSize counter_offsets[4]{};
    uint32_t addresses[4]{}, sizes[4]{}, offsets[4]{}, mask=0,count=0;
    const R700TranslationInfo* info=nullptr;
    uint32_t fragments=~0u;
    bool prepare(uint8_t* mem,const Program& program,bool position_capture,VkPrimitiveTopology topology,uint32_t vertices,uint32_t instances) {
        if(!(S.regs[0xA2AC]&1))return true;
        mask=program.info[0].streamout_mask;
        if(!mask)return true;
        info=&program.info[0];
        if(position_capture || program.rect || !gx2vk::caps().transform_feedback) {
            RBREW_LOGF(stderr,"[gx2] unsupported stream-out stage/capture or missing native transform-feedback capability\n");
            return false;
        }
        if((S.regs[0xA2C8]&mask)!=mask) {
            RBREW_LOGF(stderr,"[gx2] stream-out shader uses a disabled output buffer\n");return false;
        }
        uint64_t assembled=vertices;
        if(topology==VK_PRIMITIVE_TOPOLOGY_LINE_STRIP)assembled=vertices>1?uint64_t(vertices-1)*2:0;
        if(topology==VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP || topology==VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN)
            assembled=vertices>2?uint64_t(vertices-2)*3:0;
        if(!s_streamout_limits_valid) {
            VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};props.pNext=&s_streamout_limits;
            vkGetPhysicalDeviceProperties2(gx2vk::physical_device(),&props);
            s_streamout_limits_valid=true;
        }
        const auto& tf=s_streamout_limits;
        for(unsigned i=0;i<4;++i) {
            if(!(mask&(1u<<i)))continue;
            count=i+1;
            addresses[i]=S.regs[0xA2B6+i*4]<<8;
            sizes[i]=S.regs[0xA2B4+i*4]<<2;
            offsets[i]=S.regs[0xA2B7+i*4];
            if(!addresses[i] || !sizes[i] || !rbrew_memory_accessible(mem,addresses[i],sizes[i]) ||
               (offsets[i]&3)) {
                RBREW_LOGF(stderr,"[gx2] invalid stream-out buffer %u address=%08X size=%u offset=%u\n",i,addresses[i],sizes[i],offsets[i]);
                return false;
            }
            const uint32_t stride=info->streamout_strides[i];
            const uint64_t capacity=std::max<uint64_t>(sizes[i],uint64_t(offsets[i])+assembled*instances*stride);
            if(!stride || stride>tf.maxTransformFeedbackBufferDataStride || capacity>tf.maxTransformFeedbackBufferSize || capacity>UINT32_MAX) {
                RBREW_LOGF(stderr,"[gx2] stream-out exceeds native device limits\n");return false;
            }
            // Hardware clips writes to each guest buffer independently but
            // advances the byte offset for the full export. Native scratch
            // storage accommodates the full GPU export before bounded publication.
            if(s_streamout_data[i].size<capacity) {
                gx2vk::destroy_buffer(s_streamout_data[i]);
                s_streamout_data[i]=gx2vk::create_buffer(capacity,VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT,gx2vk::MemoryUse::Readback);
            }
            if(!s_streamout_counter[i].buffer)
                s_streamout_counter[i]=gx2vk::create_buffer(4,VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT,gx2vk::MemoryUse::Readback);
            data[i]=s_streamout_data[i];counter[i]=s_streamout_counter[i];
            if(!data[i].memory.mapped || !counter[i].memory.mapped)return false;
            // Only the shader's written words are published below. Copying
            // the entire guest allocation here wastes bandwidth and cannot
            // contribute to those GPU-written words or preserved holes.
            std::memcpy(counter[i].memory.mapped,&offsets[i],4);
            counters[i]=counter[i].buffer;
        }
        return true;
    }
    void begin(VkCommandBuffer cb) {
        if(!mask)return;
        VkBuffer buffers[4]{};VkDeviceSize begin_offsets[4]{},ranges[4]{};
        unsigned first=0;while(!(mask&(1u<<first)))++first;
        for(unsigned i=0;i<count;++i) {
            // Unused shader slots still need a valid native binding in the
            // contiguous range. They alias storage the shader never writes.
            const unsigned binding=(mask&(1u<<i))?i:first;
            buffers[i]=data[binding].buffer;ranges[i]=data[binding].size;
        }
        vkCmdBindTransformFeedbackBuffersEXT(cb,0,count,buffers,begin_offsets,ranges);
        vkCmdBeginTransformFeedbackEXT(cb,0,count,counters,counter_offsets);
    }
    bool finish(VkCommandBuffer cb,uint8_t* mem) {
        if(!mask)return true;
        vkCmdEndTransformFeedbackEXT(cb,0,count,counters,counter_offsets);
        const bool trace=s_trace_query_open;
        end_rendering();
        VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        barrier.srcStageMask=VK_PIPELINE_STAGE_2_TRANSFORM_FEEDBACK_BIT_EXT;
        barrier.srcAccessMask=VK_ACCESS_2_TRANSFORM_FEEDBACK_WRITE_BIT_EXT|VK_ACCESS_2_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT;
        barrier.dstStageMask=VK_PIPELINE_STAGE_2_HOST_BIT;barrier.dstAccessMask=VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};dep.memoryBarrierCount=1;dep.pMemoryBarriers=&barrier;
        vkCmdPipelineBarrier2(cb,&dep);
        {
            Gx2PerfScope wait_scope(kPerfSubmitWait);
            if(!gx2vk::finish())return false;
        }
        if(trace) {
            uint64_t passed=0;
            vkGetQueryPoolResults(gx2vk::device(),query_pool(s_trace_query),s_trace_query%kQueriesPerPool,1,8,&passed,8,VK_QUERY_RESULT_64_BIT);
            fragments=uint32_t(passed);
        }
        for(unsigned i=0;i<count;++i)if(mask&(1u<<i)) {
            uint32_t written;std::memcpy(&written,counter[i].memory.mapped,4);
            if(written<offsets[i] || written>data[i].size) {
                RBREW_LOGF(stderr,"[gx2] stream-out counter outside buffer %u: %u\n",i,written);return false;
            }
            const uint32_t end=std::min(written,sizes[i]),stride=info->streamout_strides[i];
            static const bool xfb_log=rbrew_diagnostic_env("RBREW_XFB_LOG")!=nullptr;
            if(xfb_log)
                RBREW_LOGF(stderr,"[xfb] publish slot=%u base=%08X start=%u end=%u written=%u\n",i,addresses[i],offsets[i],end,written);
            const auto& words=info->streamout_written_words[i];
            for(uint32_t byte=offsets[i];byte<end;) {
                const uint32_t word=((byte-offsets[i])%stride)/4;
                if(word>=words.size() || !words[word]){byte+=4;continue;}
                const uint32_t start=byte;byte+=4;
                while(byte<end) {
                    const uint32_t next=((byte-offsets[i])%stride)/4;
                    if(next>=words.size() || !words[next])break;
                    byte+=4;
                }
                rbrew_write_memory(mem,addresses[i]+start,data[i].memory.mapped+start,byte-start);
            }
            if(end>offsets[i])gx2_render_invalidate_textures(addresses[i]+offsets[i],end-offsets[i]);
            S.regs[0xA2B7+i*4]=written;
        }
        return true;
    }
};

static bool trace_skip(const char* reason) {
    TXDIAG(txdiag::count(txdiag::Skipped);
        if(txdiag::sample("draw_skip",txdiag::hash(reason,std::strlen(reason))))txdiag::event("draw_skip","\"reason\":%s,\"vs\":%u,\"ps\":%u,\"framebuffer\":%u",txdiag::quote(reason).c_str(),S.cur_vs,S.cur_ps,S.current_fbo_id));
    if (g_trace_active)
        RBREW_LOGF(stderr, "[ftrace] draw skipped: %s fb.valid=%d depth_only=%d depth=%p color0=%p masks=%X depth_target=%p vp=%d,%d,%dx%d\n", reason,
                     int(S.framebuffer.valid), int(S.framebuffer.depth_only), (void*)S.framebuffer.depth, (void*)S.framebuffer.colors[0],
                     S.target_masks, (void*)S.depth_target, S.raster.viewport[0], S.raster.viewport[1], S.raster.viewport[2], S.raster.viewport[3]);
    return false;
}

// Per-frame tally for the shadow log: how many passes touch color vs depth
// only, which is how a light-space shadow pass shows up without a trace.
static uint32_t s_shadow_depth_only_draws, s_shadow_color_draws, s_shadow_frames_logged;

bool issue_draw(uint8_t* mem, uint32_t prim, uint32_t count, uint32_t first_vertex, const IndexStream& indices, bool xfb,
                void (*hook)(bool, VkCommandBuffer)) {
    TXDIAG(txdiag::count(txdiag::Draws));
    Program* prog = resolve_active_program(mem);
    if (!prog) return trace_skip("no-program");
    s_last_program = prog->id;
    TXDIAG(if(txdiag::sample("raster_state",uint64_t(prog->id)<<32|S.current_fbo_id))
        txdiag::event("raster_state","\"program\":%u,\"framebuffer\":%u,\"color_write\":%s,\"blend\":%s,\"blend_src\":%u,\"blend_dst\":%u,\"depth_test\":%s,\"depth_write\":%s,\"depth_func\":%u,\"cull\":%s,\"front_face\":%u,\"scissor_on\":%s,\"scissor\":[%d,%d,%d,%d]",prog->id,S.current_fbo_id,S.color_write?"true":"false",S.raster.blend?"true":"false",S.raster.blend_src_rgb,S.raster.blend_dst_rgb,S.raster.depth_test?"true":"false",S.raster.depth_write?"true":"false",S.raster.depth_func,S.raster.cull?"true":"false",S.raster.front_face,S.raster.scissor_test?"true":"false",S.raster.scissor[0],S.raster.scissor[1],S.raster.scissor[2],S.raster.scissor[3]));
    TXDIAG(if(txdiag::sample("program_identity",prog->id,txdiag::journal().epoch.load(),UINT64_MAX)) {
        txdiag::event("program_identity","\"program\":%u,\"warm_key\":%llu,\"vs_source_hash\":%llu,\"ps_source_hash\":%llu,\"gs_source_hash\":%llu,\"descriptor_layout\":%llu,\"pipeline_layout\":%llu",prog->id,prog->warm_key,txdiag::hash(prog->vs_source.data(),prog->vs_source.size()),txdiag::hash(prog->ps_source.data(),prog->ps_source.size()),txdiag::hash(prog->gs_source.data(),prog->gs_source.size()),txdiag::handle(prog->set_layout),txdiag::handle(prog->layout));
        const std::string prefix="e"+std::to_string(txdiag::journal().epoch.load())+"-program-"+std::to_string(prog->id);
        txdiag::artifact(prefix+"-vs.glsl",prog->vs_source);txdiag::artifact(prefix+"-ps.glsl",prog->ps_source);txdiag::artifact(prefix+"-gs.glsl",prog->gs_source);
    });
    TXDIAG(if(txdiag::sample("draw_program",uint64_t(prog->id)<<32|S.current_fbo_id))
        txdiag::event("draw_program","\"program\":%u,\"vs\":%u,\"ps\":%u,\"framebuffer\":%u,\"primitive\":%u,\"count\":%u,\"masks\":%u,\"viewport\":[%d,%d,%d,%d]",prog->id,S.cur_vs,S.cur_ps,S.current_fbo_id,prim,count,S.target_masks,S.raster.viewport[0],S.raster.viewport[1],S.raster.viewport[2],S.raster.viewport[3]));
    const bool streamout_only=prog->info[0].streamout_mask && (S.regs[0xA2AC]&1) && (S.regs[0xA204]&(1u<<22));
    if (!ensure_rendering(streamout_only)) return trace_skip("ensure_rendering");
    if (!streamout_only) {
        const bool depth_only_pass = s_active_fb.depth && !(S.target_masks & 1u);
        if (depth_only_pass) ++s_shadow_depth_only_draws; else ++s_shadow_color_draws;
        if (shadow_log() && depth_only_pass && s_shadow_depth_only_draws <= 6)
            RBREW_LOGF(stderr, "[shadow] depth-only draw prog=%u masks=%X depth=%ux%u gx2fmt=%X test=%d write=%d func=%X vp=%d,%d,%dx%d\n",
                         prog->id, S.target_masks, s_active_fb.depth->width, s_active_fb.depth->height, s_active_fb.depth->gx2_format,
                         int(S.raster.depth_test), int(S.raster.depth_write), S.raster.depth_func, S.raster.viewport[0], S.raster.viewport[1], S.raster.viewport[2],
                         S.raster.viewport[3]);
    }
    const VkPrimitiveTopology topology = vk_topology(prim);
    // Pipeline formats and blend state from the bound framebuffer.
    PipelineRequest r{};
    r.program = prog;
    r.topology = topology;
    r.xfb = xfb;
    uint32_t uint_mask = 0, sint_mask = 0;
    const auto& fb = s_active_fb;
    const uint32_t outputs = prog->info[1].vk.pixel_outputs;
    for (unsigned i = 0; i < 8; ++i) {
        auto& b = r.blend[i];
        b = {};
        auto* t = fb.colors[i];
        r.colors[i] = t ? t->fmt.attachment : VK_FORMAT_UNDEFINED;
        if (!t) continue;
        const bool integer = t->fmt.integer;
        if (integer && (outputs >> i & 1)) { if (t->gx2_format == 0x322u) sint_mask |= 1u << i; else uint_mask |= 1u << i; }
        // GPU ignores blending for integer buffers.
        const bool blendable = S.raster.blend && !integer && gx2vk::format_supports(t->fmt.attachment, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT);
        b.blendEnable = S.raster.blend && blendable;
        if (b.blendEnable) {
            b.srcColorBlendFactor = vk_blend(S.raster.blend_src_rgb); b.dstColorBlendFactor = vk_blend(S.raster.blend_dst_rgb);
            b.colorBlendOp = vk_blend_op(S.raster.blend_eq_rgb);
            b.srcAlphaBlendFactor = vk_blend(S.raster.blend_src_alpha); b.dstAlphaBlendFactor = vk_blend(S.raster.blend_dst_alpha);
            b.alphaBlendOp = vk_blend_op(S.raster.blend_eq_alpha);
        }
        // Outputs the pixel shader does not write keep the attachment's contents.
        b.colorWriteMask = (outputs >> i & 1) ? S.raster.color_mask[i] : 0;
        if (xfb) b.colorWriteMask = 0;   // replay position capture leaves the image unchanged
    }
    r.depth = fb.depth ? fb.depth->format : VK_FORMAT_UNDEFINED;
    r.stencil = fb.depth && fb.depth->stencil;
    r.ps_uint_mask = uint_mask; r.ps_sint_mask = sint_mask;
    VkCommandBuffer cb = gx2vk::cmd();
    if (s_dynamic_serial != gx2vk::recording_serial()) reset_command_tracking();
    const bool restart = indices.indexed && indices.restart;
    if (!apply_dynamic_state(cb, topology, restart)) return trace_skip("dynamic-state");
    frame_trace_draw(*prog);
    const unsigned mode = prim & 31;
    const uint32_t loops = S.draw_first_instance ? S.draw_instances : 1;
    bool any = false;
    uint32_t trace_fragments = 0;   // the GPU trace query spans the whole draw call
    for (uint32_t i = 0; i < loops; ++i) {
        // GPU offsets instance attributes after division; Latte divides the
        // effective instance index, so nonzero base instances draw singly.
        S.instance_uniform = S.draw_first_instance + i;
        VertexBindings vb;
        setup_vertex_attribs(mem, *prog, vb);
        r.input = vb.input;
        VkPipeline pipeline = get_pipeline(r);
        if (!pipeline) return any;
        StageDescriptors sd[2];
        if (!build_stage(mem, *prog, 0, sd[0]) || !build_stage(mem, *prog, 1, sd[1])) return any;
        // Refreshing a sampled depth array can end rendering for its copies.
        if (!ensure_rendering(streamout_only)) return any;
        // A trace readback or capture may have submitted since the state was
        // applied, and ensure_rendering() resets tracking for a new command
        // buffer: re-apply (only differences against the tracked values
        // are recorded).
        cb = gx2vk::cmd();
        if (s_dynamic_serial != gx2vk::recording_serial()) reset_command_tracking();
        if (!apply_dynamic_state(cb, topology, restart)) return any;
        if (pipeline != s_bound_pipeline) { vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline); s_bound_pipeline = pipeline; }
        std::array<VkWriteDescriptorSet, StageDescriptors::kMaxWrites * 2> writes;
        const uint32_t write_count = sd[0].write_count + sd[1].write_count;
        std::copy_n(sd[0].writes.data(), sd[0].write_count, writes.data());
        std::copy_n(sd[1].writes.data(), sd[1].write_count, writes.data() + sd[0].write_count);
        bind_descriptors(cb, *prog, writes.data(), write_count);
        const int32_t base_instance = int32_t(S.instance_uniform);
        bind_instance(cb, prog->layout, base_instance);
        bind_vertices(cb, vb.input.count, vb.buffers, vb.offsets);
        const uint32_t instances = S.draw_first_instance ? 1 : S.draw_instances;
        GuestStreamOut streamout;
        if(!streamout.prepare(mem,*prog,xfb,topology,indices.indexed?indices.count:count,instances))return any;
        if (g_trace_active) frame_trace_begin_query(cb);
        if (hook) hook(true, cb);
        streamout.begin(cb);
        if (indices.indexed) {
            vkCmdBindIndexBuffer(cb, indices.span.buffer, indices.span.offset, indices.type);
            vkCmdDrawIndexed(cb, indices.count, instances, 0, int32_t(first_vertex), 0);
        } else {
            vkCmdDraw(cb, count, instances, first_vertex, 0);
        }
        if (hook) hook(false, cb);
        if(!streamout.finish(cb,mem))return any;
        ++C.draws;
        {
            const uint32_t n = indices.indexed ? indices.count : count;
            uint64_t prims = 0;
            switch (topology) {
            case VK_PRIMITIVE_TOPOLOGY_POINT_LIST: prims = n; break;
            case VK_PRIMITIVE_TOPOLOGY_LINE_LIST: prims = n / 2; break;
            case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP: prims = n > 1 ? n - 1 : 0; break;
            case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN: prims = n > 2 ? n - 2 : 0; break;
            case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY: prims = n / 4; break;
            default: prims = n / 3; break;
            }
            s_submitted_primitives += prims * instances;
        }
        const bool discarded=(S.regs[0xA2AC]&1) && (S.regs[0xA204]&(1u<<22));
        for (unsigned attachment = 0; attachment < 8; ++attachment)
            if (!discarded && fb.colors[attachment] && r.blend[attachment].colorWriteMask)
                gx2vk::note_gpu_write(fb.colors[attachment]->image);
        if (!discarded && fb.depth && ((S.raster.depth_test && S.raster.depth_write) || S.raster.stencil_test))
            gx2vk::note_gpu_write(fb.depth->image);
        gx2vk::note_gpu_write();
        any = true;
        if (g_trace_active) {
            const uint32_t fragments = streamout.mask?streamout.fragments:frame_trace_end_query(cb);
            trace_fragments = fragments == ~0u || trace_fragments == ~0u ? ~0u : trace_fragments + fragments;
            if (i + 1 == loops) frame_trace_after_draw(prim, count, trace_fragments);
        }
    }
    (void)mode;
    return any;
}

void (*s_xfb_hook)(bool, VkCommandBuffer) = nullptr;
bool s_xfb_pipeline = false;
}  // namespace

// Per-frame shadow tally; the counters live in the anonymous namespace above.
void gx2r::shadow_log_frame_end() {
    if (!shadow_log()) return;
    if (s_shadow_depth_only_draws || s_shadow_frames_logged < 8)
        RBREW_LOGF(stderr, "[shadow] frame: color draws=%u depth-only draws=%u (depth_test=%d write=%d func=%X fbo=%u)\n", s_shadow_color_draws,
                     s_shadow_depth_only_draws, int(S.raster.depth_test), int(S.raster.depth_write), S.raster.depth_func, S.current_fbo_id);
    ++s_shadow_frames_logged;
    s_shadow_depth_only_draws = s_shadow_color_draws = 0;
}

// Replay transform-feedback control (gx2_render_vk_capture.cpp).
void gx2r_set_xfb_hook(void (*hook)(bool, VkCommandBuffer)) { s_xfb_hook = hook; s_xfb_pipeline = hook != nullptr; }
uint32_t gx2r_last_trace_fragments() { return s_last_trace_fragments; }
void gx2r_reset_trace_fragments() { s_last_trace_fragments = ~0u; }
uint32_t gx2r_frame_draw_index() { return s_frame_draw_index; }
size_t gx2r_vertex_cache_entries() { return vertex_cache_entries(); }
size_t gx2r_native_query_count() { return s_native_queries.size(); }
uint64_t gx2r_vertex_cache_bytes() { return vertex_cache_bytes(); }
void gx2r_frame_draw_index_reset() { s_frame_draw_index = 0; }
uint32_t gx2r_next_frame_draw() { return s_frame_draw_index++; }

void gx2r_draw_report() {
    static unsigned s_copies;
    if (++s_copies % 120 != 0) return;
    RBREW_LOGF(stderr, "[draw] last 120 scan copies: draws=%u no_program=%u (no_vs_or_ps=%u)\n", s_draws_requested, s_draws_no_program, s_draws_no_shader_set);
    s_draws_requested = s_draws_no_program = s_draws_no_shader_set = 0;
    RBREW_LOGF(stderr, "[draw]   draws by fbo:");
    for (auto& [fbo, n] : s_draws_per_fbo) RBREW_LOGF(stderr, " %u:%u", fbo, n);
    RBREW_LOGF(stderr, "\n");
    s_draws_per_fbo.clear();
}

// Big-endian guest indices -> native order, 16 bytes per SSSE3 shuffle (the
// renderer converts about 1.7 million indices per busy plaza frame). Same
// bytes as the scalar conversion, which still handles the tail and CPUs
// without SSSE3.
static bool has_ssse3() {
    static const bool ssse3 = [] { int info[4]{}; __cpuid(info, 1); return (info[2] & (1 << 9)) != 0; }();
    return ssse3;
}
static void copy_swapped_indices(uint8_t* dst, const uint8_t* src, uint32_t count, bool is_u32) {
    const size_t bytes = size_t(count) * (is_u32 ? 4 : 2);
    size_t i = 0;
    if (has_ssse3()) {
        const __m128i order = is_u32 ? _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12)
                                     : _mm_setr_epi8(1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14);
        for (; i + 16 <= bytes; i += 16)
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i),
                             _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i)), order));
    }
    if (is_u32) for (; i < bytes; i += 4) { dst[i] = src[i + 3]; dst[i + 1] = src[i + 2]; dst[i + 2] = src[i + 1]; dst[i + 3] = src[i]; }
    else for (; i < bytes; i += 2) { dst[i] = src[i + 1]; dst[i + 1] = src[i]; }
}

extern bool gx2r_capture_draw_if_armed(bool indexed, uint32_t prim, uint32_t count, uint32_t index_type, uint32_t index_addr,
                                       uint32_t first_vertex, uint8_t* mem);

void gx2_render_draw(uint32_t prim_type, uint32_t count, uint32_t first_vertex, uint8_t* mem, uint32_t instances, uint32_t first_instance) {
    Gx2PerfScope perf_scope(kPerfDraw);
    if (!S.conditional_draw || !gx2vk::ready()) {
        TXDIAG(trace_skip(!S.conditional_draw?"conditional-render":"device-not-ready"));return;
    }
    S.draw_instances = instances; S.draw_first_instance = first_instance; S.instance_uniform = first_instance;
    S.single_instance_fetch = first_instance != 0;
    S.regs[0x2256] = prim_type & 31;
    gx2r_capture_draw_if_armed(false, prim_type, count, 0, 0, first_vertex, mem);
    if (!count || !instances) return;
    static unsigned array_trace = 0; static const bool array_trace_on = rbrew_diagnostic_env("RBREW_DRAW_TRACE") != nullptr;
    if (array_trace_on && array_trace++ < 32)
        RBREW_LOGF(stderr, "[draw] arrays primitive=%X count=%u first=%u instances=%u first_instance=%u\n", prim_type, count, first_vertex, instances, first_instance);
    IndexStream indices;
    const unsigned mode = prim_type & 31;
    if (mode == 0x13 || mode == 0x14 || mode == 0x0D) {
        // QUADS/QUAD_STRIP: adjacency quadruplets for the assembly GS (0-based,
        // base vertex = first); LINE_LOOP: strip closed by its first vertex.
        std::vector<uint32_t> expanded;
        if (mode == 0x0D) { for (uint32_t v = 0; v < count; ++v) expanded.push_back(v); if (count > 1) expanded.push_back(0); }
        else for (unsigned v = 0; v + 3 < count; v += mode == 0x13 ? 4 : 2) {
            if (mode == 0x13) expanded.insert(expanded.end(), {v, v + 1, v + 2, v + 3});
            else expanded.insert(expanded.end(), {v, v + 1, v + 3, v + 2});
        }
        if (expanded.empty()) return;
        indices.indexed = true;
        indices.count = uint32_t(expanded.size());
        indices.span = gx2vk::upload(expanded.size() * 4, 16);
        if (!indices.span.data) return;
        std::memcpy(indices.span.data, expanded.data(), expanded.size() * 4);
    }
    issue_draw(mem, prim_type, count, first_vertex, indices, s_xfb_pipeline, s_xfb_hook);
}

void gx2_render_draw_indexed(uint32_t prim_type, uint32_t count, uint32_t index_type, uint32_t idx_guest_addr, uint32_t first_vertex,
                             uint8_t* mem, uint32_t instances, uint32_t first_instance) {
    Gx2PerfScope perf_scope(kPerfDraw);
    if (!S.conditional_draw || !gx2vk::ready()) {
        TXDIAG(trace_skip(!S.conditional_draw?"conditional-render":"device-not-ready"));return;
    }
    S.draw_instances = instances; S.draw_first_instance = first_instance; S.instance_uniform = first_instance;
    S.single_instance_fetch = first_instance != 0;
    S.regs[0x2256] = prim_type & 31;
    gx2r_capture_draw_if_armed(true, prim_type, count, index_type, idx_guest_addr, first_vertex, mem);
    if (!instances) return;
    const bool is_u32 = index_type == GX2_INDEX_TYPE_U32_BE || index_type == GX2_INDEX_TYPE_U32_LE;
    const bool need_swap = index_type == GX2_INDEX_TYPE_U16_BE || index_type == GX2_INDEX_TYPE_U32_BE;
    const uint32_t restart = S.raster.restart_index & (is_u32 ? 0xFFFFFFFFu : 0xFFFFu);
    const uint64_t idx_bytes64 = uint64_t(count) * (is_u32 ? 4u : 2u);
    if (idx_bytes64 > UINT32_MAX) return;
    const uint32_t idx_bytes = uint32_t(idx_bytes64);
    if (idx_guest_addr == 0 || !count || !rbrew_memory_accessible(mem, idx_guest_addr, idx_bytes)) return;
    static unsigned draw_trace = 0;
    static const bool s_draw_trace = rbrew_diagnostic_env("RBREW_DRAW_TRACE") != nullptr;
    if (s_draw_trace && draw_trace++ < 32)
        RBREW_LOGF(stderr, "[draw] indexed primitive=%X count=%u type=%u base=%u indices=%08X instances=%u first_instance=%u\n", prim_type, count,
                     index_type, first_vertex, idx_guest_addr, instances, first_instance);
    if (g_trace_active) {
        RBREW_LOGF(stderr, "[ftrace]   indexed prim=%X count=%u index_type=%u raw:", prim_type, count, index_type);
        for (uint32_t i = 0; i < idx_bytes && i < 16; ++i) RBREW_LOGF(stderr, " %02X", mem[idx_guest_addr + i]);
        RBREW_LOGF(stderr, "\n");
    }
    Gx2PerfScope index_scope(kPerfIndex);
    auto index = [&](unsigned i) -> uint32_t {
        const uint8_t* p = mem + idx_guest_addr + i * (is_u32 ? 4 : 2);
        if (is_u32) return need_swap ? rbrew_read32(mem, idx_guest_addr + i * 4) : uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
        return need_swap ? uint32_t(p[0]) << 8 | p[1] : uint32_t(p[0]) | uint32_t(p[1]) << 8;
    };
    const unsigned mode = prim_type & 31;
    const bool restart_on = S.raster.primitive_restart;
    const VkPrimitiveTopology topology = vk_topology(prim_type);
    IndexStream stream;
    stream.indexed = true;
    // Ordinary indexed primitives need no expansion. Preserve their native
    // width and fill the mapped upload directly, without a heap-backed u32
    // vector or a second copy. Fixed restart uses the sentinel of that width.
    const bool direct = mode != 0x13 && mode != 0x14 && mode != 0x0D &&
        (!restart_on || (restart == (is_u32 ? UINT32_MAX : 0xFFFFu) &&
                        (!list_topology(topology) || gx2vk::caps().list_restart)));
    if (direct) {
        stream.count = count;
        stream.type = is_u32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
        stream.restart = restart_on;
        const IndexKey key{mem, uint64_t(idx_guest_addr) << 32 | idx_bytes, index_type};
        const auto written = gx2_write_generation(mem, idx_guest_addr, idx_bytes);
        const auto found = s_index_spans.find(key);
        if (written != kGx2WriteUntracked && found != s_index_spans.end() &&
            written <= found->second.written && gx2vk::span_alive(found->second.span)) {
            stream.span = found->second.span;
#ifndef RBREW_DISABLE_LOGGING
            if (cache_verify_enabled() && stream.span.data) {
                static thread_local std::vector<uint8_t> guest;
                guest.resize(idx_bytes);
                if (!need_swap) std::memcpy(guest.data(), mem + idx_guest_addr, idx_bytes);
                else copy_swapped_indices(guest.data(), mem + idx_guest_addr, count, is_u32);
                cache_verify("index", mem, idx_guest_addr, idx_bytes, static_cast<const uint8_t*>(stream.span.data), guest.data(),
                             found->second.written, written);
            }
#endif
        } else {
            stream.span = gx2vk::upload(idx_bytes, 16);
            if (!stream.span.data) return;
            gx2_perf_bytes(kBytesIndex, idx_bytes);
            if (!need_swap) std::memcpy(stream.span.data, mem + idx_guest_addr, idx_bytes);
            else copy_swapped_indices(stream.span.data, mem + idx_guest_addr, count, is_u32);
            if (written != kGx2WriteUntracked && stream.span.chunk_id) {
                if (s_index_chunk != stream.span.chunk_id) {
                    // Metadata shares the existing ring's lifetime; no extra
                    // geometry allocation or permanent CPU byte copy is kept.
                    std::erase_if(s_index_spans, [](const auto& item) { return !gx2vk::span_resident(item.second.span); });
                    s_index_chunk = stream.span.chunk_id;
                }
                s_index_spans[key] = {stream.span, written};
            }
        }
        index_scope.stop();
        issue_draw(mem, prim_type, count, first_vertex, stream, s_xfb_pipeline, s_xfb_hook);
        return;
    }
    std::vector<uint32_t> out;
    out.reserve(count + 8);
    if (mode == 0x13 || mode == 0x14) {
        // Respect restart boundaries before reordering quad-strip pairs; each
        // segment contributes whole quads, so the restart index itself is not
        // needed in the adjacency list.
        const bool strip = mode == 0x14;
        std::vector<uint32_t> segment;
        auto emit = [&] {
            for (unsigned i = 0; i + 3 < segment.size(); i += strip ? 2 : 4) {
                const uint32_t a = segment[i], b = segment[i + 1], c = segment[i + 2], d = segment[i + 3];
                if (strip) out.insert(out.end(), {a, b, d, c}); else out.insert(out.end(), {a, b, c, d});
            }
            segment.clear();
        };
        for (unsigned i = 0; i < count; ++i) { const uint32_t v = index(i); if (restart_on && v == restart) emit(); else segment.push_back(v); }
        emit();
        stream.restart = false;
    } else {
        // Vulkan restarts at all-ones of a 32-bit index: convert so any GX2
        // restart value (and a non-restart 0xFFFF) keeps its GPU meaning.
        const bool vk_restart = restart_on && (!list_topology(topology) || gx2vk::caps().list_restart);
        uint32_t first = UINT32_MAX;
        bool segment_open = false;
        for (unsigned i = 0; i < count; ++i) {
            const uint32_t v = index(i);
            if (restart_on && v == restart) {
                if (mode == 0x0D && segment_open && first != UINT32_MAX) out.push_back(first);
                segment_open = false; first = UINT32_MAX;
                if (vk_restart) out.push_back(0xFFFFFFFFu);
                continue;
            }
            if (!segment_open) { first = v; segment_open = true; }
            out.push_back(v);
        }
        if (mode == 0x0D && segment_open && first != UINT32_MAX) out.push_back(first);
        stream.restart = vk_restart;
    }
    index_scope.stop();
    if (out.empty()) return;
    stream.count = uint32_t(out.size());
    stream.type = VK_INDEX_TYPE_UINT32;
    stream.span = gx2vk::upload(out.size() * 4, 16);
    if (!stream.span.data) return;
    gx2_perf_bytes(kBytesIndex, out.size() * 4);
    std::memcpy(stream.span.data, out.data(), out.size() * 4);
    issue_draw(mem, prim_type, stream.count, first_vertex, stream, s_xfb_pipeline, s_xfb_hook);
}

// ---------------------------------------------------------------------------
// Native occlusion queries
// ---------------------------------------------------------------------------
namespace {
void close_query_segment() {
    if (!s_query_segment) return;
    if (s_rendering) end_segment_query(gx2vk::cmd());
    s_query_segment->closed = true;
    // Only end_segment_query assigns the submission containing GPU work.
    // An empty segment has no GPU dependency: waiting for the next recording
    // serial would wait for a submission that may never be made.
    if (s_query_segment->indices.empty()) s_query_segment->ready = true;
    s_query_segment.reset();
}
void open_query_segment() {
    if (s_open_queries.empty()) return;
    s_query_segment = std::make_shared<QuerySegment>();
    s_query_segment->serial = gx2vk::recording_serial();
    for (auto* q : s_open_queries) q->segments.push_back(s_query_segment);
    if (s_rendering) begin_segment_query(gx2vk::cmd());
}
bool query_samples(NativeQuery& q, bool wait, uint64_t& total) {
    if (!q.ended) return false;
    total = 0;
    for (auto& segment : q.segments) {
        if (!segment->ready) {
            // Results of commands still being recorded need a submission
            // (the GPU back-end flushes at query end).
            if (segment->serial >= gx2vk::recording_serial()) gx2vk::submit(false);
            if (segment->serial > gx2vk::completed_serial()) {
                if (!wait) return false;
                if (!gx2vk::wait_serial(segment->serial)) return false;
            }
            uint64_t sum = 0;
            for (auto index : segment->indices) {
                uint64_t count = 0;
                const VkResult result = vkGetQueryPoolResults(gx2vk::device(), query_pool(index), index % kQueriesPerPool, 1, 8, &count, 8,
                                                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
                if (result != VK_SUCCESS) return false;
                sum += count;
            }
            segment->samples = sum;
            segment->ready = true;
        }
        total += segment->samples;
    }
    if (!q.published) {
        // Four 16-byte start/end counter pairs. Each GPU uint64 is LE; bit 63
        // is completion. The aggregate goes in RB0, the others are zero.
        std::array<uint8_t, 64> counters{};
        for (unsigned rb = 0; rb < 4; ++rb) {
            const uint64_t start = 1ull << 63, end = (1ull << 63) | (rb ? 0 : total);
            for (unsigned byte = 0; byte < 8; ++byte) {
                counters[rb * 16 + byte] = uint8_t(start >> (byte * 8));
                counters[rb * 16 + 8 + byte] = uint8_t(end >> (byte * 8));
            }
        }
        rbrew_write_memory(q.mem, q.address, counters.data(), uint32_t(counters.size()));
        q.published = true;
        gx2_query_render_published(q.address);
    }
    return true;
}
NativeQuery* find_query(uint8_t* mem, uint32_t address) {
    const auto it = s_native_queries.find({reinterpret_cast<uintptr_t>(mem), address});
    return it == s_native_queries.end() ? nullptr : &it->second;
}
void recycle_deferred_queries() {
    if (s_deferred_free_queries.empty()) return;
    // Conservative: everything submitted so far must be complete.
    if (gx2vk::completed_serial() + 1 < gx2vk::recording_serial()) return;
    for (auto q : s_deferred_free_queries) release_query(q);
    s_deferred_free_queries.clear();
}
}  // namespace

void gx2_render_query_begin(uint32_t type, uint32_t address, uint8_t* mem) {
    if ((type != 0 && type != 2) || !address || !rbrew_memory_accessible(mem, address, 64)) {
        RBREW_LOGF(stderr, "[gx2] Unsupported or invalid query begin type=%u address=%08X\n", type, address); return;
    }
    auto& q = s_native_queries[{reinterpret_cast<uintptr_t>(mem), address}];
    if (q.open) { RBREW_LOGF(stderr, "[gx2] Query %08X already open\n", address); return; }
    close_query_segment();
    q = NativeQuery{mem, address, type, true, false, false, {}};
    gx2_query_render_begin(address);
    const std::array<uint8_t, 64> zero{}; rbrew_write_memory(mem, address, zero.data(), 64);
    s_open_queries.insert(&q); open_query_segment();
}
void gx2_render_query_end(uint32_t type, uint32_t address, uint8_t* mem) {
    auto* q = find_query(mem, address);
    if (!q || !q->open || q->type != type) { RBREW_LOGF(stderr, "[gx2] Query end without matching begin %08X type=%u\n", address, type); return; }
    close_query_segment(); s_open_queries.erase(q); q->open = false; q->ended = true;
    open_query_segment();
}
bool gx2_render_query_result(uint32_t address, uint32_t output, uint8_t* mem) {
    if (!output || !rbrew_memory_accessible(mem, output, 8)) return false;
    rbrew_write64(mem, output, 0);
    auto* q = find_query(mem, address); uint64_t samples = 0;
    if (q && !query_samples(*q, false, samples)) return false;
    if (!address || !rbrew_memory_accessible(mem, address, 64)) return false;
    // GetResult is a CPU data query in gx2.rpl: copied counter blocks and CPU
    // clears remain observable, independently of native query identity.
    samples = 0;
    for (unsigned rb = 0; rb < 4; ++rb) {
        const uint64_t start = rbrew_bswap64(rbrew_read64(mem, address + rb * 16));
        const uint64_t end = rbrew_bswap64(rbrew_read64(mem, address + rb * 16 + 8));
        if (!(start & (1ull << 63)) || !(end & (1ull << 63))) return false;
        samples += end - start;
    }
    rbrew_write64(mem, output, samples); return true;
}
void gx2_render_query_poll() {
    if (!gx2vk::ready()) return;
    for (auto& [key, q] : s_native_queries) if (q.ended && !q.published) { uint64_t count = 0; query_samples(q, false, count); }
    recycle_deferred_queries();
}
void gx2_render_conditional_begin(uint32_t type, uint32_t address, bool no_wait, bool pixels_pass, uint8_t* mem) {
    auto* q = find_query(mem, address); uint64_t samples = 0;
    if ((type != 0 && type != 2) || !q || q->type != type) {
        RBREW_LOGF(stderr, "[gx2] Conditional render has no matching occlusion query %08X\n", address); S.conditional_draw = true; return;
    }
    // NO_WAIT renders when the GPU cannot decide yet; a wait predicate waits
    // only for the contributing segments.
    const bool ready = query_samples(*q, !no_wait, samples);
    S.conditional_draw = !ready || ((samples != 0) == pixels_pass);
    TXDIAG(if(txdiag::sample("conditional",address,uint64_t(S.conditional_draw)))
        txdiag::event("conditional","\"query\":%u,\"ready\":%s,\"samples\":%llu,\"draw\":%s,\"pixels_pass\":%s,\"no_wait\":%s",address,ready?"true":"false",samples,S.conditional_draw?"true":"false",pixels_pass?"true":"false",no_wait?"true":"false"));
}
void gx2_render_conditional_end() { S.conditional_draw = true; }

// GX2Flush / GX2DrawDone (gx2_vk_context.cpp)
void gx2_render_vk_flush() {
    if (!gx2vk::ready()) return;
    end_rendering();
    gx2vk::submit(false);
    gx2_render_query_poll();
}
bool gx2_render_vk_finish() {
    if (!gx2vk::ready()) return false;
    end_rendering();
    const bool ok = gx2vk::finish();
    gx2_render_query_poll();
    return ok;
}

// ---------------------------------------------------------------------------
// Internal transfer pass: depth -> R32F color (GX2CopySurface /
// ConvertDepthBufferToTextureSurface), the GPU back-end's transfer program.
// ---------------------------------------------------------------------------
namespace {
VkDescriptorSetLayout s_transfer_set = VK_NULL_HANDLE;
VkPipelineLayout s_transfer_layout = VK_NULL_HANDLE;
std::map<VkFormat, VkPipeline> s_transfer_pipelines;
const char* kTransferVs = "#version 450\nvoid main(){vec2 p=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);gl_Position=vec4(p*2-1,0,1);}\n";
const char* kTransferPs = "#version 450\nlayout(set=0,binding=0) uniform sampler2D source;layout(location=0) out vec4 value;"
                          "void main(){value=vec4(texelFetch(source,ivec2(gl_FragCoord.xy),0).r,0,0,1);}\n";
}

bool gx2r::depth_to_color_pass(DepthTarget& source, ColorTarget& destination, uint32_t width, uint32_t height) {
    if (!(destination.image.desc.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) return false;
    if (!s_transfer_layout) {
        VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        set.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        set.bindingCount = 1; set.pBindings = &binding;
        vkCreateDescriptorSetLayout(gx2vk::device(), &set, nullptr, &s_transfer_set);
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout.setLayoutCount = 1; layout.pSetLayouts = &s_transfer_set;
        vkCreatePipelineLayout(gx2vk::device(), &layout, nullptr, &s_transfer_layout);
    }
    const VkFormat format = destination.fmt.attachment;
    auto& pipeline = s_transfer_pipelines[format];
    if (!pipeline) {
        VkPipelineShaderStageCreateInfo stages[2]{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = gx2vk::shader_module(VK_SHADER_STAGE_VERTEX_BIT, kTransferVs); stages[0].pName = "main";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = gx2vk::shader_module(VK_SHADER_STAGE_FRAGMENT_BIT, kTransferPs); stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = 1; viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL; raster.lineWidth = 1.0f; raster.cullMode = VK_CULL_MODE_NONE;
        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        VkPipelineColorBlendAttachmentState attachment{};
        attachment.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1; blend.pAttachments = &attachment;
        const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = 2; dynamic.pDynamicStates = dynamic_states;
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = 1; rendering.pColorAttachmentFormats = &format;
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        info.pNext = &rendering; info.stageCount = 2; info.pStages = stages; info.pVertexInputState = &vertex;
        info.pInputAssemblyState = &assembly; info.pViewportState = &viewport; info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample; info.pDepthStencilState = &depth; info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic; info.layout = s_transfer_layout;
        if (!stages[0].module || !stages[1].module ||
            vkCreateGraphicsPipelines(gx2vk::device(), gx2vk::pipeline_cache(), 1, &info, nullptr, &pipeline) != VK_SUCCESS) {
            pipeline = VK_NULL_HANDLE;
            return false;
        }
    }
    end_rendering();
    VkCommandBuffer cb = gx2vk::cmd();
    gx2vk::barrier_if_dirty();
    gx2vk::ViewDesc dv; dv.type = VK_IMAGE_VIEW_TYPE_2D; dv.format = format;
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = gx2vk::image_view(destination.image, dv);
    color.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
    info.renderArea = {{0, 0}, {destination.width, destination.height}};
    info.layerCount = 1; info.colorAttachmentCount = 1; info.pColorAttachments = &color;
    vkCmdBeginRendering(cb, &info);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    const VkViewport vp{0, 0, float(width), float(height), 0, 1};
    const VkRect2D sc{{0, 0}, {std::min(width, destination.width), std::min(height, destination.height)}};
    vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sc);
    gx2vk::ViewDesc sv; sv.type = VK_IMAGE_VIEW_TYPE_2D; sv.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    TexParams nearest; nearest.min_filter = nearest.mag_filter = wire::NEAREST; nearest.max_level = 0;
    VkDescriptorImageInfo image{tex_object_sampler(nearest, false, false), gx2vk::image_view(source.image, sv), VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstBinding = 0; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &image;
    vkCmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_transfer_layout, 0, 1, &w);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    // Binding this static-state pipeline invalidates the guest pipeline's
    // dynamic depth/cull/stencil/restart state as well as viewport/scissor.
    // A following draw in the same command buffer must emit all of it again.
    reset_command_tracking();
    gx2vk::note_gpu_write(destination.image);
    return true;
}

// Startup, on the render thread ahead of every game command: the shader
// warm-up the first draw would run, reporting progress for the splash.
void gx2_render_vk_startup_warmup(void (*progress)(double)) {
    s_warm_progress = progress;
    warm_start();
    s_warm_progress = nullptr;
}

#ifdef RBREW_GX2_QUEUE_TESTS
// One synchronous loading pass over `dir` into a private preload: entry bytes
// by key and {on_disk, from_pack, read, stale, complete, pack_written}.
std::array<size_t, 6> gx2_render_vk_test_translation_preload(const std::string& dir, const std::string& prefix,
                                                               std::map<uint64_t, std::vector<uint8_t>>& entries) {
    TranslationPreload preload;
    const PreloadStats stats = run_translation_preload(preload, dir, prefix);
    entries.clear();
    for (const auto& [key, body] : preload.entries) entries[key] = translation_blob(key, *body);
    return {stats.on_disk, stats.from_pack, stats.read, stats.stale, size_t(stats.complete), size_t(stats.pack_written)};
}
// Distinct bodies a loading pass of `dir` holds (interning check).
size_t gx2_render_vk_test_translation_bodies(const std::string& dir, const std::string& prefix) {
    TranslationPreload preload;
    run_translation_preload(preload, dir, prefix);
    std::set<const void*> bodies;
    for (const auto& [key, body] : preload.entries) bodies.insert(body.get());
    return bodies.size();
}
void gx2_render_vk_test_compile_gate(std::function<void()> gate) { s_compile_test_gate = std::move(gate); }
void gx2_render_vk_test_start_warmup() { warm_start(); }
uint64_t gx2_render_vk_test_pixel_translation_posts() { return s_pixel_translation_posts; }
uint64_t gx2_render_vk_test_key_memo_hits() { return s_exact_key_memo_hits; }
uint64_t gx2_render_vk_test_exact_key() { return s_last_exact_key; }
uint64_t gx2_render_vk_test_canonical_reuses() { return s_canonical_reuses; }
void gx2_render_vk_test_vertex_cache_limits(size_t entries, uint64_t window) {
    if (entries) s_test_vertex_limits = VertexCacheLimits{entries, window, ~0ull}; else s_test_vertex_limits.reset();
}
void gx2_render_vk_test_key_memo_enable(bool enabled) { s_test_key_memo_enabled = enabled; }
std::shared_future<void> gx2_render_vk_test_background_compile(std::function<void()> task) {
    return post_compile(std::move(task), true).result;
}
#endif

// Window close: wait (bounded) for queued translation-cache files, so translations
// made this session are not lost when the process ends without destructors.
void gx2_render_wait_cache_writes(uint32_t timeout_ms) {
    auto& writer = translation_writer();
    std::unique_lock lock(writer.mutex);
    writer.idle.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return writer.queue.empty() && !writer.writing; });
}
uint32_t gx2_render_vk_last_program() { return s_last_program; }
uint64_t gx2_render_vk_submitted_primitives() { return s_submitted_primitives; }

void gx2r::shutdown_draw_state() {
    const auto device = gx2vk::device();
    // Join every compiler before destroying layouts, modules or the device.
    if (s_compile_queue) s_compile_queue->drain();
    s_open_queries.clear();
    s_query_segment.reset();
    s_native_queries.clear();
    for (auto pool : s_query_pools) vkDestroyQueryPool(device, pool, nullptr);
    s_query_pools.clear(); s_free_queries.clear(); s_deferred_free_queries.clear();
    s_query_active = s_trace_query_open = false;
    s_trace_query = UINT32_MAX;
    for (auto& [key, job] : s_pipelines) {
        try { if (auto pipeline = job.result.get()) vkDestroyPipeline(device, pipeline, nullptr); } catch (...) {}
    }
    s_pipelines.clear();
    for (auto& [key, program] : s_prog_by_source) {
        if (program->layout) vkDestroyPipelineLayout(device, program->layout, nullptr);
        if (program->set_layout) vkDestroyDescriptorSetLayout(device, program->set_layout, nullptr);
    }
    s_prog_cache.clear(); s_prog_by_source.clear(); s_exact_reads.clear(); s_exact_key_memo = {}; s_canonical.clear(); s_canonical_reuses = 0; warm_reset(); s_compile_queue.reset(); s_next_program = 1;
    for(unsigned i=0;i<4;++i) {gx2vk::destroy_buffer(s_streamout_data[i]);gx2vk::destroy_buffer(s_streamout_counter[i]);}
    s_streamout_limits_valid=false;
    for (auto& [key, pipeline] : s_transfer_pipelines) if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
    s_transfer_pipelines.clear();
    if (s_transfer_layout) vkDestroyPipelineLayout(device, s_transfer_layout, nullptr);
    if (s_transfer_set) vkDestroyDescriptorSetLayout(device, s_transfer_set, nullptr);
    s_transfer_layout = VK_NULL_HANDLE; s_transfer_set = VK_NULL_HANDLE;
    if (s_descriptor_pool) vkDestroyDescriptorPool(device, s_descriptor_pool, nullptr);
    for (auto& [serial, pool] : s_retired_pools) vkDestroyDescriptorPool(device, pool, nullptr);
    s_descriptor_pool = VK_NULL_HANDLE; s_descriptor_pool_serial = 0; s_retired_pools.clear();
    for (auto& [key, buffer] : s_vertex_buffers) gx2vk::destroy_buffer(buffer.buffer);
    s_vertex_buffers.clear(); s_vertex_bytes = 0; s_vertex_evict_bucket = 0; s_vertex_stale.clear();
    s_index_spans.clear(); s_index_chunk = 0;
    gx2vk::destroy_buffer(s_null_vertex);
    s_zero_ubo = {};
    for (auto& [key, dummy] : s_dummies) gx2vk::destroy_image(dummy.image);
    s_dummies.clear();
    for (auto& [source, copy] : s_shadow_copies) { gx2vk::destroy_image(copy.image); gx2vk::destroy_buffer(copy.staging); }
    s_shadow_copies.clear();
    s_active_fb = {}; s_area = {}; s_rendering = false;
    s_last_program = 0; s_last_trace_fragments = ~0u;
    reset_command_tracking();
}
