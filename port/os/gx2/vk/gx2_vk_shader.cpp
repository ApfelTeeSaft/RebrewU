#include "../../../runtime/rbrew_diagnostics.h"
#include "../../../runtime/texture_diagnostics.h"
#include "gx2_vk_shader.h"
#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <atomic>
#include <future>
#include <mutex>

namespace gx2vk {
namespace {
bool s_glslang = false;
std::string s_cache_dir;
// Module identity: stage, source length and the two 64-bit source hashes
// that also name its on-disk SPIR-V (disk_path). Keying on the full source
// text kept another copy of every translated shader resident.
struct ModuleKey {
    uint64_t a = 0, b = 0;
    uint64_t size = 0;
    uint32_t stage = 0;
    bool operator==(const ModuleKey& o) const { return a == o.a && b == o.b && size == o.size && stage == o.stage; }
};
struct ModuleKeyHash { size_t operator()(const ModuleKey& k) const noexcept { return size_t(k.a ^ (k.b * 0x9E3779B97F4A7C15ull) ^ k.stage); } };
std::unordered_map<ModuleKey, std::shared_future<VkShaderModule>, ModuleKeyHash> s_modules;
std::mutex s_module_mutex, s_glslang_mutex, s_pipeline_mutex;
VkPipelineCache s_pipeline_cache = VK_NULL_HANDLE;
std::atomic<uint64_t> s_compiles{0}, s_disk_hits{0};
// Bump when the compiler, its options or the emitted conventions change, so
// stale on-disk SPIR-V can never satisfy a new key.
constexpr uint32_t kCacheRevision = 1;

glslang_stage_t glslang_stage(VkShaderStageFlagBits stage) {
    switch (stage) {
    case VK_SHADER_STAGE_VERTEX_BIT: return GLSLANG_STAGE_VERTEX;
    case VK_SHADER_STAGE_GEOMETRY_BIT: return GLSLANG_STAGE_GEOMETRY;
    case VK_SHADER_STAGE_FRAGMENT_BIT: return GLSLANG_STAGE_FRAGMENT;
    case VK_SHADER_STAGE_COMPUTE_BIT: return GLSLANG_STAGE_COMPUTE;
    default: return GLSLANG_STAGE_VERTEX;
    }
}

std::string disk_path(VkShaderStageFlagBits stage, const std::string& source) {
    const uint64_t a = stable_hash(source.data(), source.size(), 0xCBF29CE484222325ull ^ (uint64_t(stage) << 32) ^ kCacheRevision);
    const uint64_t b = stable_hash(source.data(), source.size(), 0x84222325CBF29CE4ull + kCacheRevision);
    char name[64];
    std::snprintf(name, sizeof(name), "%016llx%016llx.spv", (unsigned long long)a, (unsigned long long)b);
    return (std::filesystem::path(s_cache_dir) / "spirv" / name).string();
}

bool load_disk(const std::string& path, size_t source_size, std::vector<uint32_t>& spirv) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    uint64_t header[2]{};
    if (!file.read(reinterpret_cast<char*>(header), sizeof(header))) return false;
    if (header[0] != (0x52425350ull << 32 | kCacheRevision) || header[1] == 0) return false;
    // header[1] = source length << 32 | word count
    if ((header[1] >> 32) != source_size) return false;
    spirv.resize(size_t(header[1] & 0xFFFFFFFFu));
    if (!file.read(reinterpret_cast<char*>(spirv.data()), std::streamsize(spirv.size() * 4))) return false;
    return !spirv.empty() && spirv[0] == 0x07230203u;
}

void store_disk(const std::string& path, size_t source_size, const std::vector<uint32_t>& spirv) {
    std::error_code ignored;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ignored);
    const std::string temp = path + ".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) return;
        const uint64_t header[2] = {0x52425350ull << 32 | kCacheRevision, uint64_t(source_size) << 32 | spirv.size()};
        file.write(reinterpret_cast<const char*>(header), sizeof(header));
        file.write(reinterpret_cast<const char*>(spirv.data()), std::streamsize(spirv.size() * 4));
    }
    std::filesystem::rename(temp, path, ignored);
}

