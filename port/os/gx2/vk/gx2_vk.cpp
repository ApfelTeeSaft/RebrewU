#include "../../../runtime/rbrew_diagnostics.h"
#include "../../../runtime/texture_diagnostics.h"
#include "gx2_vk.h"
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_vulkan.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <unordered_map>
#include <atomic>
#include <immintrin.h>
#include <thread>
#include <type_traits>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace gx2vk {
namespace {

// ---------------------------------------------------------------------------
// Global device state (render-thread owned)
// ---------------------------------------------------------------------------
SDL_Window* s_window = nullptr;
VkInstance s_instance = VK_NULL_HANDLE;
VkDebugUtilsMessengerEXT s_messenger = VK_NULL_HANDLE;
VkPhysicalDevice s_physical = VK_NULL_HANDLE;
VkDevice s_device = VK_NULL_HANDLE;
VkQueue s_queue = VK_NULL_HANDLE;
uint32_t s_queue_family = 0;
VkSurfaceKHR s_surface = VK_NULL_HANDLE;
VkPhysicalDeviceMemoryProperties s_memory{};
Caps s_caps;
std::unordered_map<VkFormat,VkFormatProperties> s_format_properties;
uint64_t s_image_generation = 0;
bool s_ready = false;
bool s_debug_utils = false;
#ifdef RBREW_TEXTURE_DIAGNOSTICS
bool s_diagnostic_shutdown_idle = false;
#endif

std::mutex s_validation_mutex;
uint64_t s_validation_errors = 0, s_validation_warnings = 0;
std::string s_last_validation_error;

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    std::lock_guard lock(s_validation_mutex);
    const bool error = severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    uint64_t& count = error ? s_validation_errors : s_validation_warnings;
    ++count;
    TXDIAG(const char* diagnostic_class=data && data->pMessageIdName?data->pMessageIdName:"unknown";
        if(count<=32 || (count&(count-1))==0 || txdiag::sample("validation_class",txdiag::hash(diagnostic_class,std::strlen(diagnostic_class)),error))
        txdiag::event("validation","\"error\":%s,\"count\":%llu,\"message\":%s",error?"true":"false",count,txdiag::quote(data && data->pMessage?data->pMessage:"").c_str()));
    if (error) s_last_validation_error = data && data->pMessage ? data->pMessage : "";
    // Bounded: the first messages of each class, then powers of two.
    if (count <= 32 || (count & (count - 1)) == 0)
        RBREW_LOGF(stderr, "[vk] %s #%llu: %s\n", error ? "VALIDATION ERROR" : "validation warning",
                     static_cast<unsigned long long>(count), data && data->pMessage ? data->pMessage : "");
    return VK_FALSE;
}

bool env_flag(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && std::strcmp(v, "0") != 0;
}

// ---------------------------------------------------------------------------
// Memory: one free list per (memory type, linear) pool, 64 MiB blocks.
// Linear (buffer) and optimal (image) resources never share a block, which
// satisfies bufferImageGranularity.
// ---------------------------------------------------------------------------
constexpr VkDeviceSize kBlockSize = 64ull << 20;
constexpr VkDeviceSize kDedicatedThreshold = 16ull << 20;
struct Block {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* mapped = nullptr;
    std::map<VkDeviceSize, VkDeviceSize> free;   // offset -> size
    VkDeviceSize used = 0;
};
struct Pool { uint32_t type = 0; bool linear = false; std::vector<Block> blocks; };
std::vector<Pool> s_pools;
uint64_t s_allocated = 0;
std::array<uint64_t, VK_MAX_MEMORY_TYPES> s_allocated_by_type{};

int find_memory_type(uint32_t bits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred) {
    int fallback = -1;
    for (uint32_t i = 0; i < s_memory.memoryTypeCount; ++i) {
        if (!(bits & (1u << i))) continue;
        const auto flags = s_memory.memoryTypes[i].propertyFlags;
        if ((flags & required) != required) continue;
        if ((flags & preferred) == preferred) return int(i);
        if (fallback < 0) fallback = int(i);
    }
    return fallback;
}

uint32_t pool_index(uint32_t type, bool linear) {
    for (uint32_t i = 0; i < s_pools.size(); ++i)
        if (s_pools[i].type == type && s_pools[i].linear == linear) return i;
    s_pools.push_back({type, linear, {}});
    return uint32_t(s_pools.size() - 1);
}

bool type_host_visible(uint32_t type) {
    return s_memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
}

VkDeviceMemory raw_allocate(VkDeviceSize size, uint32_t type, uint8_t** mapped) {
    VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    info.allocationSize = size;
    info.memoryTypeIndex = type;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(s_device, &info, nullptr, &memory) != VK_SUCCESS) return VK_NULL_HANDLE;
    *mapped = nullptr;
    if (type_host_visible(type)) {
        void* p = nullptr;
        if (vkMapMemory(s_device, memory, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) {
            vkFreeMemory(s_device, memory, nullptr);
            return VK_NULL_HANDLE;
        }
        *mapped = static_cast<uint8_t*>(p);
    }
    s_allocated += size;
    s_allocated_by_type[type] += size;
    return memory;
}

bool block_allocate(Block& block, VkDeviceSize size, VkDeviceSize alignment, VkDeviceSize& out) {
    for (auto it = block.free.begin(); it != block.free.end(); ++it) {
        const VkDeviceSize start = (it->first + alignment - 1) / alignment * alignment;
        const VkDeviceSize end = it->first + it->second;
        if (start + size > end) continue;
        const VkDeviceSize range_start = it->first;
        block.free.erase(it);
        if (start > range_start) block.free[range_start] = start - range_start;
        if (start + size < end) block.free[start + size] = end - (start + size);
        block.used += size;
        out = start;
        return true;
    }
    return false;
}

void block_release(Block& block, VkDeviceSize offset, VkDeviceSize size) {
    block.used -= size;
    auto next = block.free.lower_bound(offset);
    if (next != block.free.end() && offset + size == next->first) {
        size += next->second;
        next = block.free.erase(next);
    }
    if (next != block.free.begin()) {
        auto previous = std::prev(next);
        if (previous->first + previous->second == offset) {
            previous->second += size;
            return;
        }
    }
    block.free[offset] = size;
}

// ---------------------------------------------------------------------------
// Submissions: a timeline semaphore value per submission ("serial").
// ---------------------------------------------------------------------------
VkSemaphore s_timeline = VK_NULL_HANDLE;
uint64_t s_next_serial = 1;      // serial of the command buffer being recorded
uint64_t s_completed = 0;
VkCommandPool s_command_pool = VK_NULL_HANDLE;
struct PooledCommand { VkCommandBuffer cb; uint64_t serial; };
std::deque<PooledCommand> s_free_commands;     // ordered by serial
std::vector<PooledCommand> s_pending_commands;
VkCommandBuffer s_current = VK_NULL_HANDLE;
// Image initialization (UNDEFINED -> GENERAL) recorded apart from the main
// command buffer so images can be created at any point, also while a
// rendering instance is open. It is submitted first in the same batch; the
// barrier second scope covers the later command buffer in submission order.
VkCommandBuffer s_prologue = VK_NULL_HANDLE;
bool s_recorded = false;
uint64_t s_submissions = 0;
void (*s_before_end)() = nullptr;
void (*s_after_submit)(uint64_t) = nullptr;
void (*s_leave_rendering)() = nullptr;
void leave_rendering() { if (s_leave_rendering) s_leave_rendering(); }
struct Deferred { uint64_t serial; std::function<void()> f; };
std::deque<Deferred> s_deferred;
bool s_dirty = false;
uint64_t s_barriers = 0;

void poll_completed() {
    if (!s_timeline) return;
    uint64_t value = 0;
    if (vkGetSemaphoreCounterValue(s_device, s_timeline, &value) == VK_SUCCESS) s_completed = std::max(s_completed, value);
    while (!s_deferred.empty() && s_deferred.front().serial <= s_completed) {
        auto f = std::move(s_deferred.front().f);
        s_deferred.pop_front();
        f();
    }
    for (auto it = s_pending_commands.begin(); it != s_pending_commands.end();) {
        if (it->serial <= s_completed) { s_free_commands.push_back(*it); it = s_pending_commands.erase(it); }
        else ++it;
    }
}

// ---------------------------------------------------------------------------
// Upload ring: 32 MiB host-visible chunks; a chunk is recycled after the
// submission of its last allocation completed.
// ---------------------------------------------------------------------------
constexpr VkDeviceSize kChunkSize = 32ull << 20;
struct Chunk {
    Buffer buffer;
    VkDeviceSize used = 0;
    uint64_t last_serial = 0;
    uint64_t id = 0;
};
std::vector<Chunk> s_chunks;
int s_active_chunk = -1;
uint64_t s_chunk_ids = 0;
std::vector<uint64_t> s_live_chunk_ids;

// ---------------------------------------------------------------------------
// Swapchain
// ---------------------------------------------------------------------------
struct Swapchain {
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkSemaphore> rendered;           // per image
    std::array<VkSemaphore, 3> acquired{};       // per frame slot
    std::array<uint64_t, 3> slot_serial{};
    unsigned slot = 0;
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
};
Swapchain s_swapchain;
uint64_t s_presents = 0;
bool s_capture_presents = false;
std::vector<uint8_t> s_last_present;
uint32_t s_last_present_w = 0, s_last_present_h = 0;

// Sampler identity: the create-info bytes from flags onward plus the custom
// border colour, as a fixed-size key. Building a heap string per lookup cost
// more than the lookup itself (one per sampled texture per draw).
struct SamplerKey {
    static constexpr size_t kInfoBytes = sizeof(VkSamplerCreateInfo) - offsetof(VkSamplerCreateInfo, flags);
    std::array<uint8_t, kInfoBytes + sizeof(VkClearColorValue)> bytes{};
    bool border = false;
    bool operator==(const SamplerKey& o) const { return border == o.border && bytes == o.bytes; }
};
static_assert(sizeof(SamplerKey::bytes) % 8 == 0);
struct SamplerKeyHash {
    size_t operator()(const SamplerKey& k) const noexcept {
        uint64_t h = 1469598103934665603ull ^ uint64_t(k.border);
        for (size_t i = 0; i < k.bytes.size(); i += 8) {
            uint64_t w; std::memcpy(&w, k.bytes.data() + i, 8);
            h = (h ^ w) * 1099511628211ull; h ^= h >> 29;
        }
        return size_t(h);
    }
};
std::unordered_map<SamplerKey, VkSampler, SamplerKeyHash> s_samplers;
uint64_t s_images = 0;

void destroy_swapchain_objects(bool keep_handle) {
    if (!s_device) return;
    for (auto s : s_swapchain.rendered) vkDestroySemaphore(s_device, s, nullptr);
    s_swapchain.rendered.clear();
    s_swapchain.images.clear();
    if (!keep_handle && s_swapchain.swapchain) {
        vkDestroySwapchainKHR(s_device, s_swapchain.swapchain, nullptr);
        s_swapchain.swapchain = VK_NULL_HANDLE;
    }
}

