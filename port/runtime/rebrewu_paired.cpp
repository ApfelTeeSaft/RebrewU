#include "rbrew_diagnostics.h"
// Espresso paired-single instructions, executed on the CPUState register
// file. Semantics follow decaf-emu (libcpu interpreter_pairedsingle.cpp and
// interpreter_loadstore.cpp psqLoad/psqStore): ps0 lives in f[n], ps1 in
// ps1[n]; results are rounded to single precision, a double inserted into
// slot 1 is truncated, quantized loads/stores use GQR types u8/u16/s8/s16
// with a signed 6-bit scale and saturate on store. Signalling-NaN payload
// details of the hardware are not modelled.
#include "rebrewu_runtime.h"
#include <algorithm>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace {
uint32_t bits(uint32_t w, unsigned first, unsigned last) { // IBM bit numbering
    return (w >> (31u - last)) & ((1u << (last - first + 1u)) - 1u);
}
float as_float(uint32_t v) { float f; std::memcpy(&f, &v, 4); return f; }
uint32_t as_bits(float f) { uint32_t v; std::memcpy(&v, &f, 4); return v; }
double round_single(double v) { return double(float(v)); }
// Double -> float rounding toward zero (decaf truncate_double).
float truncate_single(double v) {
    if (std::isnan(v) || std::isinf(v)) return float(v);
    float f = float(v);
    if (std::isinf(f) || std::fabs(double(f)) > std::fabs(v)) f = std::nextafter(f, 0.0f);
    return f;
}

enum QuantType : uint32_t { QFloat = 0, QU8 = 4, QU16 = 5, QS8 = 6, QS16 = 7 };
int signed_scale(uint32_t scale) { int e = int(scale & 0x3F); return e - ((e & 32) << 1); }
uint32_t quant_size(uint32_t type) {
    return type == QU8 || type == QS8 ? 1u : type == QU16 || type == QS16 ? 2u : 4u;
}
void require_type(uint32_t type) {
    if (type != QFloat && (type < QU8 || type > QS16))
        throw std::runtime_error("psq: GQR selects an undefined quantization type " + std::to_string(type));
}
template<class T> double load_quantized(uint8_t* mem, uint32_t ea, double factor) {
    if constexpr (std::is_same_v<T, float>) return double(as_float(rbrew_read32(mem, ea)));
    else if constexpr (sizeof(T) == 1) return double(T(rbrew_read8(mem, ea))) * factor;
    else return double(T(rbrew_read16(mem, ea))) * factor;
}
template<class T> void load_pair(CPUState* cpu, uint32_t frd, uint32_t ea, uint32_t w, uint32_t scale) {
    // A 16-bit integer times 2^[-31,32] is exactly representable as a normal
    // double. Construct that power directly: no libm call or per-lane type
    // dispatch, with precisely the same ldexp result for every GQR scale.
    const double factor = std::is_same_v<T, float> ? 1.0 :
        std::bit_cast<double>(uint64_t(1023 - signed_scale(scale)) << 52);
    const double p0 = load_quantized<T>(cpu->mem, ea, factor);
    const double p1 = w ? 1.0 : load_quantized<T>(cpu->mem, ea + uint32_t(sizeof(T)), factor);
    cpu->f[frd] = p0; cpu->ps1[frd] = p1;
}
template <typename T> T saturate(double v) {
    const double lo = double(std::numeric_limits<T>::min()), hi = double(std::numeric_limits<T>::max());
    return T(std::max(lo, std::min(v, hi)));
}
void quantize(CPUState* cpu, uint32_t ea, double v, uint32_t type, uint32_t scale) {
    const int e = signed_scale(scale);
    const bool nan = std::isnan(v), neg = std::signbit(v);
    switch (type) {
    case QFloat: {
        uint64_t d; std::memcpy(&d, &v, 8);
        const uint32_t exponent = uint32_t(d >> 52) & 0x7FF;
        // Values below the single range store a signed zero (decaf).
        rbrew_write32(cpu->mem, ea, exponent <= 896 ? (neg ? 0x80000000u : 0u) : as_bits(float(v)));
        return;
    }
    case QU8:  rbrew_write8(cpu->mem, ea, nan ? (neg ? 0 : 0xFF) : saturate<uint8_t>(std::ldexp(v, e))); return;
    case QU16: rbrew_write16(cpu->mem, ea, nan ? (neg ? 0 : 0xFFFF) : saturate<uint16_t>(std::ldexp(v, e))); return;
    case QS8:  rbrew_write8(cpu->mem, ea, uint8_t(nan ? (neg ? -0x80 : 0x7F) : saturate<int8_t>(std::ldexp(v, e)))); return;
    case QS16: rbrew_write16(cpu->mem, ea, uint16_t(nan ? (neg ? -0x8000 : 0x7FFF) : saturate<int16_t>(std::ldexp(v, e)))); return;
    }
    require_type(type);
}