std::string pipeline_cache_path() {
    return (std::filesystem::path(s_cache_dir) / "pipelines.bin").string();
}
}  // namespace

uint64_t stable_hash(const void* data, size_t bytes, uint64_t seed) {
    uint64_t h = seed;
    const uint64_t length = bytes;
    for (unsigned i = 0; i < 8; ++i) { h ^= (length >> (i * 8)) & 0xFF; h *= 0x100000001B3ull; }
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < bytes; ++i) { h ^= p[i]; h *= 0x100000001B3ull; }
    return h;
}

void shader_set_cache_dir(const std::string& directory) { s_cache_dir = directory; }
const std::string& shader_cache_dir() { return s_cache_dir; }

bool compile_glsl(VkShaderStageFlagBits stage, const std::string& source, std::vector<uint32_t>& spirv, std::string* log) {
    {
        std::lock_guard lock(s_glslang_mutex);
        if (!s_glslang) {
            if (!glslang_initialize_process()) return false;
            s_glslang = true;
        }
    }
    const std::string path = s_cache_dir.empty() ? std::string() : disk_path(stage, source);
    if (!path.empty() && load_disk(path, source.size(), spirv)) { ++s_disk_hits; return true; }
    glslang_input_t input{};
    input.language = GLSLANG_SOURCE_GLSL;
    input.stage = glslang_stage(stage);
    input.client = GLSLANG_CLIENT_VULKAN;
    input.client_version = GLSLANG_TARGET_VULKAN_1_3;
    input.target_language = GLSLANG_TARGET_SPV;
    input.target_language_version = GLSLANG_TARGET_SPV_1_6;
    input.code = source.c_str();
    input.default_version = 450;
    input.default_profile = GLSLANG_NO_PROFILE;
    input.force_default_version_and_profile = false;
    input.forward_compatible = false;
    input.messages = static_cast<glslang_messages_t>(GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT);
    input.resource = glslang_default_resource();
    glslang_shader_t* shader = glslang_shader_create(&input);
    bool ok = shader && glslang_shader_preprocess(shader, &input) && glslang_shader_parse(shader, &input);
    glslang_program_t* program = nullptr;
    if (ok) {
        program = glslang_program_create();
        glslang_program_add_shader(program, shader);
        ok = glslang_program_link(program, GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT) != 0;
    }
    if (ok) {
        glslang_program_SPIRV_generate(program, input.stage);
        spirv.resize(glslang_program_SPIRV_get_size(program));
        glslang_program_SPIRV_get(program, spirv.data());
        ok = !spirv.empty();
    }
    if (!ok && log) {
        *log = shader ? glslang_shader_get_info_log(shader) : "glslang_shader_create failed";
        if (program) *log += glslang_program_get_info_log(program);
    }
    if (program) glslang_program_delete(program);
    if (shader) glslang_shader_delete(shader);
    ++s_compiles;
    if (ok && !path.empty()) store_disk(path, source.size(), spirv);
    return ok;
}

