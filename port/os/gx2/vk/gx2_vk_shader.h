#pragma once
// gx2_vk_shader.h - GLSL (Vulkan semantics) -> SPIR-V for translated Latte
// shaders and the backend's internal shaders.
//
// Modules are cached by exact stage + source text. With a cache directory
// (RBREW_VK_CACHE_DIR, or the directory Gambit selects next to its
// executable) SPIR-V is also stored on disk under a stable content hash of
// the source and compiler revision, and the VkPipelineCache is persisted.
// Identity is the translated source, never a guest address: guest code,
// descriptor and register changes already produce different source/keys.
#include "gx2_vk.h"
#include <string>
#include <vector>

namespace gx2vk {

void shader_set_cache_dir(const std::string& directory);  // empty disables the disk cache
// Directory given to shader_set_cache_dir (empty: no disk cache).
const std::string& shader_cache_dir();
bool compile_glsl(VkShaderStageFlagBits stage, const std::string& source, std::vector<uint32_t>& spirv, std::string* log);
// Thread-safe exact-source cache. Concurrent requests share one compilation.
// VK_NULL_HANDLE on failure (diagnostic tools retain compiler messages).
VkShaderModule shader_module(VkShaderStageFlagBits stage, const std::string& source);
VkPipelineCache pipeline_cache();
void shader_cache_flush();       // writes the pipeline cache to disk
void shader_shutdown();
uint64_t shader_compile_count();      // glslang compilations
uint64_t shader_disk_hits();          // SPIR-V loaded from the disk cache
uint64_t shader_module_count();
// Stable 64-bit content hash (FNV-1a, length-prefixed) used for cache keys.
uint64_t stable_hash(const void* data, size_t bytes, uint64_t seed = 0xCBF29CE484222325ull);

}  // namespace gx2vk
