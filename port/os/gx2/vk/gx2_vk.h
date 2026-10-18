#pragma once
// gx2_vk.h - Vulkan device layer of the native GX2 backend.
//
// One instance/device/queue, a sub-allocating memory manager, a streaming
// upload ring, serial-tracked submissions (timeline semaphore), deferred
// destruction and the swapchain. Everything here runs on the native render
// thread (gx2_vk_context.cpp) or, before it starts and in tests, under the
// renderer lock - the same single-owner model as the native graphics context.
//
// Synchronization model: every image stays in VK_IMAGE_LAYOUT_GENERAL after
// creation. GPU writes (draws, clears, copies) mark the device dirty; the
// renderer calls barrier_if_dirty() before it starts a rendering instance,
// a transfer or a readback, which records one global memory dependency from
// all prior commands to all later ones. Host writes into mapped memory are
// made visible by queue submission; mapped memory is reused only after the
// submission that read it has completed.
#define VK_NO_PROTOTYPES
#include <volk.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct SDL_Window;

namespace gx2vk {

struct Caps {
    uint32_t api_version = 0;
    std::string device_name;
    VkPhysicalDeviceLimits limits{};
    bool push_descriptor = false;      // core 1.4 or VK_KHR_push_descriptor
    uint32_t max_push_descriptors = 0;
    bool custom_border_color = false;  // VK_EXT_custom_border_color
    bool list_restart = false;         // VK_EXT_primitive_topology_list_restart
    bool transform_feedback = false;   // VK_EXT_transform_feedback (offline replay positions)
    bool vertex_divisor = false;       // vertexAttributeInstanceRateDivisor
    bool vertex_divisor_zero = false;
    bool mirror_clamp_to_edge = false;
    bool anisotropy = false;
    bool depth_bias_clamp = false;
    bool precise_occlusion = false;
    bool geometry_shader = false;
    bool image_cube_array = false;
    bool robust_buffer_access = false;
    bool d24s8 = false;                // D24_UNORM_S8_UINT attachment + sampled
    bool negative_one_to_one = false;  // native GPU/GX2 clip depth in fixed-function viewport
    bool provoking_vertex_last = false; // VK_EXT_provoking_vertex LAST convention (flat shading)
    bool depth_clamp = false;           // depthClamp feature (Latte always clamps depth)
    bool depth_clip_enable = false;     // VK_EXT_depth_clip_enable (PA_CL_CLIP_CNTL ZCLIP_*_DISABLE)
    bool validation = false;           // VK_LAYER_KHRONOS_validation enabled
    bool sync_validation = false;
    float max_anisotropy = 1.0f;
};

bool init(SDL_Window* window, bool native_depth = true); // null: headless; false selects shader depth fallback
void shutdown();
bool ready();
const Caps& caps();
VkInstance instance();
VkPhysicalDevice physical_device();
VkDevice device();
VkQueue queue();
uint32_t queue_family();
// Validation messages of severity error seen since init (tests require 0).
uint64_t validation_error_count();
uint64_t validation_warning_count();
// Message text of the last validation error (tests print it on failure).
std::string last_validation_error();

// ---- memory --------------------------------------------------------------
struct Allocation {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize offset = 0, size = 0;
    uint8_t* mapped = nullptr;           // host pointer at offset (host-visible types)
    int32_t block = -1;                  // allocator block, -1 = dedicated
    uint32_t pool = 0;
    explicit operator bool() const { return memory != VK_NULL_HANDLE; }
};
enum class MemoryUse { DeviceLocal, Upload, Readback };
Allocation allocate(const VkMemoryRequirements& requirements, MemoryUse use, bool linear_resource);
void free_now(Allocation& allocation);
uint64_t allocated_bytes();
struct MemoryUsage {
    uint64_t device_local = 0, host_only = 0, host_visible_device_local = 0;
};
// Application-owned allocations by heap placement, not driver residency or
// shader/pipeline internals. Host-visible VRAM remains part of device_local.
MemoryUsage memory_usage();

// ---- submissions and lifetime -------------------------------------------
// Serial assigned to the command buffer currently being recorded.
uint64_t recording_serial();
uint64_t completed_serial();
// Last completion already observed by the render thread; never polls/waits.
// A stale answer is conservative for deciding whether storage can be reused.
uint64_t known_completed_serial();
bool wait_serial(uint64_t serial, uint64_t timeout_ns = UINT64_MAX);
// Run f once every command recorded up to now has completed on the GPU.
void defer(std::function<void()> f);
// Current command buffer (begun on first use).
VkCommandBuffer cmd();
bool has_recorded_commands();
// Called before the command buffer is ended (end rendering, close query
// sub-segments) and after a submission (renderer bookkeeping).
void set_submit_hooks(void (*before_end)(), void (*after_submit)(uint64_t serial));
// Closes the renderer's open rendering instance; called before commands that
// must be recorded outside one (barriers, copies, readback, presentation).
void set_leave_rendering_hook(void (*leave)());
// Submit the recorded commands. wait = block until they complete. Returns
// the serial of the submission (or the last one when nothing was recorded).
uint64_t submit(bool wait);
// Wait for all submitted work; returns false on device loss.
bool finish();
uint64_t submission_count();

// ---- barriers ------------------------------------------------------------
void note_gpu_write();          // a draw/clear/copy wrote device memory
bool gpu_dirty();
void barrier_if_dirty();        // must be outside a rendering instance
void full_barrier();
uint64_t barrier_count();

// ---- upload ring (host-visible, per-submission lifetime) ------------------
struct Span {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0, size = 0;
    uint8_t* data = nullptr;
    uint64_t chunk_id = 0;       // identifies the backing chunk generation
};
Span upload(VkDeviceSize bytes, VkDeviceSize alignment);
// Read-only residency query for discarding stale cache metadata. Unlike
// span_alive(), this does not reserve unused spans for another submission.
bool span_resident(const Span& span);
// Whether a span from an earlier upload is still resident (same chunk
// generation, not yet recycled): lets callers reuse unchanged data. A live
// span's chunk is reserved for the recording in progress.
bool span_alive(const Span& span);

// ---- buffers -------------------------------------------------------------
struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    Allocation memory;
    VkDeviceSize size = 0;
};
Buffer create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, MemoryUse use);
void destroy_buffer(Buffer& buffer);   // deferred until GPU completion

