#include "gx2_pm4_state.h"

// Packet layout and register-space bases are Latte hardware facts (as used by
// Cemu LattePM4.h / LatteCommandProcessor.cpp): type-3 header =
// 0xC0000000 | (payloadWords-1)<<16 | opcode<<8; a register-set payload is a
// register offset followed by consecutive values.
namespace {
uint32_t read_be(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
bool register_base(uint32_t opcode, uint32_t& base) {
    switch (opcode) {
    case 0x68: base = 0x2000; return true; // SET_CONFIG_REG
    case 0x69: base = 0xA000; return true; // SET_CONTEXT_REG
    case 0x6A: base = 0xC000; return true; // SET_ALU_CONST
    case 0x6C: base = 0xF880; return true; // SET_LOOP_CONST
    case 0x6D: base = 0xE000; return true; // SET_RESOURCE
    case 0x6E: base = 0xF000; return true; // SET_SAMPLER
    case 0x6F: base = 0xF3FC; return true; // SET_CTL_CONST (SQ_VTX_BASE_VTX_LOC)
    default: return false;
    }
}
constexpr uint32_t IT_NOP = 0x10;
}

Gx2StaticListState gx2_decode_static_display_list(const uint8_t* bytes, uint32_t size) {
    Gx2StaticListState out;
    if (!bytes || size % 4) return out;
    const uint32_t count = size / 4;
    for (uint32_t i = 0; i < count;) {
        const uint32_t header = read_be(bytes + i * 4);
        const uint32_t type = header >> 30;
        if (type == 2) { ++i; continue; }  // filler
        if (type != 3) return out;         // type 0/1 never appear in GX2 lists
        const uint32_t opcode = (header >> 8) & 0xFF;
        const uint32_t payload = ((header >> 16) & 0x3FFF) + 1;
        if (payload > count - i - 1) return out;
        const uint8_t* data = bytes + (i + 1) * 4;
        uint32_t base;
        if (register_base(opcode, base)) {
            const uint32_t first = base + read_be(data);
            if (uint64_t(first) + (payload - 1) > GX2_LATTE_REGISTER_COUNT) return out;
            for (uint32_t k = 1; k < payload; ++k)
                out.writes.push_back({first + k - 1, read_be(data + k * 4)});
        } else if (opcode != IT_NOP) {
            out.other_opcodes.push_back(opcode);
        }
        i += 1 + payload;
    }
    out.ok = true;
    return out;
}