bool create_swapchain() {
    if (!s_surface || !s_window) return false;
    int w = 0, h = 0;
    SDL_Vulkan_GetDrawableSize(s_window, &w, &h);
    VkSurfaceCapabilitiesKHR capabilities{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s_physical, s_surface, &capabilities) != VK_SUCCESS) return false;
    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = std::clamp(uint32_t(std::max(w, 0)), capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        extent.height = std::clamp(uint32_t(std::max(h, 0)), capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    }
    if (!extent.width || !extent.height) return false;   // minimized
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(s_physical, s_surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(s_physical, s_surface, &count, formats.data());
    VkSurfaceFormatKHR chosen = formats.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : formats[0];
    // UNORM: the native graphics default framebuffer receives scan bytes unconverted
    // (GL_FRAMEBUFFER_SRGB stays disabled for the present blit).
    for (const auto& f : formats)
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { chosen = f; break; }
    vkGetPhysicalDeviceSurfacePresentModesKHR(s_physical, s_surface, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(s_physical, s_surface, &count, modes.data());
    // The guest's GX2WaitForVsync owns the frame cadence (shared vblank
    // clock). Presentation must not add a second blocking refresh wait, so
    // FIFO is the last resort (the native graphics path requests swap interval 0).
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    auto has = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
    if (const char* forced = std::getenv("RBREW_VK_PRESENT_MODE")) {
        if (!std::strcmp(forced, "immediate") && has(VK_PRESENT_MODE_IMMEDIATE_KHR)) mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        else if (!std::strcmp(forced, "mailbox") && has(VK_PRESENT_MODE_MAILBOX_KHR)) mode = VK_PRESENT_MODE_MAILBOX_KHR;
    } else if (has(VK_PRESENT_MODE_MAILBOX_KHR)) mode = VK_PRESENT_MODE_MAILBOX_KHR;
    else if (has(VK_PRESENT_MODE_IMMEDIATE_KHR)) mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    if (mode == VK_PRESENT_MODE_FIFO_KHR)
        RBREW_LOGF(stderr, "[vk] WARNING: only FIFO presentation is available; presentation adds a refresh wait\n");
    uint32_t images = std::max(3u, capabilities.minImageCount);
    if (capabilities.maxImageCount) images = std::min(images, capabilities.maxImageCount);
    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = s_surface;
    info.minImageCount = images;
    info.imageFormat = chosen.format;
    info.imageColorSpace = chosen.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    // Transfer source enables the test readback of presented images.
    if (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = capabilities.currentTransform;
    info.compositeAlpha = (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ?
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VkCompositeAlphaFlagBitsKHR(capabilities.supportedCompositeAlpha & -capabilities.supportedCompositeAlpha);
    info.presentMode = mode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = s_swapchain.swapchain;
    VkSwapchainKHR created = VK_NULL_HANDLE;
    if (vkCreateSwapchainKHR(s_device, &info, nullptr, &created) != VK_SUCCESS) return false;
    destroy_swapchain_objects(false);
    s_swapchain.swapchain = created;
    s_swapchain.format = chosen.format;
    s_swapchain.extent = extent;
    s_swapchain.mode = mode;
    vkGetSwapchainImagesKHR(s_device, created, &count, nullptr);
    s_swapchain.images.resize(count);
    vkGetSwapchainImagesKHR(s_device, created, &count, s_swapchain.images.data());
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    s_swapchain.rendered.resize(count);
    for (auto& s : s_swapchain.rendered) vkCreateSemaphore(s_device, &semaphore, nullptr, &s);
    for (auto& s : s_swapchain.acquired)
        if (!s) vkCreateSemaphore(s_device, &semaphore, nullptr, &s);
    static bool reported = false;
    if (!reported) {
        reported = true;
        RBREW_LOGF(stderr, "[vk] swapchain %ux%u format=%d images=%u present mode=%s\n", extent.width, extent.height,
                     int(chosen.format), count,
                     mode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" : mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE" : "FIFO");
    }
    return true;
}

bool has_extension(const std::vector<VkExtensionProperties>& list, const char* name) {
    for (const auto& e : list) if (!std::strcmp(e.extensionName, name)) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Split render thread (RBREW_RENDER_SPLIT, default on; "0" = off).
//
// The render thread keeps all renderer state and decisions; the Vulkan
// command recording it would do is handed to a second thread. After device
// creation the volk function pointers of the per-draw vkCmd* functions are
// replaced by wrappers that deep-copy their arguments into a ring and return.
// The recording thread replays them, in order, into the same command buffer
// with the original driver functions. Every other command, and everything
// that begins, ends, submits, resets or frees command buffers or waits for
// the device, first drains the ring and then runs on the caller - so the
// recorded command sequence, its order relative to submissions, and the
// external synchronization of command buffers and their pool are exactly the
// single-threaded ones. Unusual arguments (pNext chains) take the drain path.
// ---------------------------------------------------------------------------
namespace record {
struct Real {
    PFN_vkCmdBindPipeline BindPipeline; PFN_vkCmdBindDescriptorSets BindDescriptorSets; PFN_vkCmdBindIndexBuffer BindIndexBuffer;
    PFN_vkCmdBindVertexBuffers BindVertexBuffers; PFN_vkCmdPushDescriptorSetKHR PushDescriptorSetKHR; PFN_vkCmdPushConstants PushConstants;
    PFN_vkCmdSetViewport SetViewport; PFN_vkCmdSetScissor SetScissor; PFN_vkCmdSetCullMode SetCullMode; PFN_vkCmdSetFrontFace SetFrontFace;
    PFN_vkCmdSetDepthTestEnable SetDepthTestEnable; PFN_vkCmdSetDepthWriteEnable SetDepthWriteEnable; PFN_vkCmdSetDepthCompareOp SetDepthCompareOp;
    PFN_vkCmdSetStencilTestEnable SetStencilTestEnable; PFN_vkCmdSetStencilOp SetStencilOp; PFN_vkCmdSetStencilCompareMask SetStencilCompareMask;
    PFN_vkCmdSetStencilWriteMask SetStencilWriteMask; PFN_vkCmdSetStencilReference SetStencilReference; PFN_vkCmdSetDepthBias SetDepthBias;
    PFN_vkCmdSetDepthBiasEnable SetDepthBiasEnable; PFN_vkCmdSetPrimitiveRestartEnable SetPrimitiveRestartEnable;
    PFN_vkCmdSetBlendConstants SetBlendConstants; PFN_vkCmdDraw Draw; PFN_vkCmdDrawIndexed DrawIndexed;
    PFN_vkCmdBeginQuery BeginQuery; PFN_vkCmdEndQuery EndQuery; PFN_vkCmdBeginRendering BeginRendering; PFN_vkCmdEndRendering EndRendering;
    PFN_vkCmdPipelineBarrier2 PipelineBarrier2; PFN_vkCmdPipelineBarrier PipelineBarrier; PFN_vkCmdClearAttachments ClearAttachments;
    // Drain-then-direct.
    PFN_vkCmdCopyImage CopyImage; PFN_vkCmdCopyImageToBuffer CopyImageToBuffer; PFN_vkCmdBlitImage BlitImage;
    PFN_vkCmdCopyBufferToImage CopyBufferToImage; PFN_vkCmdClearColorImage ClearColorImage;
    PFN_vkCmdClearDepthStencilImage ClearDepthStencilImage; PFN_vkCmdDispatch Dispatch;
    PFN_vkCmdBeginTransformFeedbackEXT BeginTransformFeedbackEXT; PFN_vkCmdEndTransformFeedbackEXT EndTransformFeedbackEXT;
    PFN_vkCmdBindTransformFeedbackBuffersEXT BindTransformFeedbackBuffersEXT;
    PFN_vkBeginCommandBuffer BeginCommandBuffer; PFN_vkEndCommandBuffer EndCommandBuffer; PFN_vkResetCommandBuffer ResetCommandBuffer;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers; PFN_vkFreeCommandBuffers FreeCommandBuffers;
    PFN_vkResetCommandPool ResetCommandPool; PFN_vkDestroyCommandPool DestroyCommandPool;
    PFN_vkQueueSubmit QueueSubmit; PFN_vkQueueSubmit2 QueueSubmit2; PFN_vkQueueWaitIdle QueueWaitIdle;
    PFN_vkDeviceWaitIdle DeviceWaitIdle; PFN_vkQueuePresentKHR QueuePresentKHR;
} real{};

using Replay = void (*)(uint8_t* body);
constexpr size_t kRing = size_t(32) << 20;
constexpr uint32_t kWrap = 0xFFFFFFFFu;
struct alignas(64) Ring {
    std::vector<uint8_t> data = std::vector<uint8_t>(kRing);
    alignas(64) std::atomic<uint64_t> head{0};   // bytes published (producer)
    alignas(64) std::atomic<uint64_t> tail{0};   // bytes replayed (recording thread)
    alignas(64) std::atomic<bool> drain_waiting{false};
    alignas(64) std::atomic<bool> sleeping{false};   // recording thread blocked on head
};
Ring* s_ring = nullptr;
// Closing the window calls exit() while this thread may still be running.
// Destroying a joinable std::thread then calls std::terminate: every window
// close aborted with a crash report (owner WER dumps, 2026-10-17). At exit
// the thread is detached; the process ends it.
struct RecorderThread {
    std::thread thread;
    ~RecorderThread() { if (thread.joinable()) thread.detach(); }
};
RecorderThread s_recorder;
std::atomic<bool> s_active{false}, s_exit{false};
// One producer at a time: the renderer owner (render thread, or the caller
// under the renderer lock before it starts and in tests).

constexpr size_t align16(size_t n) { return (n + 15) & ~size_t(15); }

void recorder() {
#if defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);   // same as the render thread it serves
#endif
    Ring& r = *s_ring;
    uint64_t tail = r.tail.load(std::memory_order_relaxed);
    for (;;) {
        uint64_t head = r.head.load(std::memory_order_acquire);
        if (tail == head) {
            r.tail.store(tail, std::memory_order_seq_cst);
            if (r.drain_waiting.load(std::memory_order_seq_cst)) r.tail.notify_all();
            if (s_exit.load(std::memory_order_acquire)) return;
            // Commands arrive in bursts: spin briefly, then sleep on head.
            for (unsigned n = 0; n < 4096 && head == tail; ++n) { _mm_pause(); head = r.head.load(std::memory_order_acquire); }
            if (head == tail) {
                r.sleeping.store(true, std::memory_order_seq_cst);
                if (r.head.load(std::memory_order_seq_cst) == tail) r.head.wait(tail, std::memory_order_acquire);
                r.sleeping.store(false, std::memory_order_relaxed);
                continue;
            }
        }
        while (tail != head) {
            uint8_t* p = r.data.data() + (tail % kRing);
            uint32_t size; std::memcpy(&size, p, 4);
            if (size == kWrap) { tail += kRing - tail % kRing; continue; }
            Replay fn; std::memcpy(&fn, p + 8, sizeof(fn));
            fn(p + 16);
            tail += size;
        }
        r.tail.store(tail, std::memory_order_seq_cst);
        if (r.drain_waiting.load(std::memory_order_seq_cst)) r.tail.notify_all();
    }
}

// Producer side (the render thread). Returns the packet body (16-aligned).
uint64_t s_head = 0;   // producer's private head
uint8_t* reserve(size_t body, Replay fn) {
    Ring& r = *s_ring;
    const size_t size = 16 + align16(body);
    for (;;) {
        const uint64_t tail = r.tail.load(std::memory_order_acquire);
        const size_t offset = s_head % kRing;
        const size_t needed = size + (offset + size > kRing ? kRing - offset : 0);
        if (kRing - (s_head - tail) >= needed) {
            if (offset + size > kRing) {   // wrap marker, continue at the start
                const uint32_t wrap = kWrap; std::memcpy(r.data.data() + offset, &wrap, 4);
                s_head += kRing - offset;
            }
            uint8_t* p = r.data.data() + (s_head % kRing);
            const uint32_t s32 = uint32_t(size); std::memcpy(p, &s32, 4); std::memcpy(p + 8, &fn, sizeof(fn));
            s_head += size;
            return p + 16;
        }
        _mm_pause();   // ring full: the recording thread is catching up
    }
}
void publish() {
    s_ring->head.store(s_head, std::memory_order_seq_cst);
    if (s_ring->sleeping.load(std::memory_order_seq_cst)) s_ring->head.notify_one();
}
bool deferring() { return s_active.load(std::memory_order_relaxed); }
// Wait until every published packet is recorded.
void drain() {
    if (!s_active.load(std::memory_order_acquire)) return;
    Ring& r = *s_ring;
    if (r.tail.load(std::memory_order_acquire) == s_head) return;
    for (unsigned n = 0; n < 2048; ++n) { if (r.tail.load(std::memory_order_acquire) == s_head) return; _mm_pause(); }
    r.drain_waiting.store(true, std::memory_order_seq_cst);
    for (uint64_t t; (t = r.tail.load(std::memory_order_acquire)) != s_head;) r.tail.wait(t, std::memory_order_acquire);
    r.drain_waiting.store(false, std::memory_order_relaxed);
}

// A packet whose body is a trivially-copyable lambda (plus `extra` bytes the
// lambda points into, filled by `fill`).
template<class F> void thunk(uint8_t* body) { (*reinterpret_cast<F*>(body))(); }
template<class Make> void defer(size_t extra, Make make) {
    using F = decltype(make(static_cast<uint8_t*>(nullptr)));
    static_assert(std::is_trivially_copyable_v<F>);
    uint8_t* body = reserve(align16(sizeof(F)) + extra, &thunk<F>);
    new (body) F(make(body + align16(sizeof(F))));
    publish();
}
// Copies `count` elements to the cursor and returns the copy (null for none).
template<class T> const T* put(uint8_t*& cursor, const T* src, uint32_t count) {
    if (!src || !count) return nullptr;
    T* out = reinterpret_cast<T*>(cursor);
    std::memcpy(out, src, sizeof(T) * count);
    cursor += align16(sizeof(T) * count);
    return out;
}
template<class T> constexpr size_t bytes(const T*, uint32_t count) { return align16(sizeof(T) * count); }

// --- Deferred commands (scalars by value) ---
#define RBREW_DEFER0(Name, Params, Args) \
    VKAPI_ATTR void VKAPI_CALL w_##Name Params { \
        if (!deferring()) return real.Name Args; \
        defer(0, [=](uint8_t*) { return [=] { real.Name Args; }; }); }
RBREW_DEFER0(BindPipeline, (VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p), (cb, bp, p))
RBREW_DEFER0(BindIndexBuffer, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkIndexType t), (cb, b, o, t))
RBREW_DEFER0(SetCullMode, (VkCommandBuffer cb, VkCullModeFlags m), (cb, m))
RBREW_DEFER0(SetFrontFace, (VkCommandBuffer cb, VkFrontFace f), (cb, f))
RBREW_DEFER0(SetDepthTestEnable, (VkCommandBuffer cb, VkBool32 e), (cb, e))
RBREW_DEFER0(SetDepthWriteEnable, (VkCommandBuffer cb, VkBool32 e), (cb, e))
RBREW_DEFER0(SetDepthCompareOp, (VkCommandBuffer cb, VkCompareOp o), (cb, o))
RBREW_DEFER0(SetStencilTestEnable, (VkCommandBuffer cb, VkBool32 e), (cb, e))
RBREW_DEFER0(SetStencilOp, (VkCommandBuffer cb, VkStencilFaceFlags f, VkStencilOp a, VkStencilOp b, VkStencilOp c, VkCompareOp d), (cb, f, a, b, c, d))
RBREW_DEFER0(SetStencilCompareMask, (VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t m), (cb, f, m))
RBREW_DEFER0(SetStencilWriteMask, (VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t m), (cb, f, m))
RBREW_DEFER0(SetStencilReference, (VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t v), (cb, f, v))
RBREW_DEFER0(SetDepthBias, (VkCommandBuffer cb, float a, float b, float c), (cb, a, b, c))
RBREW_DEFER0(SetDepthBiasEnable, (VkCommandBuffer cb, VkBool32 e), (cb, e))
RBREW_DEFER0(SetPrimitiveRestartEnable, (VkCommandBuffer cb, VkBool32 e), (cb, e))
RBREW_DEFER0(Draw, (VkCommandBuffer cb, uint32_t v, uint32_t i, uint32_t fv, uint32_t fi), (cb, v, i, fv, fi))
RBREW_DEFER0(DrawIndexed, (VkCommandBuffer cb, uint32_t n, uint32_t i, uint32_t fi, int32_t vo, uint32_t fin), (cb, n, i, fi, vo, fin))
RBREW_DEFER0(BeginQuery, (VkCommandBuffer cb, VkQueryPool p, uint32_t q, VkQueryControlFlags f), (cb, p, q, f))
RBREW_DEFER0(EndQuery, (VkCommandBuffer cb, VkQueryPool p, uint32_t q), (cb, p, q))
RBREW_DEFER0(EndRendering, (VkCommandBuffer cb), (cb))
#undef RBREW_DEFER0

// --- Deferred commands with arrays (deep copies) ---
VKAPI_ATTR void VKAPI_CALL w_SetBlendConstants(VkCommandBuffer cb, const float c[4]) {
    if (!deferring()) return real.SetBlendConstants(cb, c);
    struct C { float v[4]; } values; std::memcpy(values.v, c, 16);
    defer(0, [=](uint8_t*) { return [=] { real.SetBlendConstants(cb, values.v); }; });
}
VKAPI_ATTR void VKAPI_CALL w_SetViewport(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkViewport* v) {
    if (!deferring()) return real.SetViewport(cb, first, count, v);
    defer(bytes(v, count), [=](uint8_t* c) { const VkViewport* copy = put(c, v, count); return [=] { real.SetViewport(cb, first, count, copy); }; });
}
VKAPI_ATTR void VKAPI_CALL w_SetScissor(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkRect2D* s) {
    if (!deferring()) return real.SetScissor(cb, first, count, s);
    defer(bytes(s, count), [=](uint8_t* c) { const VkRect2D* copy = put(c, s, count); return [=] { real.SetScissor(cb, first, count, copy); }; });
}
VKAPI_ATTR void VKAPI_CALL w_BindVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkBuffer* b, const VkDeviceSize* o) {
    if (!deferring()) return real.BindVertexBuffers(cb, first, count, b, o);
    defer(bytes(b, count) + bytes(o, count), [=](uint8_t* c) {
        const VkBuffer* bc = put(c, b, count); const VkDeviceSize* oc = put(c, o, count);
        return [=] { real.BindVertexBuffers(cb, first, count, bc, oc); }; });
}
VKAPI_ATTR void VKAPI_CALL w_BindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t first,
        uint32_t count, const VkDescriptorSet* sets, uint32_t dyn_count, const uint32_t* dyn) {
    if (!deferring()) return real.BindDescriptorSets(cb, bp, l, first, count, sets, dyn_count, dyn);
    defer(bytes(sets, count) + bytes(dyn, dyn_count), [=](uint8_t* c) {
        const VkDescriptorSet* sc = put(c, sets, count); const uint32_t* dc = put(c, dyn, dyn_count);
        return [=] { real.BindDescriptorSets(cb, bp, l, first, count, sc, dyn_count, dc); }; });
}
VKAPI_ATTR void VKAPI_CALL w_PushConstants(VkCommandBuffer cb, VkPipelineLayout l, VkShaderStageFlags s, uint32_t offset, uint32_t size, const void* v) {
    if (!deferring()) return real.PushConstants(cb, l, s, offset, size, v);
    defer(align16(size), [=](uint8_t* c) {
        const uint8_t* vc = put(c, static_cast<const uint8_t*>(v), size);
        return [=] { real.PushConstants(cb, l, s, offset, size, vc); }; });
}
VKAPI_ATTR void VKAPI_CALL w_ClearAttachments(VkCommandBuffer cb, uint32_t n, const VkClearAttachment* a, uint32_t rn, const VkClearRect* rects) {
    if (!deferring()) return real.ClearAttachments(cb, n, a, rn, rects);
    defer(bytes(a, n) + bytes(rects, rn), [=](uint8_t* c) {
        const VkClearAttachment* ac = put(c, a, n); const VkClearRect* rc = put(c, rects, rn);
        return [=] { real.ClearAttachments(cb, n, ac, rn, rc); }; });
}
bool descriptor_images(VkDescriptorType t) {
    return t == VK_DESCRIPTOR_TYPE_SAMPLER || t == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || t == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
           t == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || t == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
}
bool descriptor_buffers(VkDescriptorType t) {
    return t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
           t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
}
bool descriptor_texels(VkDescriptorType t) { return t == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || t == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER; }
VKAPI_ATTR void VKAPI_CALL w_PushDescriptorSet(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t set,
        uint32_t n, const VkWriteDescriptorSet* writes) {
    bool simple = deferring();
    size_t extra = bytes(writes, n);
    for (uint32_t i = 0; simple && i < n; ++i) {
        const auto& w = writes[i];
        if (w.pNext) { simple = false; break; }
        if (descriptor_images(w.descriptorType)) extra += bytes(w.pImageInfo, w.descriptorCount);
        else if (descriptor_buffers(w.descriptorType)) extra += bytes(w.pBufferInfo, w.descriptorCount);
        else if (descriptor_texels(w.descriptorType)) extra += bytes(w.pTexelBufferView, w.descriptorCount);
        else simple = false;
    }
    if (!simple) { drain(); return real.PushDescriptorSetKHR(cb, bp, l, set, n, writes); }
    defer(extra, [=](uint8_t* c) {
        VkWriteDescriptorSet* wc = const_cast<VkWriteDescriptorSet*>(put(c, writes, n));
        for (uint32_t i = 0; i < n; ++i) {
            auto& w = wc[i];
            if (descriptor_images(w.descriptorType)) { w.pImageInfo = put(c, w.pImageInfo, w.descriptorCount); w.pBufferInfo = nullptr; w.pTexelBufferView = nullptr; }
            else if (descriptor_buffers(w.descriptorType)) { w.pBufferInfo = put(c, w.pBufferInfo, w.descriptorCount); w.pImageInfo = nullptr; w.pTexelBufferView = nullptr; }
            else { w.pTexelBufferView = put(c, w.pTexelBufferView, w.descriptorCount); w.pImageInfo = nullptr; w.pBufferInfo = nullptr; }
        }
        return [=] { real.PushDescriptorSetKHR(cb, bp, l, set, n, wc); };
    });
}
VKAPI_ATTR void VKAPI_CALL w_BeginRendering(VkCommandBuffer cb, const VkRenderingInfo* info) {
    const bool simple = deferring() && !info->pNext &&
        std::all_of(info->pColorAttachments, info->pColorAttachments + info->colorAttachmentCount, [](const VkRenderingAttachmentInfo& a) { return !a.pNext; }) &&
        (!info->pDepthAttachment || !info->pDepthAttachment->pNext) && (!info->pStencilAttachment || !info->pStencilAttachment->pNext);
    if (!simple) { drain(); return real.BeginRendering(cb, info); }
    defer(bytes(info, 1) + bytes(info->pColorAttachments, info->colorAttachmentCount) + bytes(info->pDepthAttachment, 1) +
          bytes(info->pStencilAttachment, 1), [=](uint8_t* c) {
        VkRenderingInfo* ic = const_cast<VkRenderingInfo*>(put(c, info, 1));
        ic->pColorAttachments = put(c, info->pColorAttachments, info->colorAttachmentCount);
        ic->pDepthAttachment = put(c, info->pDepthAttachment, info->pDepthAttachment ? 1u : 0u);
        ic->pStencilAttachment = put(c, info->pStencilAttachment, info->pStencilAttachment ? 1u : 0u);
        return [=] { real.BeginRendering(cb, ic); };
    });
}
VKAPI_ATTR void VKAPI_CALL w_PipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo* d) {
    const bool simple = deferring() && !d->pNext &&
        std::all_of(d->pMemoryBarriers, d->pMemoryBarriers + d->memoryBarrierCount, [](const VkMemoryBarrier2& b) { return !b.pNext; }) &&
        std::all_of(d->pBufferMemoryBarriers, d->pBufferMemoryBarriers + d->bufferMemoryBarrierCount, [](const VkBufferMemoryBarrier2& b) { return !b.pNext; }) &&
        std::all_of(d->pImageMemoryBarriers, d->pImageMemoryBarriers + d->imageMemoryBarrierCount, [](const VkImageMemoryBarrier2& b) { return !b.pNext; });
    if (!simple) { drain(); return real.PipelineBarrier2(cb, d); }
    defer(bytes(d, 1) + bytes(d->pMemoryBarriers, d->memoryBarrierCount) + bytes(d->pBufferMemoryBarriers, d->bufferMemoryBarrierCount) +
          bytes(d->pImageMemoryBarriers, d->imageMemoryBarrierCount), [=](uint8_t* c) {
        VkDependencyInfo* dc = const_cast<VkDependencyInfo*>(put(c, d, 1));
        dc->pMemoryBarriers = put(c, d->pMemoryBarriers, d->memoryBarrierCount);
        dc->pBufferMemoryBarriers = put(c, d->pBufferMemoryBarriers, d->bufferMemoryBarrierCount);
        dc->pImageMemoryBarriers = put(c, d->pImageMemoryBarriers, d->imageMemoryBarrierCount);
        return [=] { real.PipelineBarrier2(cb, dc); };
    });
}
VKAPI_ATTR void VKAPI_CALL w_PipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags f,
        uint32_t mn, const VkMemoryBarrier* m, uint32_t bn, const VkBufferMemoryBarrier* b, uint32_t in, const VkImageMemoryBarrier* im) {
    const bool simple = deferring() && std::all_of(m, m + mn, [](const VkMemoryBarrier& x) { return !x.pNext; }) &&
        std::all_of(b, b + bn, [](const VkBufferMemoryBarrier& x) { return !x.pNext; }) &&
        std::all_of(im, im + in, [](const VkImageMemoryBarrier& x) { return !x.pNext; });
    if (!simple) { drain(); return real.PipelineBarrier(cb, src, dst, f, mn, m, bn, b, in, im); }
    defer(bytes(m, mn) + bytes(b, bn) + bytes(im, in), [=](uint8_t* c) {
        const VkMemoryBarrier* mc = put(c, m, mn); const VkBufferMemoryBarrier* bc = put(c, b, bn); const VkImageMemoryBarrier* ic = put(c, im, in);
        return [=] { real.PipelineBarrier(cb, src, dst, f, mn, mc, bn, bc, in, ic); };
    });
}