VkShaderModule shader_module(VkShaderStageFlagBits stage, const std::string& source) {
    ModuleKey key;
    key.a = stable_hash(source.data(), source.size(), 0xCBF29CE484222325ull ^ (uint64_t(stage) << 32) ^ kCacheRevision);
    key.b = stable_hash(source.data(), source.size(), 0x84222325CBF29CE4ull + kCacheRevision);
    key.size = source.size();
    key.stage = uint32_t(stage);
    std::shared_future<VkShaderModule> existing;
    std::promise<VkShaderModule> promise;
    {
        std::lock_guard lock(s_module_mutex);
        auto [it, inserted] = s_modules.try_emplace(key);
        if (!inserted) existing = it->second;
        else it->second = promise.get_future().share();
    }
    if (existing.valid()) return existing.get();
    VkShaderModule module = VK_NULL_HANDLE;
    try {
        std::vector<uint32_t> spirv;
        std::string log;
        if (!compile_glsl(stage, source, spirv, &log)) {
            TXDIAG(txdiag::event("shader_compile_failed","\"stage\":%u,\"source_hash\":%llu,\"message\":%s",uint32_t(stage),key.a,txdiag::quote(log.c_str()).c_str());
                txdiag::artifact("failed-shader-"+std::to_string(key.a)+".glsl",source));
            RBREW_LOGF(stderr, "[vk] shader compilation failed (stage %d):\n%s\n----- source -----\n%s\n", int(stage), log.c_str(), source.c_str());
            promise.set_value(VK_NULL_HANDLE);
            return VK_NULL_HANDLE;
        }
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = spirv.size() * 4;
        info.pCode = spirv.data();
        if (vkCreateShaderModule(device(), &info, nullptr, &module) != VK_SUCCESS) {
            TXDIAG(txdiag::event("shader_module_failed","\"stage\":%u,\"source_hash\":%llu",uint32_t(stage),key.a));
            promise.set_value(VK_NULL_HANDLE);
            return VK_NULL_HANDLE;
        }
        promise.set_value(module);
        return module;
    } catch (...) { promise.set_exception(std::current_exception()); throw; }
}

VkPipelineCache pipeline_cache() {
    std::lock_guard lock(s_pipeline_mutex);
    if (s_pipeline_cache) return s_pipeline_cache;
    std::vector<char> data;
    if (!s_cache_dir.empty()) {
        std::ifstream file(pipeline_cache_path(), std::ios::binary);
        if (file) data.assign(std::istreambuf_iterator<char>(file), {});
    }
    VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    info.initialDataSize = data.size();
    info.pInitialData = data.empty() ? nullptr : data.data();
    // The driver validates the header (vendor/device/UUID) and ignores a
    // mismatching blob; retry empty if it is rejected outright.
    if (vkCreatePipelineCache(device(), &info, nullptr, &s_pipeline_cache) != VK_SUCCESS) {
        info.initialDataSize = 0; info.pInitialData = nullptr;
        vkCreatePipelineCache(device(), &info, nullptr, &s_pipeline_cache);
    }
    return s_pipeline_cache;
}

void shader_cache_flush() {
    if (s_cache_dir.empty() || !s_pipeline_cache) return;
    size_t size = 0;
    if (vkGetPipelineCacheData(device(), s_pipeline_cache, &size, nullptr) != VK_SUCCESS || !size) return;
    std::vector<char> data(size);
    if (vkGetPipelineCacheData(device(), s_pipeline_cache, &size, data.data()) != VK_SUCCESS) return;
    std::error_code ignored;
    std::filesystem::create_directories(s_cache_dir, ignored);
    const std::string temp = pipeline_cache_path() + ".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        file.write(data.data(), std::streamsize(size));
    }
    std::filesystem::rename(temp, pipeline_cache_path(), ignored);
}

void shader_shutdown() {
    if (device()) {
        shader_cache_flush();
        for (auto& [key, module] : s_modules) {
            // Compiler queues have been joined by shutdown_draw_state.
            try { if (auto handle = module.get()) vkDestroyShaderModule(device(), handle, nullptr); } catch (...) {}
        }
        if (s_pipeline_cache) vkDestroyPipelineCache(device(), s_pipeline_cache, nullptr);
    }
    s_modules.clear();
    s_pipeline_cache = VK_NULL_HANDLE;
    if (s_glslang) { glslang_finalize_process(); s_glslang = false; }
}

uint64_t shader_compile_count() { return s_compiles; }
uint64_t shader_disk_hits() { return s_disk_hits; }
uint64_t shader_module_count() { std::lock_guard lock(s_module_mutex); return s_modules.size(); }

}  // namespace gx2vk
