#include "gx2_shader_program.h"

GX2ShaderProgram gx2_shader_program(std::span<const uint8_t> mem, uint32_t addr, bool pixel) {
    auto read = [&](uint32_t off) {
        const auto* p = mem.data() + addr + off;
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    };
    if (!addr || uint64_t(addr) + (pixel ? 0xE8 : 0x134) > mem.size()) return {};
    // gx2.rpl: GX2SetVertexShader 0202fef8, GX2SetPixelShader 02030628.
    // Direct program/size take precedence; null program selects GX2RBuffer.
    GX2ShaderProgram p;
    const unsigned size_offset = pixel ? 0xA4 : 0xD0;
    p.address = read(size_offset + 4);
    uint64_t size = read(size_offset);
    if (!p.address) {
        const unsigned r = pixel ? 0xD8 : 0x124;
        size = uint64_t(read(r + 4)) * read(r + 8);
        p.address = read(r + 12);
    }
    p.mode = read(size_offset + 8);
    p.attribute_count = pixel ? 0 : read(0x104);
    if (!p.address || !size || (size & 7) || size > UINT32_MAX ||
        uint64_t(p.address) + size > mem.size()) return {};
    p.size = uint32_t(size);
    return p;
}