// --- Drain, then run on the caller ---
#define RBREW_DRAIN(Ret, Name, Params, Args) \
    VKAPI_ATTR Ret VKAPI_CALL w_##Name Params { drain(); return real.Name Args; }
// Transfers, image clears, dispatch and transform feedback: also deferred
// (plain arrays, no pNext), so they no longer stall the render thread.
VKAPI_ATTR void VKAPI_CALL w_CopyImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageCopy* r) {
    if (!deferring()) return real.CopyImage(cb, s, sl, d, dl, n, r);
    defer(bytes(r, n), [=](uint8_t* c) { const VkImageCopy* rc = put(c, r, n); return [=] { real.CopyImage(cb, s, sl, d, dl, n, rc); }; });
}
VKAPI_ATTR void VKAPI_CALL w_CopyImageToBuffer(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkBuffer d, uint32_t n, const VkBufferImageCopy* r) {
    if (!deferring()) return real.CopyImageToBuffer(cb, s, sl, d, n, r);
    defer(bytes(r, n), [=](uint8_t* c) { const VkBufferImageCopy* rc = put(c, r, n); return [=] { real.CopyImageToBuffer(cb, s, sl, d, n, rc); }; });
}
VKAPI_ATTR void VKAPI_CALL w_BlitImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageBlit* r, VkFilter f) {
    if (!deferring()) return real.BlitImage(cb, s, sl, d, dl, n, r, f);
    defer(bytes(r, n), [=](uint8_t* c) { const VkImageBlit* rc = put(c, r, n); return [=] { real.BlitImage(cb, s, sl, d, dl, n, rc, f); }; });
}
VKAPI_ATTR void VKAPI_CALL w_CopyBufferToImage(VkCommandBuffer cb, VkBuffer s, VkImage d, VkImageLayout dl, uint32_t n, const VkBufferImageCopy* r) {
    if (!deferring()) return real.CopyBufferToImage(cb, s, d, dl, n, r);
    defer(bytes(r, n), [=](uint8_t* c) { const VkBufferImageCopy* rc = put(c, r, n); return [=] { real.CopyBufferToImage(cb, s, d, dl, n, rc); }; });
}
VKAPI_ATTR void VKAPI_CALL w_ClearColorImage(VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearColorValue* v, uint32_t n, const VkImageSubresourceRange* r) {
    if (!deferring()) return real.ClearColorImage(cb, i, l, v, n, r);
    defer(bytes(v, 1) + bytes(r, n), [=](uint8_t* c) {
        const VkClearColorValue* vc = put(c, v, v ? 1u : 0u); const VkImageSubresourceRange* rc = put(c, r, n);
        return [=] { real.ClearColorImage(cb, i, l, vc, n, rc); }; });
}
VKAPI_ATTR void VKAPI_CALL w_ClearDepthStencilImage(VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearDepthStencilValue* v, uint32_t n, const VkImageSubresourceRange* r) {
    if (!deferring()) return real.ClearDepthStencilImage(cb, i, l, v, n, r);
    defer(bytes(v, 1) + bytes(r, n), [=](uint8_t* c) {
        const VkClearDepthStencilValue* vc = put(c, v, v ? 1u : 0u); const VkImageSubresourceRange* rc = put(c, r, n);
        return [=] { real.ClearDepthStencilImage(cb, i, l, vc, n, rc); }; });
}
VKAPI_ATTR void VKAPI_CALL w_Dispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
    if (!deferring()) return real.Dispatch(cb, x, y, z);
    defer(0, [=](uint8_t*) { return [=] { real.Dispatch(cb, x, y, z); }; });
}
VKAPI_ATTR void VKAPI_CALL w_BeginTransformFeedbackEXT(VkCommandBuffer cb, uint32_t f, uint32_t n, const VkBuffer* b, const VkDeviceSize* o) {
    if (!deferring()) return real.BeginTransformFeedbackEXT(cb, f, n, b, o);
    defer(bytes(b, n) + bytes(o, n), [=](uint8_t* c) {
        const VkBuffer* bc = put(c, b, n); const VkDeviceSize* oc = put(c, o, n);
        return [=] { real.BeginTransformFeedbackEXT(cb, f, n, bc, oc); }; });
}
VKAPI_ATTR void VKAPI_CALL w_EndTransformFeedbackEXT(VkCommandBuffer cb, uint32_t f, uint32_t n, const VkBuffer* b, const VkDeviceSize* o) {
    if (!deferring()) return real.EndTransformFeedbackEXT(cb, f, n, b, o);
    defer(bytes(b, n) + bytes(o, n), [=](uint8_t* c) {
        const VkBuffer* bc = put(c, b, n); const VkDeviceSize* oc = put(c, o, n);
        return [=] { real.EndTransformFeedbackEXT(cb, f, n, bc, oc); }; });
}
VKAPI_ATTR void VKAPI_CALL w_BindTransformFeedbackBuffersEXT(VkCommandBuffer cb, uint32_t f, uint32_t n, const VkBuffer* b, const VkDeviceSize* o, const VkDeviceSize* s) {
    if (!deferring()) return real.BindTransformFeedbackBuffersEXT(cb, f, n, b, o, s);
    defer(bytes(b, n) + bytes(o, n) + bytes(s, n), [=](uint8_t* c) {
        const VkBuffer* bc = put(c, b, n); const VkDeviceSize* oc = put(c, o, n); const VkDeviceSize* sc = put(c, s, n);
        return [=] { real.BindTransformFeedbackBuffersEXT(cb, f, n, bc, oc, sc); }; });
}
RBREW_DRAIN(VkResult, BeginCommandBuffer, (VkCommandBuffer cb, const VkCommandBufferBeginInfo* i), (cb, i))
RBREW_DRAIN(VkResult, EndCommandBuffer, (VkCommandBuffer cb), (cb))
RBREW_DRAIN(VkResult, ResetCommandBuffer, (VkCommandBuffer cb, VkCommandBufferResetFlags f), (cb, f))
RBREW_DRAIN(VkResult, AllocateCommandBuffers, (VkDevice d, const VkCommandBufferAllocateInfo* i, VkCommandBuffer* o), (d, i, o))
RBREW_DRAIN(void, FreeCommandBuffers, (VkDevice d, VkCommandPool p, uint32_t n, const VkCommandBuffer* c), (d, p, n, c))
RBREW_DRAIN(VkResult, ResetCommandPool, (VkDevice d, VkCommandPool p, VkCommandPoolResetFlags f), (d, p, f))
RBREW_DRAIN(void, DestroyCommandPool, (VkDevice d, VkCommandPool p, const VkAllocationCallbacks* a), (d, p, a))
RBREW_DRAIN(VkResult, QueueSubmit, (VkQueue q, uint32_t n, const VkSubmitInfo* s, VkFence f), (q, n, s, f))
RBREW_DRAIN(VkResult, QueueSubmit2, (VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f), (q, n, s, f))
RBREW_DRAIN(VkResult, QueueWaitIdle, (VkQueue q), (q))
RBREW_DRAIN(VkResult, DeviceWaitIdle, (VkDevice d), (d))
RBREW_DRAIN(VkResult, QueuePresentKHR, (VkQueue q, const VkPresentInfoKHR* p), (q, p))
#undef RBREW_DRAIN

