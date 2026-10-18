#include "gx2_fetch_decode.h"

namespace {
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
// Inverse of r700_to_glsl's GX2 attribute type -> hardware data format table.
constexpr uint32_t kGx2ToHw[20] = {1,2,5,6,7,0xD,0xE,0xF,0x10,0x16,0x1A,0x19,0x1D,0x1E,0x1F,0x20,0x2F,0x30,0x22,0x23};
bool gx2_type_from_hw(uint32_t hw, uint32_t& type) {
    for (uint32_t i = 0; i < 20; ++i) if (kGx2ToHw[i] == hw) { type = i; return true; }
    return false;
}
}

bool gx2_decode_fetch_program(const uint8_t* program, uint32_t size, std::vector<R700FetchAttribute>& out,
                              const uint32_t* divisors) {
    out.clear();
    if (!program || size < 16 || (size & 7)) return false;
    for (uint32_t cf = 0; cf + 8 <= size; cf += 8) {
        const uint32_t w0 = le32(program + cf), w1 = le32(program + cf + 4);
        const uint32_t inst = (w1 >> 23) & 0x7F;
        if (inst == 2 || inst == 3) {                          // VTX / VTX_TC clause
            const uint32_t count = ((w1 >> 10) & 7) + (((w1 >> 19) & 1) << 3) + 1;
            const uint64_t clause = uint64_t(w0) * 8;
            if (clause + uint64_t(count) * 16 > size) return false;
            for (uint32_t i = 0; i < count; ++i) {
                const uint8_t* v = program + clause + i * 16;
                const uint32_t v0 = le32(v), v1 = le32(v + 4), v2 = le32(v + 8);
                if ((v0 & 0x1F) != 1) continue;                // only semantic fetches define attributes
                const uint32_t buffer_id = (v0 >> 8) & 0xFF;
                if (buffer_id < 0xA0 || buffer_id >= 0xA0 + 16) return false;
                uint32_t type;
                if (!gx2_type_from_hw((v1 >> 22) & 0x3F, type)) return false;
                const uint32_t nfa = (v1 >> 28) & 3;
                R700FetchAttribute a{};
                a.semantic = v1 & 0xFF;
                a.buffer = buffer_id - 0xA0;
                a.offset = v2 & 0xFFFF;
                a.format = type | (nfa == 1 ? 0x100u : nfa == 2 ? 0x800u : 0u) | (((v1 >> 30) & 1) ? 0x200u : 0u);
                a.endian = (v2 >> 16) & 3;
                a.per_instance = ((v0 >> 5) & 3) == 1;
                if (a.per_instance) {
                    const uint32_t select = (v0 >> 24) & 3;
                    a.divisor = select == 1 || select == 2 ? (divisors ? divisors[select - 1] : 0) : 1;
                    if (!a.divisor) a.divisor = 1;
                }
                for (uint32_t c = 0; c < 4; ++c)
                    a.destination_select |= ((v1 >> (9 + 3 * c)) & 7) << (24 - 8 * c);
                out.push_back(a);
            }
        }
        if ((w1 >> 21) & 1) break;                              // END_OF_PROGRAM
        if (inst == 0x14 || inst == 0x13) break;                // RETURN / end markers in fetch shaders
    }
    return !out.empty();
}