// ---- images --------------------------------------------------------------
struct ImageDesc {
    VkImageType type = VK_IMAGE_TYPE_2D;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    uint32_t width = 1, height = 1, depth = 1, levels = 1, layers = 1;
    VkImageUsageFlags usage = 0;
    VkImageCreateFlags flags = 0;
};
struct Image {
    VkImage image = VK_NULL_HANDLE;
    Allocation memory;
    ImageDesc desc;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    uint64_t content_generation = 0; // changes on creation and every recorded image write
    struct View { uint64_t key; VkImageView view; };
    std::vector<View> views;
    explicit operator bool() const { return image != VK_NULL_HANDLE; }
};
// Also marks the global synchronization dependency dirty. Generations track
// recorded writes, so alias copies can be reused before GPU completion.
void note_gpu_write(Image& image);
VkImageAspectFlags format_aspect(VkFormat format);
bool format_is_depth(VkFormat format);
bool format_has_stencil(VkFormat format);
bool format_is_integer(VkFormat format);
bool format_is_compressed(VkFormat format);
bool format_supports(VkFormat format, VkFormatFeatureFlags features, bool optimal = true);
// Creates the image and records its UNDEFINED -> GENERAL transition.
bool create_image(Image& image, const ImageDesc& desc);
void destroy_image(Image& image);      // deferred until GPU completion
struct ViewDesc {
    VkImageViewType type = VK_IMAGE_VIEW_TYPE_2D;
    VkFormat format = VK_FORMAT_UNDEFINED;      // UNDEFINED = image format
    VkComponentMapping swizzle{};
    uint32_t base_level = 0, levels = 1, base_layer = 0, layers = 1;
    VkImageAspectFlags aspect = 0;               // 0 = image aspect (depth only for depth/stencil)
};
VkImageView image_view(Image& image, const ViewDesc& desc);
uint64_t image_count();

// ---- samplers (cached by create info) ------------------------------------
VkSampler sampler(const VkSamplerCreateInfo& info, const VkClearColorValue* custom_border = nullptr);

// ---- swapchain / presentation ---------------------------------------------
bool has_surface();
// Copies src (GENERAL, mip 0 layer 0) to the swapchain with linear scaling to
// the window's drawable size, submits and presents. Recreates the swapchain
// when the window changed. Returns false when nothing could be presented
// (minimized window, lost surface).
bool present(Image& source, uint32_t source_width, uint32_t source_height);
uint64_t present_count();
VkPresentModeKHR present_mode();

// ---- one-shot readback -------------------------------------------------------
// Records a copy of one image subresource into host memory, submits, waits and
// returns the tightly packed bytes (texel blocks for compressed formats).
bool read_image(Image& image, VkImageAspectFlags aspect, uint32_t level, uint32_t layer,
                uint32_t width, uint32_t height, std::vector<uint8_t>& out);
uint32_t format_block_bytes(VkFormat format, VkImageAspectFlags aspect);

// Debug names for validation output.
void set_name(VkObjectType type, uint64_t handle, const char* name);

}  // namespace gx2vk

namespace gx2vk {
// Tests: keep a host copy of each presented swapchain image (RGBA8).
bool capture_presents(bool enable); // returns the previous setting
bool last_present(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height);
}
