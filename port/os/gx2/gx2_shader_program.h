#pragma once
#include <cstdint>
#include <span>

struct GX2ShaderProgram {
    uint32_t address{}, size{}, mode{}, attribute_count{};
    explicit operator bool() const { return address && size; }
};

// Resolve the GX2 descriptor into a bounded byte range, without a GPU context.
GX2ShaderProgram gx2_shader_program(std::span<const uint8_t> arena,
                                   uint32_t descriptor, bool pixel);