void psq_load(CPUState* cpu, uint32_t frd, uint32_t ea, uint32_t w, uint32_t i) {
    const uint32_t g = cpu->gqr[i], type = (g >> 16) & 7, scale = (g >> 24) & 0x3F;
    switch (type) {
    case QFloat: return load_pair<float>(cpu, frd, ea, w, scale);
    case QU8: return load_pair<uint8_t>(cpu, frd, ea, w, scale);
    case QU16: return load_pair<uint16_t>(cpu, frd, ea, w, scale);
    case QS8: return load_pair<int8_t>(cpu, frd, ea, w, scale);
    case QS16: return load_pair<int16_t>(cpu, frd, ea, w, scale);
    default: require_type(type); return;
    }
}
void psq_store(CPUState* cpu, uint32_t frs, uint32_t ea, uint32_t w, uint32_t i) {
    const uint32_t g = cpu->gqr[i], type = g & 7, scale = (g >> 8) & 0x3F;
    require_type(type);
    quantize(cpu, ea, cpu->f[frs], type, scale);
    if (!w) quantize(cpu, ea + quant_size(type), cpu->ps1[frs], type, scale);
}

void set_pair(CPUState* cpu, uint32_t d, double p0, double p1) { cpu->f[d] = p0; cpu->ps1[d] = p1; }
void record(CPUState* cpu, uint32_t w) { if (w & 1u) cpu->cr[1] = (cpu->fpscr >> 28) & 0xFu; }
void compare(CPUState* cpu, uint32_t crf, double a, double b) {
    const uint32_t c = (std::isnan(a) || std::isnan(b)) ? 1u : a < b ? 8u : a > b ? 4u : 2u;
    cpu->cr[crf] = c;
    cpu->fpscr = (cpu->fpscr & ~0xF000u) | (c << 12); // FPCC
}

[[noreturn]] void undefined(uint32_t word) {
    char text[96];
    std::snprintf(text, sizeof text, "paired-single instruction %08X is not implemented", word);
    throw std::runtime_error(text);
}
}