// Swap one volk pointer for its wrapper, keeping the driver function. A
// pointer that is already our wrapper (re-initialization, or a test that
// restored it after shutdown) keeps the driver function saved before.
template<class P> void hook(P& volk_pointer, P& saved, P wrapper) {
    if (!volk_pointer) return;
    if (volk_pointer != wrapper) saved = volk_pointer;
    volk_pointer = wrapper;
}

void install() {
    const char* v = std::getenv("RBREW_RENDER_SPLIT");
    if (v && v[0] == '0') return;
#define H(Name) hook(vkCmd##Name, real.Name, &w_##Name)
    H(BindPipeline); H(BindDescriptorSets); H(BindIndexBuffer); H(BindVertexBuffers); H(PushConstants);
    hook(vkCmdPushDescriptorSetKHR, real.PushDescriptorSetKHR, &w_PushDescriptorSet);
    H(SetViewport); H(SetScissor); H(SetCullMode); H(SetFrontFace); H(SetDepthTestEnable); H(SetDepthWriteEnable);
    H(SetDepthCompareOp); H(SetStencilTestEnable); H(SetStencilOp); H(SetStencilCompareMask); H(SetStencilWriteMask);
    H(SetStencilReference); H(SetDepthBias); H(SetDepthBiasEnable); H(SetPrimitiveRestartEnable); H(SetBlendConstants);
    H(Draw); H(DrawIndexed); H(BeginQuery); H(EndQuery); H(BeginRendering); H(EndRendering); H(PipelineBarrier2);
    H(PipelineBarrier); H(ClearAttachments); H(CopyImage); H(CopyImageToBuffer); H(BlitImage); H(CopyBufferToImage);
    H(ClearColorImage); H(ClearDepthStencilImage); H(Dispatch); H(BeginTransformFeedbackEXT); H(EndTransformFeedbackEXT);
    H(BindTransformFeedbackBuffersEXT);
#undef H
#define H(Name) hook(vk##Name, real.Name, &w_##Name)
    H(BeginCommandBuffer); H(EndCommandBuffer); H(ResetCommandBuffer); H(AllocateCommandBuffers); H(FreeCommandBuffers);
    H(ResetCommandPool); H(DestroyCommandPool); H(QueueSubmit); H(QueueSubmit2); H(QueueWaitIdle); H(DeviceWaitIdle); H(QueuePresentKHR);
#undef H
    if (!s_ring) s_ring = new Ring;   // reused across re-initialization
    s_ring->head.store(s_head, std::memory_order_relaxed);
    s_ring->tail.store(s_head, std::memory_order_relaxed);
    s_exit = false;
    s_recorder.thread = std::thread(recorder);
    s_active = true;
}
void stop() {
    if (!s_recorder.thread.joinable()) return;
    drain();
    s_exit = true;
    // A sleeping recorder wakes only when head changes: publish a no-op.
    defer(0, [](uint8_t*) { return [] {}; });
    s_ring->head.notify_all();
    s_recorder.thread.join();
    s_active = false;
}
}  // namespace record

}  // namespace

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------
bool init(SDL_Window* window, bool native_depth) {
    if (s_ready) return true;
    s_format_properties.clear();
    s_window = window;
    if (volkInitialize() != VK_SUCCESS) {
        RBREW_LOGF(stderr, "[vk] FATAL: the Vulkan loader (vulkan-1.dll) is not available\n");
        return false;
    }
    if (volkGetInstanceVersion() < VK_API_VERSION_1_3) {
        RBREW_LOGF(stderr, "[vk] FATAL: Vulkan 1.3 is required (loader reports %u.%u)\n",
                     VK_API_VERSION_MAJOR(volkGetInstanceVersion()), VK_API_VERSION_MINOR(volkGetInstanceVersion()));
        return false;
    }
    std::vector<const char*> extensions;
    if (window) {
        unsigned count = 0;
        SDL_Vulkan_GetInstanceExtensions(window, &count, nullptr);
        std::vector<const char*> sdl(count);
        SDL_Vulkan_GetInstanceExtensions(window, &count, sdl.data());
        extensions.insert(extensions.end(), sdl.begin(), sdl.end());
    }
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());
    s_debug_utils = has_extension(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    // RBREW_VK_VALIDATION=1 enables the Khronos layer, =sync adds
    // synchronization validation. Tests enable it explicitly.
#ifdef RBREW_TEXTURE_DIAGNOSTICS
    const char* validation_mode = std::getenv("RBREW_VK_VALIDATION");
    if(!validation_mode)validation_mode="sync";
#ifdef RBREW_TEXTURE_DIAG_LAYER_PATH
    if(!std::getenv("VK_ADD_LAYER_PATH"))_putenv_s("VK_ADD_LAYER_PATH",RBREW_TEXTURE_DIAG_LAYER_PATH);
#endif
#else
    const char* validation_mode = rbrew_diagnostic_env("RBREW_VK_VALIDATION");
#endif
    const bool validation = validation_mode && *validation_mode && std::strcmp(validation_mode, "0") != 0;
    const bool sync_validation = validation && (!std::strcmp(validation_mode, "sync") || !std::strcmp(validation_mode, "2"));
    std::vector<const char*> layers;
    if (validation) {
        vkEnumerateInstanceLayerProperties(&count, nullptr);
        std::vector<VkLayerProperties> props(count);
        vkEnumerateInstanceLayerProperties(&count, props.data());
        bool found = false;
        for (const auto& l : props) found |= !std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation");
        if (found) layers.push_back("VK_LAYER_KHRONOS_validation");
        else RBREW_LOGF(stderr, "[vk] RBREW_VK_VALIDATION requested but VK_LAYER_KHRONOS_validation is not installed (set VK_ADD_LAYER_PATH)\n");
    }
    if (s_debug_utils) extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Gambit native port";
    app.pEngineName = "RebrewU GX2";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = uint32_t(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    info.enabledLayerCount = uint32_t(layers.size());
    info.ppEnabledLayerNames = layers.data();
    VkValidationFeatureEnableEXT enables[] = {VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT};
    VkValidationFeaturesEXT features{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    features.enabledValidationFeatureCount = 1;
    features.pEnabledValidationFeatures = enables;
    if (!layers.empty() && sync_validation) info.pNext = &features;
    VkResult result = vkCreateInstance(&info, nullptr, &s_instance);
    if (result != VK_SUCCESS) {
        RBREW_LOGF(stderr, "[vk] FATAL: vkCreateInstance failed (%d)\n", int(result));
        return false;
    }
    volkLoadInstance(s_instance);
    s_caps.validation = !layers.empty();
    s_caps.sync_validation = s_caps.validation && sync_validation;
    TXDIAG(txdiag::event("device_validation","\"requested\":%s,\"active\":%s,\"synchronization\":%s,\"debug_utils\":%s",validation?"true":"false",s_caps.validation?"true":"false",s_caps.sync_validation?"true":"false",s_debug_utils?"true":"false"));
    if (s_debug_utils) {
        VkDebugUtilsMessengerCreateInfoEXT messenger{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        messenger.pfnUserCallback = debug_callback;
        if (s_caps.validation) vkCreateDebugUtilsMessengerEXT(s_instance, &messenger, nullptr, &s_messenger);
    }
    if (window && !SDL_Vulkan_CreateSurface(window, s_instance, &s_surface)) {
        RBREW_LOGF(stderr, "[vk] FATAL: SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
        return false;
    }

    // Physical device: the first discrete GPU with Vulkan 1.3 and a graphics
    // queue that can present to the window.
    vkEnumeratePhysicalDevices(s_instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(s_instance, &count, devices.data());
    int best_score = -1;
    for (auto candidate : devices) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(candidate, &props);
        if (props.apiVersion < VK_API_VERSION_1_3) continue;
        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, nullptr);
        std::vector<VkQueueFamilyProperties> family_props(families);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, family_props.data());
        for (uint32_t f = 0; f < families; ++f) {
            if (!(family_props[f].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            VkBool32 present = VK_TRUE;
            if (s_surface) vkGetPhysicalDeviceSurfaceSupportKHR(candidate, f, s_surface, &present);
            if (!present) continue;
            const int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 :
                              props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1 : 0;
            if (score > best_score) { best_score = score; s_physical = candidate; s_queue_family = f; }
            break;
        }
    }
    if (!s_physical) {
        RBREW_LOGF(stderr, "[vk] FATAL: no Vulkan 1.3 device with a graphics/present queue\n");
        return false;
    }
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(s_physical, &props);
    s_caps.api_version = props.apiVersion;
    s_caps.device_name = props.deviceName;
    s_caps.limits = props.limits;
    vkGetPhysicalDeviceMemoryProperties(s_physical, &s_memory);

    vkEnumerateDeviceExtensionProperties(s_physical, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> device_extensions(count);
    vkEnumerateDeviceExtensionProperties(s_physical, nullptr, &count, device_extensions.data());
    std::vector<const char*> enable;
    if (s_surface) enable.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    auto want = [&](const char* name) {
        const bool ok = has_extension(device_extensions, name);
        if (ok) enable.push_back(name);
        return ok;
    };
    const bool push_ext = want(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
    const bool border_ext = want(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    const bool restart_ext = want(VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME);
    const bool xfb_ext = want(VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME);
    const bool depth_clip_ext = want(VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME);
    const bool provoking_ext = want(VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME);
    const bool clip_enable_ext = want(VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME);
    const bool divisor_khr = want(VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
    const bool divisor_ext = !divisor_khr && want(VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);

    // Feature query chain.
    VkPhysicalDeviceCustomBorderColorFeaturesEXT border{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT restart{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT};
    VkPhysicalDeviceTransformFeedbackFeaturesEXT xfb{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT};
    VkPhysicalDeviceDepthClipControlFeaturesEXT depth_clip{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
    VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR divisor{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR};
    VkPhysicalDeviceProvokingVertexFeaturesEXT provoking{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT};
    VkPhysicalDeviceDepthClipEnableFeaturesEXT clip_enable{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f11; f11.pNext = &f12; f12.pNext = &f13;
    void** tail = &f13.pNext;
    if (border_ext) { *tail = &border; tail = &border.pNext; }
    if (restart_ext) { *tail = &restart; tail = &restart.pNext; }
    if (xfb_ext) { *tail = &xfb; tail = &xfb.pNext; }
    if (depth_clip_ext) { *tail = &depth_clip; tail = &depth_clip.pNext; }
    if (divisor_khr || divisor_ext) { *tail = &divisor; tail = &divisor.pNext; }
    if (provoking_ext) { *tail = &provoking; tail = &provoking.pNext; }
    if (clip_enable_ext) { *tail = &clip_enable; tail = &clip_enable.pNext; }
    vkGetPhysicalDeviceFeatures2(s_physical, &f2);

    // Required features (the GX2 translation depends on each).
    struct Required { const char* name; VkBool32 value; };
    const Required required[] = {
        {"dynamicRendering", f13.dynamicRendering}, {"synchronization2", f13.synchronization2},
        {"timelineSemaphore", f12.timelineSemaphore}, {"hostQueryReset", f12.hostQueryReset},
        {"geometryShader", f2.features.geometryShader}, {"independentBlend", f2.features.independentBlend},
        {"textureCompressionBC", f2.features.textureCompressionBC}, {"imageCubeArray", f2.features.imageCubeArray},
        {"largePoints", f2.features.largePoints},
        // glslang lowers GLSL discard (alpha test, GX2 KILL) to
        // OpDemoteToHelperInvocation for SPIR-V 1.6.
        {"shaderDemoteToHelperInvocation", f13.shaderDemoteToHelperInvocation},
    };
    for (const auto& r : required) {
        if (!r.value) {
            RBREW_LOGF(stderr, "[vk] FATAL: required device feature %s is not supported by %s\n", r.name, props.deviceName);
            return false;
        }
    }
    // Enabled set: required plus optional capabilities.
    VkPhysicalDeviceFeatures2 e2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan11Features e11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features e12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features e13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    e2.pNext = &e11; e11.pNext = &e12; e12.pNext = &e13;
    e13.dynamicRendering = VK_TRUE;
    e13.synchronization2 = VK_TRUE;
    e13.shaderDemoteToHelperInvocation = VK_TRUE;
    e12.timelineSemaphore = VK_TRUE;
    e12.hostQueryReset = VK_TRUE;
    e12.samplerMirrorClampToEdge = f12.samplerMirrorClampToEdge;
    e2.features.geometryShader = VK_TRUE;
    e2.features.independentBlend = VK_TRUE;
    e2.features.textureCompressionBC = VK_TRUE;
    e2.features.imageCubeArray = VK_TRUE;
    e2.features.largePoints = VK_TRUE;
    e2.features.samplerAnisotropy = f2.features.samplerAnisotropy;
    e2.features.depthBiasClamp = f2.features.depthBiasClamp;
    // Latte always clamps depth; Z clipping is a separate PA_CL_CLIP_CNTL
    // switch (Cemu: depthClampEnable always on, depthClipEnable per pipeline).
    e2.features.depthClamp = f2.features.depthClamp;
    e2.features.occlusionQueryPrecise = f2.features.occlusionQueryPrecise;
    e2.features.robustBufferAccess = f2.features.robustBufferAccess;
    e2.features.vertexPipelineStoresAndAtomics = f2.features.vertexPipelineStoresAndAtomics;
    e2.features.fragmentStoresAndAtomics = f2.features.fragmentStoresAndAtomics;
    e2.features.shaderImageGatherExtended = f2.features.shaderImageGatherExtended;
    e2.features.dualSrcBlend = f2.features.dualSrcBlend;
    tail = &e13.pNext;
    VkPhysicalDeviceCustomBorderColorFeaturesEXT e_border{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT e_restart{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT};
    VkPhysicalDeviceTransformFeedbackFeaturesEXT e_xfb{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT};
    VkPhysicalDeviceDepthClipControlFeaturesEXT e_depth_clip{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
    VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR e_divisor{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR};
    VkPhysicalDeviceProvokingVertexFeaturesEXT e_provoking{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT};
    VkPhysicalDeviceDepthClipEnableFeaturesEXT e_clip_enable{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT};
    if (border_ext && border.customBorderColors) {
        e_border.customBorderColors = VK_TRUE;
        e_border.customBorderColorWithoutFormat = border.customBorderColorWithoutFormat;
        *tail = &e_border; tail = &e_border.pNext;
    }
    if (restart_ext && restart.primitiveTopologyListRestart) {
        e_restart.primitiveTopologyListRestart = VK_TRUE;
        *tail = &e_restart; tail = &e_restart.pNext;
    }
    if (xfb_ext && xfb.transformFeedback) {
        e_xfb.transformFeedback = VK_TRUE;
        *tail = &e_xfb; tail = &e_xfb.pNext;
    }
    if (native_depth && depth_clip_ext && depth_clip.depthClipControl) {
        e_depth_clip.depthClipControl = VK_TRUE;
        *tail = &e_depth_clip; tail = &e_depth_clip.pNext;
    }
    if ((divisor_khr || divisor_ext) && divisor.vertexAttributeInstanceRateDivisor) {
        e_divisor.vertexAttributeInstanceRateDivisor = VK_TRUE;
        e_divisor.vertexAttributeInstanceRateZeroDivisor = divisor.vertexAttributeInstanceRateZeroDivisor;
        *tail = &e_divisor; tail = &e_divisor.pNext;
    }
    if (provoking_ext && provoking.provokingVertexLast) {
        e_provoking.provokingVertexLast = VK_TRUE;
        *tail = &e_provoking; tail = &e_provoking.pNext;
    }
    if (clip_enable_ext && clip_enable.depthClipEnable) {
        e_clip_enable.depthClipEnable = VK_TRUE;
        *tail = &e_clip_enable; tail = &e_clip_enable.pNext;
    }
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = s_queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.pNext = &e2;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = uint32_t(enable.size());
    device_info.ppEnabledExtensionNames = enable.data();
    result = vkCreateDevice(s_physical, &device_info, nullptr, &s_device);
    if (result != VK_SUCCESS) {
        RBREW_LOGF(stderr, "[vk] FATAL: vkCreateDevice failed (%d)\n", int(result));
        return false;
    }
    volkLoadDevice(s_device);
    record::install();   // split render thread: per-draw command recording on a second thread
    vkGetDeviceQueue(s_device, s_queue_family, 0, &s_queue);

    s_caps.push_descriptor = push_ext;
    s_caps.negative_one_to_one = e_depth_clip.depthClipControl != 0;
    s_caps.provoking_vertex_last = e_provoking.provokingVertexLast != 0;
    s_caps.depth_clamp = e2.features.depthClamp != 0;
    s_caps.depth_clip_enable = e_clip_enable.depthClipEnable != 0;
    if (push_ext) {
        VkPhysicalDevicePushDescriptorPropertiesKHR push{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &push;
        vkGetPhysicalDeviceProperties2(s_physical, &p2);
        s_caps.max_push_descriptors = push.maxPushDescriptors;
    }
    s_caps.custom_border_color = e_border.customBorderColors;
    s_caps.list_restart = e_restart.primitiveTopologyListRestart;
    s_caps.transform_feedback = e_xfb.transformFeedback;
    s_caps.vertex_divisor = e_divisor.vertexAttributeInstanceRateDivisor;
    s_caps.vertex_divisor_zero = e_divisor.vertexAttributeInstanceRateZeroDivisor;
    s_caps.mirror_clamp_to_edge = e12.samplerMirrorClampToEdge;
    s_caps.anisotropy = e2.features.samplerAnisotropy;
    s_caps.max_anisotropy = props.limits.maxSamplerAnisotropy;
    s_caps.depth_bias_clamp = e2.features.depthBiasClamp;
    s_caps.precise_occlusion = e2.features.occlusionQueryPrecise;
    s_caps.geometry_shader = true;
    s_caps.image_cube_array = true;
    s_caps.robust_buffer_access = e2.features.robustBufferAccess;
    s_caps.d24s8 = format_supports(VK_FORMAT_D24_UNORM_S8_UINT,
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT);
    if (!s_caps.vertex_divisor)
        RBREW_LOGF(stderr, "[vk] WARNING: no vertex attribute divisor support; instanced fetch divisors > 1 are unavailable\n");
    if (!s_caps.precise_occlusion)
        RBREW_LOGF(stderr, "[vk] WARNING: occlusionQueryPrecise is unavailable; GX2 sample counts are not exact\n");

    VkSemaphoreTypeCreateInfo timeline_type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    timeline_type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    semaphore.pNext = &timeline_type;
    vkCreateSemaphore(s_device, &semaphore, nullptr, &s_timeline);
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = s_queue_family;
    vkCreateCommandPool(s_device, &pool, nullptr, &s_command_pool);
    s_ready = true;
    RBREW_LOGF(stderr, "[vk] device %s, Vulkan %u.%u.%u, validation=%s, push descriptors=%u, border=%d list restart=%d xfb=%d divisor=%d provoking=%d\n",
                 props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
                 VK_API_VERSION_PATCH(props.apiVersion), s_caps.sync_validation ? "sync" : s_caps.validation ? "on" : "off",
                 s_caps.push_descriptor ? s_caps.max_push_descriptors : 0, int(s_caps.custom_border_color), int(s_caps.list_restart),
                 int(s_caps.transform_feedback), int(s_caps.vertex_divisor), int(s_caps.provoking_vertex_last));
    if (s_surface && !create_swapchain())
        RBREW_LOGF(stderr, "[vk] swapchain creation deferred (window not yet visible)\n");
    return true;
}

void shutdown() {
    record::stop();
    s_format_properties.clear();
    if (!s_device) return;
#ifdef RBREW_TEXTURE_DIAGNOSTICS
    const auto diagnostic_wait=vkDeviceWaitIdle(s_device);
    s_diagnostic_shutdown_idle=diagnostic_wait==VK_SUCCESS;
    txdiag::event("device_shutdown_wait","\"result\":%d",int(diagnostic_wait));
#else
    vkDeviceWaitIdle(s_device);
#endif
    poll_completed();
    while (!s_deferred.empty()) { s_deferred.front().f(); s_deferred.pop_front(); }
    TXDIAG(s_diagnostic_shutdown_idle=false);
    for (auto& [key, sampler] : s_samplers) {
        TXDIAG(txdiag::retire("sampler",txdiag::handle(sampler),s_completed);
            txdiag::destroyed("sampler",txdiag::handle(sampler),txdiag::resource_id("sampler",txdiag::handle(sampler)),s_completed));
        vkDestroySampler(s_device, sampler, nullptr);
    }
    s_samplers.clear();
    for (auto& chunk : s_chunks) {
        vkDestroyBuffer(s_device, chunk.buffer.buffer, nullptr);
        free_now(chunk.buffer.memory);
    }
    s_chunks.clear();
    s_active_chunk = -1;
    s_live_chunk_ids.clear();
    destroy_swapchain_objects(false);
    for (auto& s : s_swapchain.acquired) if (s) { vkDestroySemaphore(s_device, s, nullptr); s = VK_NULL_HANDLE; }
    if (s_command_pool) vkDestroyCommandPool(s_device, s_command_pool, nullptr);
    s_command_pool = VK_NULL_HANDLE;
    s_free_commands.clear(); s_pending_commands.clear(); s_current = VK_NULL_HANDLE; s_prologue = VK_NULL_HANDLE;
    if (s_timeline) vkDestroySemaphore(s_device, s_timeline, nullptr);
    s_timeline = VK_NULL_HANDLE;
    for (auto& pool : s_pools) for (auto& block : pool.blocks) if (block.memory) vkFreeMemory(s_device, block.memory, nullptr);
    s_pools.clear();
    vkDestroyDevice(s_device, nullptr);
    s_device = VK_NULL_HANDLE;
    if (s_surface) vkDestroySurfaceKHR(s_instance, s_surface, nullptr);
    s_surface = VK_NULL_HANDLE;
    if (s_messenger) vkDestroyDebugUtilsMessengerEXT(s_instance, s_messenger, nullptr);
    s_messenger = VK_NULL_HANDLE;
    vkDestroyInstance(s_instance, nullptr);
    s_instance = VK_NULL_HANDLE;
    s_ready = false;
    s_next_serial = 1; s_completed = 0; s_recorded = false;
    s_dirty = false; s_allocated = 0; s_allocated_by_type = {};
    s_before_end = nullptr; s_after_submit = nullptr; s_leave_rendering = nullptr;
    s_last_present.clear(); s_last_present_w = s_last_present_h = 0;
    s_capture_presents = false;
}

bool ready() { return s_ready; }
const Caps& caps() { return s_caps; }
VkInstance instance() { return s_instance; }
VkPhysicalDevice physical_device() { return s_physical; }
VkDevice device() { return s_device; }
VkQueue queue() { return s_queue; }
uint32_t queue_family() { return s_queue_family; }
uint64_t validation_error_count() { std::lock_guard lock(s_validation_mutex); return s_validation_errors; }
uint64_t validation_warning_count() { std::lock_guard lock(s_validation_mutex); return s_validation_warnings; }
std::string last_validation_error() { std::lock_guard lock(s_validation_mutex); return s_last_validation_error; }

// ---------------------------------------------------------------------------
// Memory
// ---------------------------------------------------------------------------
Allocation allocate(const VkMemoryRequirements& requirements, MemoryUse use, bool linear) {
    VkMemoryPropertyFlags required = 0, preferred = 0;
    switch (use) {
    case MemoryUse::DeviceLocal: required = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT; break;
    case MemoryUse::Upload:
        required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        preferred = required | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        break;
    case MemoryUse::Readback:
        required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        preferred = required | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        break;
    }
    int type = find_memory_type(requirements.memoryTypeBits, required, preferred | required);
    // A device-local upload heap is only worth it when it is large (ReBAR);
    // a 256 MiB BAR window would be exhausted by streamed geometry.
    if (use == MemoryUse::Upload && type >= 0 &&
        (s_memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
        s_memory.memoryHeaps[s_memory.memoryTypes[type].heapIndex].size < (1ull << 30))
        type = find_memory_type(requirements.memoryTypeBits, required, required);
    if (type < 0) return {};
    Allocation a;
    a.size = requirements.size;
    if (requirements.size >= kDedicatedThreshold) {
        uint8_t* mapped = nullptr;
        a.memory = raw_allocate(requirements.size, uint32_t(type), &mapped);
        a.mapped = mapped;
        a.block = -1;
        a.pool = uint32_t(type);
        return a.memory ? a : Allocation{};
    }
    const uint32_t p = pool_index(uint32_t(type), linear);
    auto& pool = s_pools[p];
    const VkDeviceSize alignment = std::max<VkDeviceSize>(requirements.alignment, 16);
    for (uint32_t b = 0; b < pool.blocks.size(); ++b) {
        auto& block = pool.blocks[b];
        if (!block.memory) continue;
        VkDeviceSize offset = 0;
        if (block_allocate(block, requirements.size, alignment, offset)) {
            a.memory = block.memory; a.offset = offset; a.block = int32_t(b); a.pool = p;
            a.mapped = block.mapped ? block.mapped + offset : nullptr;
            return a;
        }
    }
    Block block;
    block.memory = raw_allocate(kBlockSize, uint32_t(type), &block.mapped);
    if (!block.memory) return {};
    block.free[0] = kBlockSize;
    // Allocation handles contain block indices. Reuse vacant slots without
    // moving live blocks or accumulating metadata after transient peaks.
    uint32_t index = 0;
    while (index < pool.blocks.size() && pool.blocks[index].memory) ++index;
    if (index == pool.blocks.size()) pool.blocks.push_back(std::move(block));
    else pool.blocks[index] = std::move(block);
    auto& added = pool.blocks[index];
    VkDeviceSize offset = 0;
    block_allocate(added, requirements.size, alignment, offset);
    a.memory = added.memory; a.offset = offset; a.block = int32_t(index); a.pool = p;
    a.mapped = added.mapped ? added.mapped + offset : nullptr;
    return a;
}

void free_now(Allocation& a) {
    if (!a.memory) return;
    if (a.block < 0) {
        vkFreeMemory(s_device, a.memory, nullptr);
        s_allocated -= a.size;
        s_allocated_by_type[a.pool] -= a.size;
    } else if (a.pool < s_pools.size() && uint32_t(a.block) < s_pools[a.pool].blocks.size()) {
        auto& blocks = s_pools[a.pool].blocks;
        auto& block = blocks[a.block];
        block_release(block, a.offset, a.size);
        // One empty block stays warm for this memory type/resource class.
        // Other completely unused blocks need not retain the lifetime peak.
        // free_now is called only after their last GPU use has completed.
        if (!block.used && std::any_of(blocks.begin(), blocks.end(), [&](const Block& other) {
                return &other != &block && other.memory && !other.used;
            })) {
            vkFreeMemory(s_device, block.memory, nullptr);
            s_allocated -= kBlockSize;
            s_allocated_by_type[s_pools[a.pool].type] -= kBlockSize;
            block = {};
        }
    }
    a = {};
}

uint64_t allocated_bytes() { return s_allocated; }
MemoryUsage memory_usage() {
    MemoryUsage result;
    for (uint32_t i = 0; i < s_memory.memoryTypeCount; ++i) {
        const auto flags = s_memory.memoryTypes[i].propertyFlags;
        if (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
            result.device_local += s_allocated_by_type[i];
            if (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) result.host_visible_device_local += s_allocated_by_type[i];
        } else result.host_only += s_allocated_by_type[i];
    }
    return result;
}

// ---------------------------------------------------------------------------
// Command buffers and submissions
// ---------------------------------------------------------------------------
uint64_t recording_serial() { return s_next_serial; }
uint64_t completed_serial() { poll_completed(); return s_completed; }
uint64_t known_completed_serial() { return s_completed; }

bool wait_serial(uint64_t serial, uint64_t timeout_ns) {
    if (serial >= s_next_serial) submit(false);
    if (serial <= s_completed) { poll_completed(); return true; }
    VkSemaphoreWaitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    info.semaphoreCount = 1;
    info.pSemaphores = &s_timeline;
    info.pValues = &serial;
    const VkResult result = vkWaitSemaphores(s_device, &info, timeout_ns);
    TXDIAG(if(result!=VK_SUCCESS)txdiag::event("wait_failed","\"waiting_for\":%llu,\"result\":%d",serial,int(result)));
    poll_completed();
    if (result == VK_ERROR_DEVICE_LOST) RBREW_LOGF(stderr, "[vk] FATAL: device lost while waiting for submission %llu\n", (unsigned long long)serial);
    return result == VK_SUCCESS;
}

void defer(std::function<void()> f) { s_deferred.push_back({s_next_serial, std::move(f)}); }

namespace {
VkCommandBuffer begin_command_buffer() {
    poll_completed();
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (!s_free_commands.empty()) {
        cb = s_free_commands.front().cb;
        s_free_commands.pop_front();
        vkResetCommandBuffer(cb, 0);
    } else {
        VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        info.commandPool = s_command_pool;
        info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        info.commandBufferCount = 1;
        vkAllocateCommandBuffers(s_device, &info, &cb);
    }
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &begin);
    return cb;
}
VkCommandBuffer prologue() {
    if (!s_prologue) s_prologue = begin_command_buffer();
    return s_prologue;
}
}

VkCommandBuffer cmd() {
    if (!s_current) s_current = begin_command_buffer();
    s_recorded = true;
    return s_current;
}

bool has_recorded_commands() { return (s_current && s_recorded) || s_prologue; }

void set_submit_hooks(void (*before_end)(), void (*after_submit)(uint64_t)) {
    s_before_end = before_end;
    s_after_submit = after_submit;
}
void set_leave_rendering_hook(void (*leave)()) { s_leave_rendering = leave; }

namespace {
uint64_t submit_with(const VkSemaphore* wait, VkPipelineStageFlags2 wait_stage, const VkSemaphore* signal) {
    if (!s_current) cmd();
    if (s_before_end) s_before_end();
    vkEndCommandBuffer(s_current);
    const uint64_t serial = s_next_serial++;
    VkCommandBufferSubmitInfo cbs[2]{{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}, {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}};
    uint32_t cb_count = 0;
    if (s_prologue) {
        vkEndCommandBuffer(s_prologue);
        cbs[cb_count++].commandBuffer = s_prologue;
        s_pending_commands.push_back({s_prologue, serial});
        s_prologue = VK_NULL_HANDLE;
    }
    cbs[cb_count++].commandBuffer = s_current;
    VkSemaphoreSubmitInfo signals[2]{{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}, {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}};
    signals[0].semaphore = s_timeline;
    signals[0].value = serial;
    signals[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    uint32_t signal_count = 1;
    if (signal) {
        signals[1].semaphore = *signal;
        signals[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        signal_count = 2;
    }
    VkSemaphoreSubmitInfo wait_info{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    if (wait) { wait_info.semaphore = *wait; wait_info.stageMask = wait_stage; }
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.commandBufferInfoCount = cb_count;
    submit.pCommandBufferInfos = cbs;
    submit.signalSemaphoreInfoCount = signal_count;
    submit.pSignalSemaphoreInfos = signals;
    submit.waitSemaphoreInfoCount = wait ? 1 : 0;
    submit.pWaitSemaphoreInfos = &wait_info;
    const VkResult result = vkQueueSubmit2(s_queue, 1, &submit, VK_NULL_HANDLE);
    TXDIAG(txdiag::count(txdiag::Submissions);txdiag::journal().serial=s_next_serial;txdiag::journal().completed=s_completed;
        if(result!=VK_SUCCESS || txdiag::sample("submit",0))txdiag::event("submit","\"submitted\":%llu,\"command_buffers\":%u,\"result\":%d",serial,cb_count,int(result)));
    if (result != VK_SUCCESS) RBREW_LOGF(stderr, "[vk] FATAL: vkQueueSubmit2 failed (%d)\n", int(result));
    s_pending_commands.push_back({s_current, serial});
    s_current = VK_NULL_HANDLE;
    s_recorded = false;
    ++s_submissions;
    // The ring chunk in use stays reserved until this submission completes.
    if (s_active_chunk >= 0) s_chunks[s_active_chunk].last_serial = serial;
    // Commands in later command buffers are ordered after this one by the
    // barrier rules (first scope includes earlier submissions); writes made
    // here still require a barrier before later reads.
    if (s_after_submit) s_after_submit(serial);
    return serial;
}
}

uint64_t submit(bool wait) {
    if (!s_ready) return 0;
    if (!has_recorded_commands()) {
        if (s_current) { /* empty command buffer: still submit for ordering */ }
        else {
            if (wait) wait_serial(s_next_serial - 1);
            return s_next_serial - 1;
        }
    }
    const uint64_t serial = submit_with(nullptr, 0, nullptr);
    if (wait) wait_serial(serial);
    else poll_completed();
    return serial;
}

bool finish() {
    if (!s_ready) return false;
    const uint64_t serial = submit(false);
    return wait_serial(serial);
}

uint64_t submission_count() { return s_submissions; }

// ---------------------------------------------------------------------------
// Barriers
// ---------------------------------------------------------------------------
void note_gpu_write() { s_dirty = true; }
void note_gpu_write(Image& image) {
    image.content_generation = ++s_image_generation; note_gpu_write();
    TXDIAG(txdiag::count(txdiag::ImageWrites);txdiag::use("image",txdiag::handle(image.image),s_next_serial);
        if(txdiag::sample("image_write",txdiag::resource_id("image",txdiag::handle(image.image))))
            txdiag::event("image_write","\"image_id\":%llu,\"content\":%llu",txdiag::resource_id("image",txdiag::handle(image.image)),image.content_generation));
}
bool gpu_dirty() { return s_dirty; }
void full_barrier() {
    TXDIAG(txdiag::count(txdiag::Barriers);
        if(txdiag::sample("barrier",0))txdiag::event("barrier","\"src_stage\":%llu,\"dst_stage\":%llu,\"src_access\":%llu,\"dst_access\":%llu",uint64_t(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT),uint64_t(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT),uint64_t(VK_ACCESS_2_MEMORY_WRITE_BIT),uint64_t(VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT)));
    leave_rendering();
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd(), &dependency);
    s_dirty = false;
    ++s_barriers;
}
void barrier_if_dirty() { if (s_dirty) full_barrier(); }
uint64_t barrier_count() { return s_barriers; }

// ---------------------------------------------------------------------------
// Upload ring
// ---------------------------------------------------------------------------
Span upload(VkDeviceSize bytes, VkDeviceSize alignment) {
    alignment = std::max<VkDeviceSize>(alignment, 16);
    bytes = std::max<VkDeviceSize>(bytes, 4);
    if (bytes > kChunkSize) {
        // Rare oversized transfer: a dedicated buffer freed with this submission.
        auto buffer = create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            MemoryUse::Upload);
        if (!buffer.buffer || !buffer.memory.mapped) return {};
        Span span{buffer.buffer, 0, bytes, buffer.memory.mapped, 0};
        // This callback already runs after the consuming submission. Calling
        // destroy_buffer here would defer again to a future submission that
        // may never exist while the renderer is idle.
        defer([buffer]() mutable {
            vkDestroyBuffer(s_device, buffer.buffer, nullptr);
            free_now(buffer.memory);
        });
        return span;
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (s_active_chunk >= 0) {
            auto& chunk = s_chunks[s_active_chunk];
            const VkDeviceSize start = (chunk.used + alignment - 1) / alignment * alignment;
            if (start + bytes <= kChunkSize) {
                chunk.used = start + bytes;
                chunk.last_serial = s_next_serial;
                return {chunk.buffer.buffer, start, bytes, chunk.buffer.memory.mapped + start, chunk.id};
            }
            chunk.last_serial = s_next_serial;
        }
        // Retire the active chunk and pick a completed one (or make one).
        poll_completed();
        int next = -1;
        for (int i = 0; i < int(s_chunks.size()); ++i)
            if (i != s_active_chunk && s_chunks[i].last_serial <= s_completed) { next = i; break; }
        if (next < 0) {
            Chunk chunk;
            chunk.buffer = create_buffer(kChunkSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                MemoryUse::Upload);
            if (!chunk.buffer.buffer || !chunk.buffer.memory.mapped) {
                RBREW_LOGF(stderr, "[vk] FATAL: cannot allocate a host-visible upload chunk\n");
                return {};
            }
            s_chunks.push_back(std::move(chunk));
            next = int(s_chunks.size() - 1);
        }
        s_active_chunk = next;
        s_chunks[next].used = 0;
        s_chunks[next].id = ++s_chunk_ids;
    }
    return {};
}

bool span_resident(const Span& span) {
    if (!span.chunk_id) return false;
    return std::any_of(s_chunks.begin(), s_chunks.end(), [&](const Chunk& chunk) { return chunk.id == span.chunk_id; });
}

bool span_alive(const Span& span) {
    if (!span.chunk_id) return false;
    for (auto& chunk : s_chunks) {
        if (chunk.id != span.chunk_id) continue;
        // The caller reuses the span in the recording in progress: keep the
        // chunk reserved until that submission completes, or the ring would
        // recycle (and overwrite) it while those commands still read it.
        chunk.last_serial = std::max(chunk.last_serial, s_next_serial);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Buffers
// ---------------------------------------------------------------------------
Buffer create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, MemoryUse use) {
    Buffer b;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = std::max<VkDeviceSize>(size, 16);
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(s_device, &info, nullptr, &b.buffer) != VK_SUCCESS) return {};
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(s_device, b.buffer, &requirements);
    b.memory = allocate(requirements, use, true);
    if (!b.memory || vkBindBufferMemory(s_device, b.buffer, b.memory.memory, b.memory.offset) != VK_SUCCESS) {
        vkDestroyBuffer(s_device, b.buffer, nullptr);
        free_now(b.memory);
        return {};
    }
    b.size = info.size;
    return b;
}

void destroy_buffer(Buffer& b) {
    if (!b.buffer) return;
    VkBuffer buffer = b.buffer;
    Allocation memory = b.memory;
    b = {};
    defer([buffer, memory]() mutable { vkDestroyBuffer(s_device, buffer, nullptr); free_now(memory); });
}

// ---------------------------------------------------------------------------
// Images
// ---------------------------------------------------------------------------
bool format_is_depth(VkFormat f) {
    switch (f) {
    case VK_FORMAT_D16_UNORM: case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D16_UNORM_S8_UINT: case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT: return true;
    default: return false;
    }
}
bool format_has_stencil(VkFormat f) {
    return f == VK_FORMAT_D16_UNORM_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_S8_UINT;
}
VkImageAspectFlags format_aspect(VkFormat f) {
    if (!format_is_depth(f)) return f == VK_FORMAT_S8_UINT ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    return VK_IMAGE_ASPECT_DEPTH_BIT | (format_has_stencil(f) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
}
bool format_is_integer(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R16_UINT:
    case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32G32_UINT:
    case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8B8A8_SINT:
    case VK_FORMAT_R16_SINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R32_SINT:
    case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32A32_SINT: case VK_FORMAT_A2B10G10R10_UINT_PACK32: return true;
    default: return false;
    }
}
bool format_is_compressed(VkFormat f) { return f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK; }
bool format_supports(VkFormat format, VkFormatFeatureFlags features, bool optimal) {
    // These capabilities are immutable for the selected physical device.
    // Draw submission asks about attachment blend support for every object.
    auto [it,inserted]=s_format_properties.try_emplace(format);
    if(inserted)vkGetPhysicalDeviceFormatProperties(s_physical,format,&it->second);
    const auto& props=it->second;
    return ((optimal ? props.optimalTilingFeatures : props.linearTilingFeatures) & features) == features;
}

uint32_t format_block_bytes(VkFormat f, VkImageAspectFlags aspect) {
    if (aspect == VK_IMAGE_ASPECT_STENCIL_BIT) return 1;
    switch (f) {
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: case VK_FORMAT_R8_SNORM: return 1;
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16_UNORM:
    case VK_FORMAT_R16_UINT: case VK_FORMAT_R5G6B5_UNORM_PACK16: case VK_FORMAT_D16_UNORM: case VK_FORMAT_R8G8_SNORM:
    case VK_FORMAT_R16_SINT: case VK_FORMAT_R16_SNORM: case VK_FORMAT_R4G4B4A4_UNORM_PACK16: case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
    case VK_FORMAT_R5G5B5A1_UNORM_PACK16: return 2;
    case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R16G16B16A16_SINT:
    case VK_FORMAT_R16G16B16A16_SNORM: case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC4_UNORM_BLOCK: case VK_FORMAT_BC4_SNORM_BLOCK: return 8;
    case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT:
    case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC2_SRGB_BLOCK: case VK_FORMAT_BC3_UNORM_BLOCK: case VK_FORMAT_BC3_SRGB_BLOCK:
    case VK_FORMAT_BC5_UNORM_BLOCK: case VK_FORMAT_BC5_SNORM_BLOCK: case VK_FORMAT_BC7_UNORM_BLOCK: return 16;
    case VK_FORMAT_R32G32B32_SFLOAT: case VK_FORMAT_R32G32B32_UINT: return 12;
    case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_X8_D24_UNORM_PACK32: return 4;  // depth aspect copies as 32-bit words
    case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D32_SFLOAT_S8_UINT: return 4;
    default: return 4;
    }
}

bool create_image(Image& image, const ImageDesc& desc) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.flags = desc.flags;
    info.imageType = desc.type;
    info.format = desc.format;
    info.extent = {std::max(1u, desc.width), std::max(1u, desc.height), desc.type == VK_IMAGE_TYPE_3D ? std::max(1u, desc.depth) : 1u};
    info.mipLevels = std::max(1u, desc.levels);
    info.arrayLayers = desc.type == VK_IMAGE_TYPE_3D ? 1u : std::max(1u, desc.layers);
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = desc.usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(s_device, &info, nullptr, &image.image) != VK_SUCCESS) {
        TXDIAG(txdiag::event("image_create_failed","\"format\":%u,\"width\":%u,\"height\":%u",uint32_t(desc.format),desc.width,desc.height));
        RBREW_LOGF(stderr, "[vk] vkCreateImage failed (format %d %ux%ux%u levels %u layers %u)\n", int(desc.format),
                     desc.width, desc.height, desc.depth, desc.levels, desc.layers);
        image.image = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(s_device, image.image, &requirements);
    image.memory = allocate(requirements, MemoryUse::DeviceLocal, false);
    if (!image.memory || vkBindImageMemory(s_device, image.image, image.memory.memory, image.memory.offset) != VK_SUCCESS) {
        TXDIAG(txdiag::event("image_memory_failed","\"image\":%llu,\"memory\":%llu",txdiag::handle(image.image),txdiag::handle(image.memory.memory)));
        RBREW_LOGF(stderr, "[vk] FATAL: out of device memory for a %ux%u image\n", desc.width, desc.height);
        vkDestroyImage(s_device, image.image, nullptr);
        image.image = VK_NULL_HANDLE;
        free_now(image.memory);
        return false;
    }
    image.desc = desc;
    image.desc.levels = info.mipLevels;
    image.desc.layers = info.arrayLayers;
    image.aspect = format_aspect(desc.format);
    image.content_generation = ++s_image_generation;
    image.views.clear();
    TXDIAG(const auto id=txdiag::create("image",txdiag::handle(image.image));
        txdiag::event("image_storage","\"image_id\":%llu,\"memory\":%llu,\"offset\":%llu,\"bytes\":%llu,\"format\":%u,\"width\":%u,\"height\":%u,\"depth\":%u,\"layers\":%u,\"mips\":%u,\"type\":%u,\"flags\":%u,\"usage\":%u,\"content\":%llu",id,txdiag::handle(image.memory.memory),image.memory.offset,image.memory.size,uint32_t(desc.format),desc.width,desc.height,desc.depth,desc.layers,desc.levels,uint32_t(desc.type),desc.flags,desc.usage,image.content_generation));
    // Transition to GENERAL once; it remains the layout for every use. The
    // first scope covers earlier commands only for the new image.
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image.image;
    barrier.subresourceRange = {image.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(prologue(), &dependency);
    TXDIAG(txdiag::use("image",txdiag::handle(image.image),s_next_serial);
        txdiag::event("image_layout","\"image_id\":%llu,\"old_layout\":%u,\"new_layout\":%u,\"prologue\":%llu",txdiag::resource_id("image",txdiag::handle(image.image)),uint32_t(barrier.oldLayout),uint32_t(barrier.newLayout),txdiag::handle(s_prologue)));
    ++s_images;
    return true;
}

void destroy_image(Image& image) {
    if (!image.image) return;
    VkImage handle = image.image;
    Allocation memory = image.memory;
    std::vector<VkImageView> views;
    for (auto& v : image.views) views.push_back(v.view);
#ifdef RBREW_TEXTURE_DIAGNOSTICS
    const uint64_t diagnostic_id=txdiag::resource_id("image",txdiag::handle(handle));
    const uint64_t retirement_serial=s_next_serial;
    txdiag::retire("image",txdiag::handle(handle),retirement_serial);
    std::vector<uint64_t> diagnostic_views;
    for(auto v:views){diagnostic_views.push_back(txdiag::resource_id("view",txdiag::handle(v)));txdiag::retire("view",txdiag::handle(v),retirement_serial);}
#endif
    image.image = VK_NULL_HANDLE;
    image.views.clear();
    image.memory = {};
    --s_images;
    defer([handle, memory, views
#ifdef RBREW_TEXTURE_DIAGNOSTICS
           , diagnostic_id, diagnostic_views, retirement_serial
#endif
          ]() mutable {
        // shutdown() waits for the entire device, then reclaims callbacks
        // tagged for a recording that may never have been submitted. That is
        // safe too; distinguish it from normal serial-based reclamation.
        TXDIAG(txdiag::event("image_reclaim","\"image_id\":%llu,\"retire_serial\":%llu,\"gpu_completed\":%llu,\"shutdown_idle\":%s,\"valid\":%s",diagnostic_id,retirement_serial,s_completed,s_diagnostic_shutdown_idle?"true":"false",s_completed>=retirement_serial || s_diagnostic_shutdown_idle?"true":"false");
            txdiag::destroyed("image",txdiag::handle(handle),diagnostic_id,s_completed);
            for(size_t i=0;i<views.size();++i)txdiag::destroyed("view",txdiag::handle(views[i]),diagnostic_views[i],s_completed));
        for (auto v : views) vkDestroyImageView(s_device, v, nullptr);
        vkDestroyImage(s_device, handle, nullptr);
        free_now(memory);
    });
}

VkImageView image_view(Image& image, const ViewDesc& desc) {
    const VkFormat format = desc.format == VK_FORMAT_UNDEFINED ? image.desc.format : desc.format;
    VkImageAspectFlags aspect = desc.aspect ? desc.aspect : image.aspect;
    if (!desc.aspect && (aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    uint64_t key = uint64_t(format) | uint64_t(desc.type) << 24 | uint64_t(aspect) << 28;
    key = key * 1000003u ^ (uint64_t(desc.base_level) | uint64_t(desc.levels) << 8 | uint64_t(desc.base_layer) << 16 | uint64_t(desc.layers) << 40);
    key = key * 1000003u ^ (uint64_t(desc.swizzle.r) | uint64_t(desc.swizzle.g) << 8 | uint64_t(desc.swizzle.b) << 16 | uint64_t(desc.swizzle.a) << 24);
    for (const auto& v : image.views) if (v.key == key) return v.view;
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = image.image;
    info.viewType = desc.type;
    info.format = format;
    info.components = desc.swizzle;
    info.subresourceRange = {aspect, desc.base_level, std::max(1u, desc.levels), desc.base_layer, std::max(1u, desc.layers)};
    VkImageViewUsageCreateInfo usage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    // A reinterpreting view (e.g. sRGB/UNORM, UINT of RGBA8) may not support
    // every usage of the image; restrict it to what the format offers.
    if (format != image.desc.format) {
        VkImageUsageFlags allowed = 0;
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(s_physical, format, &props);
        const auto f = props.optimalTilingFeatures;
        if (f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) allowed |= VK_IMAGE_USAGE_SAMPLED_BIT;
        if (f & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) allowed |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (f & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) allowed |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        if (f & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) allowed |= VK_IMAGE_USAGE_STORAGE_BIT;
        usage.usage = image.desc.usage & allowed;
        if (usage.usage) info.pNext = &usage;
    }
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(s_device, &info, nullptr, &view) != VK_SUCCESS) {
        TXDIAG(txdiag::event("view_create_failed","\"image_id\":%llu,\"format\":%u,\"type\":%u",txdiag::resource_id("image",txdiag::handle(image.image)),uint32_t(format),uint32_t(desc.type)));
        RBREW_LOGF(stderr, "[vk] vkCreateImageView failed (format %d type %d)\n", int(format), int(desc.type));
        return VK_NULL_HANDLE;
    }
    image.views.push_back({key, view});
    TXDIAG(const auto id=txdiag::create("view",txdiag::handle(view),txdiag::resource_id("image",txdiag::handle(image.image)));
        txdiag::event("view_storage","\"view_id\":%llu,\"format\":%u,\"type\":%u,\"base_mip\":%u,\"mips\":%u,\"base_layer\":%u,\"layers\":%u,\"swizzle\":[%u,%u,%u,%u]",id,uint32_t(format),uint32_t(desc.type),desc.base_level,desc.levels,desc.base_layer,desc.layers,uint32_t(desc.swizzle.r),uint32_t(desc.swizzle.g),uint32_t(desc.swizzle.b),uint32_t(desc.swizzle.a)));
    return view;
}

uint64_t image_count() { return s_images; }

VkSampler sampler(const VkSamplerCreateInfo& info, const VkClearColorValue* custom_border) {
    SamplerKey key;
    std::memcpy(key.bytes.data(), &info.flags, SamplerKey::kInfoBytes);
    if (custom_border) {
        key.border = true;
        std::memcpy(key.bytes.data() + SamplerKey::kInfoBytes, custom_border, sizeof(VkClearColorValue));
    }
    auto& sampler = s_samplers[key];
    if (sampler) return sampler;
    VkSamplerCreateInfo create = info;
    VkSamplerCustomBorderColorCreateInfoEXT border{VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    if (custom_border) {
        border.customBorderColor = *custom_border;
        border.format = VK_FORMAT_UNDEFINED;
        create.pNext = &border;
    }
    if (vkCreateSampler(s_device, &create, nullptr, &sampler) != VK_SUCCESS) sampler = VK_NULL_HANDLE;
    TXDIAG(std::string diagnostic_bytes;static constexpr char hex[]="0123456789abcdef";
        for(uint8_t b:key.bytes){diagnostic_bytes+=hex[b>>4];diagnostic_bytes+=hex[b&15];}
        txdiag::event("sampler_key","\"sampler\":%llu,\"custom_border\":%s,\"bytes\":\"%s\"",txdiag::handle(sampler),custom_border?"true":"false",diagnostic_bytes.c_str()));
    TXDIAG(const auto id=txdiag::create("sampler",txdiag::handle(sampler));
        txdiag::event("sampler_state","\"sampler_id\":%llu,\"info_hash\":%llu,\"min_filter\":%u,\"mag_filter\":%u,\"mipmap_mode\":%u,\"wrap\":[%u,%u,%u],\"compare\":%u,\"compare_op\":%u,\"valid\":%s",id,txdiag::hash(key.bytes.data(),key.bytes.size()),uint32_t(info.minFilter),uint32_t(info.magFilter),uint32_t(info.mipmapMode),uint32_t(info.addressModeU),uint32_t(info.addressModeV),uint32_t(info.addressModeW),info.compareEnable,uint32_t(info.compareOp),sampler?"true":"false"));
    return sampler;
}

// ---------------------------------------------------------------------------
// Presentation
// ---------------------------------------------------------------------------
bool has_surface() { return s_surface != VK_NULL_HANDLE; }
uint64_t present_count() { return s_presents; }
VkPresentModeKHR present_mode() { return s_swapchain.mode; }

bool present(Image& source, uint32_t width, uint32_t height) {
    if (s_capture_presents) { s_last_present.clear(); s_last_present_w = s_last_present_h = 0; }
    leave_rendering();
    if (!s_surface) return false;
    int w = 0, h = 0;
    SDL_Vulkan_GetDrawableSize(s_window, &w, &h);
    if (w <= 0 || h <= 0) return false;
    if (!s_swapchain.swapchain || uint32_t(w) != s_swapchain.extent.width || uint32_t(h) != s_swapchain.extent.height) {
        vkQueueWaitIdle(s_queue);
        if (!create_swapchain()) return false;
    }
    auto& slot_serial = s_swapchain.slot_serial[s_swapchain.slot];
    // Bound the frames in flight like a GPU driver does at swap; this waits
    // for GPU work three presents back, never for a display refresh.
    if (slot_serial) wait_serial(slot_serial);
    VkSemaphore acquired = s_swapchain.acquired[s_swapchain.slot];
    uint32_t index = 0;
    VkResult result = vkAcquireNextImageKHR(s_device, s_swapchain.swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        vkQueueWaitIdle(s_queue);
        if (!create_swapchain()) return false;
        result = vkAcquireNextImageKHR(s_device, s_swapchain.swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index);
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) return false;
    VkCommandBuffer cb = cmd();
    barrier_if_dirty();
    VkImage target = s_swapchain.images[index];
    VkImageMemoryBarrier2 to_transfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    to_transfer.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;    // acquire semaphore wait stage
    to_transfer.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    to_transfer.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    to_transfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_transfer.srcQueueFamilyIndex = to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.image = target;
    to_transfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &to_transfer;
    vkCmdPipelineBarrier2(cb, &dependency);
    VkClearColorValue black{};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (source.image && width && height) {
        // The scan image's row 0 is the top of the guest picture, matching
        // the swapchain's top row: no flip (the GPU path flips into its
        // bottom-up default framebuffer).
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {int32_t(width), int32_t(height), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[1] = {int32_t(s_swapchain.extent.width), int32_t(s_swapchain.extent.height), 1};
        vkCmdBlitImage(cb, source.image, VK_IMAGE_LAYOUT_GENERAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    } else {
        vkCmdClearColorImage(cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    }
    VkImageLayout last_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    Buffer readback;
    const VkExtent2D extent = s_swapchain.extent;
    if (s_capture_presents) {
        readback = create_buffer(VkDeviceSize(extent.width) * extent.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUse::Readback);
        VkImageMemoryBarrier2 to_read = to_transfer;
        to_read.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; to_read.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        to_read.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; to_read.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; to_read.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        dependency.pImageMemoryBarriers = &to_read;
        vkCmdPipelineBarrier2(cb, &dependency);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {extent.width, extent.height, 1};
        vkCmdCopyImageToBuffer(cb, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &region);
        VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        host.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT; host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo host_dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        host_dependency.memoryBarrierCount = 1; host_dependency.pMemoryBarriers = &host;
        vkCmdPipelineBarrier2(cb, &host_dependency);
        last_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }
    VkImageMemoryBarrier2 to_present = to_transfer;
    to_present.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    to_present.srcAccessMask = last_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ? VK_ACCESS_2_TRANSFER_WRITE_BIT : 0;
    to_present.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
    to_present.dstAccessMask = 0;
    to_present.oldLayout = last_layout;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    dependency.pImageMemoryBarriers = &to_present;
    vkCmdPipelineBarrier2(cb, &dependency);
    VkSemaphore rendered = s_swapchain.rendered[index];
    const uint64_t serial = submit_with(&acquired, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, &rendered);
    if (readback.buffer) {
        wait_serial(serial);
        s_last_present_w = extent.width; s_last_present_h = extent.height;
        s_last_present.assign(readback.memory.mapped, readback.memory.mapped + size_t(extent.width) * extent.height * 4);
        if (s_swapchain.format == VK_FORMAT_B8G8R8A8_UNORM || s_swapchain.format == VK_FORMAT_B8G8R8A8_SRGB)
            for (size_t i = 0; i < s_last_present.size(); i += 4) std::swap(s_last_present[i], s_last_present[i + 2]);
        destroy_buffer(readback);
    }
    slot_serial = serial;
    s_swapchain.slot = (s_swapchain.slot + 1) % s_swapchain.acquired.size();
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &rendered;
    present.swapchainCount = 1;
    present.pSwapchains = &s_swapchain.swapchain;
    present.pImageIndices = &index;
    result = vkQueuePresentKHR(s_queue, &present);
    ++s_presents;
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        vkQueueWaitIdle(s_queue);
        create_swapchain();
    }
    poll_completed();
    return result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR;
}

// ---------------------------------------------------------------------------
// Readback
// ---------------------------------------------------------------------------
bool read_image(Image& image, VkImageAspectFlags aspect, uint32_t level, uint32_t layer,
                uint32_t width, uint32_t height, std::vector<uint8_t>& out) {
    leave_rendering();
    if (!image.image) return false;
    const VkFormat format = image.desc.format;
    const bool compressed = format_is_compressed(format);
    const uint32_t bw = compressed ? (width + 3) / 4 : width, bh = compressed ? (height + 3) / 4 : height;
    const uint32_t texel = format_block_bytes(format, aspect);
    const VkDeviceSize bytes = VkDeviceSize(bw) * bh * texel;
    Buffer staging = create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUse::Readback);
    if (!staging.buffer) return false;
    VkCommandBuffer cb = cmd();
    barrier_if_dirty();
    VkBufferImageCopy region{};
    region.imageSubresource = {aspect, level, image.desc.type == VK_IMAGE_TYPE_3D ? 0u : layer, 1};
    region.imageOffset = {0, 0, image.desc.type == VK_IMAGE_TYPE_3D ? int32_t(layer) : 0};
    region.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(cb, image.image, VK_IMAGE_LAYOUT_GENERAL, staging.buffer, 1, &region);
    VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    host.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &host;
    vkCmdPipelineBarrier2(cb, &dependency);
    const uint64_t serial = submit(false);
    const bool ok = wait_serial(serial);
    if (ok) {
        out.assign(staging.memory.mapped, staging.memory.mapped + bytes);
        // The copy has completed and no later command can use this one-shot
        // buffer. Do not keep it until a nonexistent next submission.
        vkDestroyBuffer(s_device, staging.buffer, nullptr);
        free_now(staging.memory);
    } else destroy_buffer(staging);
    return ok;
}

void set_name(VkObjectType type, uint64_t handle, const char* name) {
    if (!s_caps.validation || !vkSetDebugUtilsObjectNameEXT || !handle) return;
    VkDebugUtilsObjectNameInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    info.objectType = type;
    info.objectHandle = handle;
    info.pObjectName = name;
    vkSetDebugUtilsObjectNameEXT(s_device, &info);
}

}  // namespace gx2vk

namespace gx2vk {
bool capture_presents(bool enable) { const bool previous = s_capture_presents; s_capture_presents = enable; return previous; }
bool last_present(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) {
    if (s_last_present.empty()) return false;
    rgba = s_last_present; width = s_last_present_w; height = s_last_present_h;
    return true;
}
}  // namespace gx2vk
