// r700_to_glsl.h — AMD R700 / Wii U Latte GPU shader bytecode → GLSL 410 core
//
// Entry point: r700_to_glsl().  Returns a heap-allocated GLSL source string;
// caller must free() the result.  Returns nullptr on fatal parse error.
#pragma once
#include <stdint.h>
#include <array>
#include <span>
#include <vector>
#include <string>

struct R700FetchAttribute {
    uint32_t semantic{}, buffer{}, offset{}, format{}, divisor{}, endian{};
    uint32_t destination_select{};
    bool per_instance{};
};
struct R700UniformEntry { bool is_register; uint8_t bank; uint32_t index, mapped_index; };
struct R700NativeAttribute { uint32_t type{}, components{}, default_endian{}; };
R700NativeAttribute r700_native_attribute(uint32_t gx2_format);
// Vulkan resource interface of one translated stage (Cemu resourceMappingVK
// and uniformOffsetsVK): descriptor set, consecutive bindings and the
// std140 offsets inside the stage's uniform-variable block ("ufBlock").
struct R700VulkanInterface {
    int8_t set{-1};
    int8_t uniform_vars_binding{-1};
    std::array<int8_t,16> uniform_buffer_bindings{};   // -1 = unused
    std::array<uint32_t,16> uniform_buffer_vec4s{};     // declared block size
    std::array<int8_t,18> texture_bindings{};          // -1 = unused
    // GLSL sampler type per texture unit: 0 none, 1 1D, 2 2D, 3 2D array,
    // 4 cube array, 5 3D. integer = usampler, shadow = depth comparison.
    std::array<uint8_t,18> texture_dim{};
    std::array<bool,18> texture_integer{}, texture_shadow{};
    int32_t offset_remapped{-1}, offset_registers{-1}, register_vec4s{0};
    int32_t offset_alpha_ref{-1}, offset_point_size{-1}, offset_frag_coord_scale{-1};
    int32_t offset_window_to_clip{-1};
    std::array<int32_t,18> offset_tex_scale{};         // -1 = unit does not fetch texels
    uint32_t uniform_block_bytes{};
    uint32_t pixel_outputs{};                           // passPixelColor mask
};
struct R700TranslationInfo {
    std::vector<R700UniformEntry> remapped;
    std::array<int8_t, 256> attribute_locations{};
    std::array<int8_t, 16> buffer_bindings{};
    uint32_t uniform_mode{}, register_count{};
    std::array<bool,18> texture_compare{};
    uint32_t streamout_mask{};
    std::array<uint32_t,4> streamout_strides{};
    std::array<std::vector<uint8_t>,4> streamout_written_words;
    R700VulkanInterface vk{};
};

struct R700TranslateInput {
    const uint8_t* data;       // little-endian Latte instruction words
    uint32_t       size;       // byte size of bytecode
    bool           is_pixel;   // false = vertex shader, true = pixel shader
    uint32_t       num_attribs;// number of vertex attribute slots (vertex shader only)
    const uint32_t* context_registers{}; // host endian state from GX2 API calls
    std::span<const R700FetchAttribute> attributes{};
    R700TranslationInfo* info{};
    // Native Vulkan backend: emit with the emitter's nativeVulkan option and
    // declare the base-instance push constant instead of a GPU uniform.
    bool vulkan{};
    bool vulkan_negative_one_to_one{};
};

// Translate R700 bytecode to GLSL 410 core source.
// Returns heap-allocated string (caller must free()).
// Returns nullptr if data/size is invalid.
char* r700_to_glsl(const R700TranslateInput& in);

void r700_shader_registers(uint32_t* regs, const uint8_t* descriptor, bool pixel);
void r700_texture_registers(uint32_t* regs, const uint8_t* texture, uint32_t native_slot);
// Native assembly for Latte three-corner RECT and four-vertex QUAD primitives.
std::string r700_rect_geometry(const char* vertex_source,bool quad=false);

// Vulkan source transforms (native Vulkan backend).
// Integer color attachments: the GPU backend's vec4 outputs store raw bits
// (probe: work/vulkan_port/probes/gl_conventions); Vulkan needs a matching
// integer output type, written with floatBitsToUint/Int at the end of main.
std::string r700_vulkan_integer_outputs(const std::string& pixel_source, uint32_t uint_mask, uint32_t sint_mask);
// Offline replay: capture gl_Position of the last vertex stage with
// VK_EXT_transform_feedback (buffer 0, 16-byte stride).
std::string r700_vulkan_capture_position(const std::string& source);