extern "C" void rbrew_ps(CPUState* cpu, uint32_t w) {
    const uint32_t op = w >> 26, d = bits(w, 6, 10), a = bits(w, 11, 15), b = bits(w, 16, 20), c = bits(w, 21, 25);
    auto ra0 = [&]() { return a ? cpu->r[a] : 0u; };
    switch (op) {
    case 56: case 57: case 60: case 61: {           // psq_l, psq_lu, psq_st, psq_stu
        const uint32_t wbit = bits(w, 16, 16), i = bits(w, 17, 19);
        int32_t disp = int32_t(bits(w, 20, 31) << 20) >> 20;
        const bool update = op == 57 || op == 61;
        const uint32_t ea = (update ? cpu->r[a] : ra0()) + uint32_t(disp);
        if (op < 60) psq_load(cpu, d, ea, wbit, i); else psq_store(cpu, d, ea, wbit, i);
        if (update) cpu->r[a] = ea;
        return;
    }
    case 31: {                                      // mtspr / mfspr of a GQR
        const uint32_t spr = bits(w, 11, 15) | (bits(w, 16, 20) << 5), xo = bits(w, 21, 30);
        const uint32_t index = spr >= 912 && spr <= 919 ? spr - 912 : spr >= 896 && spr <= 903 ? spr - 896 : 8;
        if (index > 7) undefined(w);
        if (xo == 467) cpu->gqr[index] = cpu->r[d];
        else if (xo == 339) cpu->r[d] = cpu->gqr[index];
        else undefined(w);
        return;
    }
    case 4: break;
    default: undefined(w);
    }

    const uint32_t xo6 = bits(w, 25, 30), xo5 = bits(w, 26, 30), xo10 = bits(w, 21, 30);
    if (xo6 == 6 || xo6 == 7 || xo6 == 38 || xo6 == 39) {   // psq_lx, psq_stx, psq_lux, psq_stux
        const uint32_t wbit = bits(w, 21, 21), i = bits(w, 22, 24);
        const bool update = xo6 >= 38;
        const uint32_t ea = (update ? cpu->r[a] : ra0()) + cpu->r[b];
        if ((xo6 & 1u) == 0) psq_load(cpu, d, ea, wbit, i); else psq_store(cpu, d, ea, wbit, i);
        if (update) cpu->r[a] = ea;
        return;
    }
    const double a0 = cpu->f[a], a1 = cpu->ps1[a], b0 = cpu->f[b], b1 = cpu->ps1[b];
    const double c0 = cpu->f[c], c1 = cpu->ps1[c];
    switch (xo5) {
    case 10: set_pair(cpu, d, round_single(a0 + b1), round_single(c1)); record(cpu, w); return;  // ps_sum0
    case 11: set_pair(cpu, d, round_single(c0), round_single(a0 + b1)); record(cpu, w); return;  // ps_sum1
    case 12: set_pair(cpu, d, round_single(a0 * c0), round_single(a1 * c0)); record(cpu, w); return; // ps_muls0
    case 13: set_pair(cpu, d, round_single(a0 * c1), round_single(a1 * c1)); record(cpu, w); return; // ps_muls1
    case 14: set_pair(cpu, d, round_single(std::fma(a0, c0, b0)), round_single(std::fma(a1, c0, b1))); record(cpu, w); return; // ps_madds0
    case 15: set_pair(cpu, d, round_single(std::fma(a0, c1, b0)), round_single(std::fma(a1, c1, b1))); record(cpu, w); return; // ps_madds1
    case 18: set_pair(cpu, d, round_single(a0 / b0), round_single(a1 / b1)); record(cpu, w); return; // ps_div
    case 20: set_pair(cpu, d, round_single(a0 - b0), round_single(a1 - b1)); record(cpu, w); return; // ps_sub
    case 21: set_pair(cpu, d, round_single(a0 + b0), round_single(a1 + b1)); record(cpu, w); return; // ps_add
    case 23: set_pair(cpu, d, a0 >= 0.0 ? c0 : b0, a1 >= 0.0 ? c1 : b1); record(cpu, w); return;    // ps_sel
    case 24: set_pair(cpu, d, round_single(1.0 / b0), round_single(1.0 / b1)); record(cpu, w); return; // ps_res
    case 25: set_pair(cpu, d, round_single(a0 * c0), round_single(a1 * c1)); record(cpu, w); return; // ps_mul
    case 26: set_pair(cpu, d, round_single(1.0 / std::sqrt(b0)), round_single(1.0 / std::sqrt(b1))); record(cpu, w); return; // ps_rsqrte
    case 28: set_pair(cpu, d, round_single(std::fma(a0, c0, -b0)), round_single(std::fma(a1, c1, -b1))); record(cpu, w); return; // ps_msub
    case 29: set_pair(cpu, d, round_single(std::fma(a0, c0, b0)), round_single(std::fma(a1, c1, b1))); record(cpu, w); return; // ps_madd
    case 30: set_pair(cpu, d, -round_single(std::fma(a0, c0, -b0)), -round_single(std::fma(a1, c1, -b1))); record(cpu, w); return; // ps_nmsub
    case 31: set_pair(cpu, d, -round_single(std::fma(a0, c0, b0)), -round_single(std::fma(a1, c1, b1))); record(cpu, w); return; // ps_nmadd
    }
    auto move = [&](uint32_t mode) {                // ps_mr/neg/abs/nabs
        uint32_t p0 = as_bits(float(b0)), p1 = as_bits(truncate_single(b1));
        if (mode == 1) { p0 ^= 0x80000000u; p1 ^= 0x80000000u; }
        if (mode == 2) { p0 &= 0x7FFFFFFFu; p1 &= 0x7FFFFFFFu; }
        if (mode == 3) { p0 |= 0x80000000u; p1 |= 0x80000000u; }
        set_pair(cpu, d, as_float(p0), as_float(p1)); record(cpu, w);
    };
    auto merge = [&](double first, double second) { // slot 1 is truncated (decaf)
        uint64_t sb; std::memcpy(&sb, &second, 8);
        const uint32_t exponent = uint32_t(sb >> 52) & 0x7FF;
        const float s = exponent >= 1151 && exponent < 2047 ? FLT_MAX : truncate_single(second);
        set_pair(cpu, d, double(float(first)), double(s)); record(cpu, w);
    };
    const uint32_t crf = bits(w, 6, 8);
    switch (xo10) {
    case 0:   compare(cpu, crf, a0, b0); return;   // ps_cmpu0
    case 32:  compare(cpu, crf, a0, b0); return;   // ps_cmpo0
    case 64:  compare(cpu, crf, a1, b1); return;   // ps_cmpu1
    case 96:  compare(cpu, crf, a1, b1); return;   // ps_cmpo1
    case 72:  move(0); return;
    case 40:  move(1); return;
    case 264: move(2); return;
    case 136: move(3); return;
    case 528: merge(a0, b0); return;               // ps_merge00
    case 560: merge(a0, b1); return;               // ps_merge01
    case 592: merge(a1, b0); return;               // ps_merge10
    case 624: merge(a1, b1); return;               // ps_merge11
    }
    undefined(w);
}

extern "C" void rbrew_trap(CPUState* cpu, uint32_t w, uint32_t pc) {
    const uint32_t to = bits(w, 6, 10), a = cpu->r[bits(w, 11, 15)];
    const uint32_t b = (w >> 26) == 3 ? uint32_t(int32_t(int16_t(w & 0xFFFFu))) : cpu->r[bits(w, 16, 20)];
    const bool hit = ((to & 16u) && int32_t(a) < int32_t(b)) || ((to & 8u) && int32_t(a) > int32_t(b)) ||
                     ((to & 4u) && a == b) || ((to & 2u) && a < b) || ((to & 1u) && a > b);
    if (!hit) return;
    char text[96];
    std::snprintf(text, sizeof text, "guest trap %08X at %08X (lr=%08X)", w, pc, cpu->lr);
    RBREW_LOGF(stderr, "[rbrew] %s\n", text);
    throw std::runtime_error(text);
}
