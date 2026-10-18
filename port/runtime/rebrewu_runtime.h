#pragma once
#include "rbrew_diagnostics.h"
// =============================================================================
// RebrewU Runtime — Wii U → PC
//
// Included by every decompiled Gambit_part*.cpp.  Defines the CPU state,
// memory accessors (big-endian ↔ host byte-swap), and dispatch helpers.
// =============================================================================

#include <stdint.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifndef RBREW_HOST_API
#define RBREW_HOST_API
#endif

// ---------------------------------------------------------------------------
// Forward declaration
// ---------------------------------------------------------------------------
struct CPUState;

// ---------------------------------------------------------------------------
// Host function type
// ---------------------------------------------------------------------------
typedef void (*HostFunc)(CPUState* cpu);

// ---------------------------------------------------------------------------
// Memory size constants
// Wii U address space used by this game:
//   0x02000000 - 0x026143A5   .text (game code — executed natively on PC)
//   0x10000000 - 0x1098xxxx   .rodata
//   0x101C0BC0 - 0x101F0000   .data / .bss / OS structs
//   0xC0000000+               import stubs (handled by dispatch table)
// Reserve [0x00000000, WIIU_MEM_SIZE) as a flat virtual aperture. The native
// application arena is committed initially; Cafe maps the optional overlay.
// ---------------------------------------------------------------------------
#include "memory_layout.h"
#define WIIU_MEM_SIZE RBREW_ARENA_END

// The virtual aperture is sparse: its reserved hole and disabled overlay are
// not guest RAM. Ordinary low accesses stay inline without a registry lookup.
// Arena owners publish overlay accessibility only after the host map/unmap
// succeeds, and clear it before destroying or reusing the arena allocation.
extern "C" RBREW_HOST_API bool rbrew_overlay_accessible(const uint8_t* mem);
extern "C" void rbrew_set_overlay_access(uint8_t* mem, bool enabled);
static inline bool rbrew_memory_accessible(const uint8_t* mem, uint32_t address, uint64_t size) {
    if (!mem || size > WIIU_MEM_SIZE) return false;
    const uint64_t end = uint64_t(address) + size; // widened, bounded size
    if (end <= RBREW_APPLICATION_ARENA_END) return true;
    if (address >= RBREW_SHARED_DATA_BASE) return end <= uint64_t(RBREW_SHARED_DATA_BASE) + RBREW_SHARED_DATA_SIZE;
    if (address >= RBREW_MEM1_BASE) return end <= uint64_t(RBREW_MEM1_BASE) + RBREW_MEM1_SIZE;
    if (address >= RBREW_FG_FREE_BASE) return end <= uint64_t(RBREW_FG_FREE_BASE) + RBREW_FG_FREE_SIZE;
    return address >= RBREW_OVERLAY_BASE && end <= uint64_t(RBREW_OVERLAY_BASE) + RBREW_OVERLAY_SIZE &&
        rbrew_overlay_accessible(mem);
}

// ---------------------------------------------------------------------------
// CPUState — mirrors PowerPC Espresso register file
// ---------------------------------------------------------------------------
struct RbrewGuestCodeRange { uint32_t begin, end; }; // exclusive end

struct CPUState {
    uint32_t r[32];     // GPRs
    double   f[32];     // FPRs (double precision on Espresso)
    uint32_t cr[8];     // Condition Register fields CR0–CR7 (4 bits each, packed as uint32)
    uint32_t lr;        // Link Register
    uint32_t ctr;       // Count Register
    uint32_t xer;       // Integer Exception Register
    uint32_t fpscr{};   // Conversion status; other FP/control instructions remain partial

    uint8_t* mem;       // pointer to base of the flat guest memory arena
    // Hybrid execution is opt-in. PC is authoritative only inside the stepper;
    // legacy whole-function C++ does not yet expose every continuation.
    uint32_t pc{};
    uint32_t guest_memory_size{WIIU_MEM_SIZE};
    const RbrewGuestCodeRange* guest_code_ranges{};
    uint32_t guest_code_range_count{};
    bool ppc_fallback_enabled{};
    uint64_t ppc_instruction_budget{100000};
    bool differential_validation{}; // detached pure replay: reject host dispatch

    // Host function dispatch table: guest address → host implementation
    // Populated at startup for all OS import addresses.
    HostFunc* dispatch_table;  // indexed by guest_addr >> 2
    uint32_t  dispatch_size;   // number of entries

    // Scratch fields used by rbrew_dispatch / rbrew_call_indirect
    uint32_t dispatch_ret_addr;

    // One local reservation per emulated CPU; versions are shared by arena.
    uint8_t* reservation_mem;
    uint32_t reservation_addr;
    uint64_t reservation_version;
    bool reservation_valid;

    // Espresso paired singles: f[n] is ps0, ps1[n] the second slot.
    // GQR0-7 drive psq_l/psq_st quantization (SPR 912-919 / UGQR 896-903).
    double   ps1[32]{};
    uint32_t gqr[8]{};
};

// Paired-single instruction (opcode 4/56/57/60/61, or mtspr/mfspr of a GQR)
// executed on the register file; implemented per decaf-emu's interpreter.
extern "C" RBREW_HOST_API void rbrew_ps(CPUState* cpu, uint32_t word);
// tw/twi: evaluate the TO condition (signed lt/gt, eq, unsigned lt/gt) and,
// when it holds, stop with a guest-trap error instead of continuing.
extern "C" RBREW_HOST_API void rbrew_trap(CPUState* cpu, uint32_t word, uint32_t pc);
// Thread start values set by coreinit (decaf OSCreateThread): GQR2-5 hold
// the u8/u16/s8/s16 quantization types, the rest are 0.
static inline void rbrew_init_thread_gqrs(CPUState* cpu) {
    for (auto& g : cpu->gqr) g = 0;
    cpu->gqr[2] = 0x40004u; cpu->gqr[3] = 0x50005u; cpu->gqr[4] = 0x60006u; cpu->gqr[5] = 0x70007u;
}

extern "C" RBREW_HOST_API uint32_t rbrew_lwarx(CPUState* cpu, uint32_t addr);
extern "C" RBREW_HOST_API void rbrew_stwcx(CPUState* cpu, uint32_t addr, uint32_t value);
// Guest-memory writers use this shared monitor so even an ABA write breaks
// an outstanding reservation. bytes are already in guest endian order.
extern "C" RBREW_HOST_API void rbrew_write_memory(uint8_t* mem, uint32_t addr, const void* bytes, uint32_t size);

// FPRs carry non-floating bit payloads too (fctiwz -> stfiwx/stfd).
static inline uint32_t rbrew_fpr_low_word(double value) {
    uint64_t bits; memcpy(&bits, &value, sizeof(bits)); return (uint32_t)bits;
}

static inline void rbrew_fctiwz(CPUState* cpu, double& destination, double source, bool record) {
    // PowerPC PEM 8-81 / 3-38: truncate regardless of RN, saturate invalid
    // conversions when VE is clear, and suppress the destination when VE is set.
    // MSR-controlled FP exception delivery is not modeled by this runtime.
    uint64_t source_bits; memcpy(&source_bits, &source, sizeof(source_bits));
    const bool nan = (source_bits & 0x7FFFFFFFFFFFFFFFull) > 0x7FF0000000000000ull;
    const bool snan = nan && !(source_bits & 0x0008000000000000ull);
    const bool invalid = nan || source > 2147483647.0 || source < -2147483648.0;
    const uint32_t previous = cpu->fpscr;
    cpu->fpscr &= ~0x00060000u; // FR=0 (toward zero never increments), FI=0
    uint32_t word;
    if (invalid) {
        cpu->fpscr |= 0x00000100u; // VXCVI, sticky
        if (snan) cpu->fpscr |= 0x01000000u; // VXSNAN, sticky
        word = !nan && source > 0.0 ? 0x7FFFFFFFu : 0x80000000u;
    } else {
        const double integral = trunc(source);
        word = (uint32_t)(int32_t)integral; // range checked before host conversion
        if (integral != source) cpu->fpscr |= 0x02020000u; // XX sticky, FI
    }
    if (invalid && (previous & 0x80u)) {
        cpu->fpscr = (cpu->fpscr & ~0x0001F000u) | 0x00011000u; // QNaN FPRF
    } else {
        // High word is architecturally undefined; choose a deterministic tag.
        uint64_t payload = 0xFFF8000000000000ull | word;
        memcpy(&destination, &payload, sizeof(payload));
        // FPRF is undefined for a completed integer conversion; retain it.
    }
    // Recompute summary bits. FX is sticky on newly raised exception bits.
    if ((cpu->fpscr & ~previous) & 0x1FF80700u) cpu->fpscr |= 0x80000000u;
    cpu->fpscr &= ~0x60000000u;
    if (cpu->fpscr & 0x01F80700u) cpu->fpscr |= 0x20000000u;
    if (((cpu->fpscr & 0x20000000u) && (cpu->fpscr & 0x80u)) ||
        ((cpu->fpscr & 0x10000000u) && (cpu->fpscr & 0x40u)) ||
        ((cpu->fpscr & 0x08000000u) && (cpu->fpscr & 0x20u)) ||
        ((cpu->fpscr & 0x04000000u) && (cpu->fpscr & 0x10u)) ||
        ((cpu->fpscr & 0x02000000u) && (cpu->fpscr & 0x08u))) cpu->fpscr |= 0x40000000u;
    if (record) cpu->cr[1] = (cpu->fpscr >> 28) & 15u;
}

// ---------------------------------------------------------------------------
// Memory access — Wii U is big-endian, host may be little-endian
// ---------------------------------------------------------------------------

static inline uint16_t rbrew_bswap16(uint16_t v) {
    return (uint16_t)((v << 8) | (v >> 8));
}
static inline uint32_t rbrew_bswap32(uint32_t v) {
    return ((v & 0xFF000000u) >> 24) | ((v & 0x00FF0000u) >> 8)
         | ((v & 0x0000FF00u) <<  8) | ((v & 0x000000FFu) << 24);
}
static inline uint64_t rbrew_bswap64(uint64_t v) {
    return ((uint64_t)rbrew_bswap32((uint32_t)(v >> 32)))
         | ((uint64_t)rbrew_bswap32((uint32_t)v) << 32);
}

static inline uint8_t rbrew_read8(uint8_t* mem, uint32_t addr) {
    if (!rbrew_memory_accessible(mem, addr, 1)) {
        RBREW_LOGF(stderr, "[rbrew] OOB read8  addr=0x%08X\n", addr); return 0;
    }
    return mem[addr];
}
static inline uint16_t rbrew_read16(uint8_t* mem, uint32_t addr) {
    if (!rbrew_memory_accessible(mem, addr, 2)) {
        RBREW_LOGF(stderr, "[rbrew] OOB read16 addr=0x%08X\n", addr); return 0;
    }
    uint16_t v; memcpy(&v, mem + addr, 2); return rbrew_bswap16(v);
}
static inline uint32_t rbrew_read32(uint8_t* mem, uint32_t addr) {
    if (!rbrew_memory_accessible(mem, addr, 4)) {
        RBREW_LOGF(stderr, "[rbrew] OOB read32 addr=0x%08X\n", addr); return 0;
    }
    uint32_t v; memcpy(&v, mem + addr, 4); return rbrew_bswap32(v);
}
static inline uint64_t rbrew_read64(uint8_t* mem, uint32_t addr) {
    if (!rbrew_memory_accessible(mem, addr, 8)) {
        RBREW_LOGF(stderr, "[rbrew] OOB read64 addr=0x%08X\n", addr); return 0;
    }
    uint64_t v; memcpy(&v, mem + addr, 8); return rbrew_bswap64(v);
}
static inline double rbrew_read_f32(uint8_t* mem, uint32_t addr) {
    uint32_t bits = rbrew_read32(mem, addr);
    float f; memcpy(&f, &bits, 4); return (double)f;
}
static inline double rbrew_read_f64(uint8_t* mem, uint32_t addr) {
    uint64_t bits = rbrew_read64(mem, addr);
    double d; memcpy(&d, &bits, 8); return d;
}

static inline void rbrew_write8(uint8_t* mem, uint32_t addr, uint8_t v) {
    if (!rbrew_memory_accessible(mem, addr, 1)) {
        RBREW_LOGF(stderr, "[rbrew] OOB write8  addr=0x%08X val=0x%02X\n", addr, v); return;
    }
    rbrew_write_memory(mem, addr, &v, 1);
}
static inline void rbrew_write16(uint8_t* mem, uint32_t addr, uint16_t v) {
    if (!rbrew_memory_accessible(mem, addr, 2)) {
        RBREW_LOGF(stderr, "[rbrew] OOB write16 addr=0x%08X val=0x%04X\n", addr, v); return;
    }
    uint16_t be = rbrew_bswap16(v); rbrew_write_memory(mem, addr, &be, 2);
}
static inline void rbrew_write32(uint8_t* mem, uint32_t addr, uint32_t v) {
    if (!rbrew_memory_accessible(mem, addr, 4)) {
        RBREW_LOGF(stderr, "[rbrew] OOB write32 addr=0x%08X val=0x%08X\n", addr, v); return;
    }
    uint32_t be = rbrew_bswap32(v); rbrew_write_memory(mem, addr, &be, 4);
}
static inline void rbrew_write64(uint8_t* mem, uint32_t addr, uint64_t v) {
    if (!rbrew_memory_accessible(mem, addr, 8)) {
        RBREW_LOGF(stderr, "[rbrew] OOB write64 addr=0x%08X\n", addr); return;
    }
    uint64_t be = rbrew_bswap64(v); rbrew_write_memory(mem, addr, &be, 8);
}
static inline void rbrew_write_f32(uint8_t* mem, uint32_t addr, double d) {
    float f = (float)d; uint32_t bits; memcpy(&bits, &f, 4);
    rbrew_write32(mem, addr, bits);
}
static inline void rbrew_write_f64(uint8_t* mem, uint32_t addr, double d) {
    uint64_t bits; memcpy(&bits, &d, 8);
    rbrew_write64(mem, addr, bits);
}

// ---------------------------------------------------------------------------
// Integer helpers
// ---------------------------------------------------------------------------
// Espresso divw/divwu never trap. Undefined-result cases follow the observed
// 750-family values (as Cemu): divw by 0 -> -1 for a negative dividend, else
// 0; INT_MIN / -1 -> 0xFFFFFFFF; divwu by 0 -> 0.
static inline uint32_t rbrew_divw(uint32_t a, uint32_t b) {
    if (b == 0) return int32_t(a) < 0 ? 0xFFFFFFFFu : 0u;
    if (a == 0x80000000u && b == 0xFFFFFFFFu) return 0xFFFFFFFFu;
    return uint32_t(int32_t(a) / int32_t(b));
}
static inline uint32_t rbrew_divwu(uint32_t a, uint32_t b) {
    return b ? a / b : 0u;
}
static inline uint32_t rbrew_abs32(uint32_t v) {
    return (uint32_t)abs((int32_t)v);
}
static inline uint32_t rbrew_rotl32(uint32_t v, uint32_t n) {
    n &= 31u;
    return (v << n) | (v >> ((32u - n) & 31u));
}
static inline uint32_t rbrew_rotr32(uint32_t v, uint32_t n) {
    n &= 31u;
    return (v >> n) | (v << ((32u - n) & 31u));
}
static inline uint32_t rbrew_clz32(uint32_t v) {
    if (v == 0) return 32;
#if defined(__GNUC__) || defined(__clang__)
    return (uint32_t)__builtin_clz(v);
#else
    uint32_t n = 0;
    while (!(v & 0x80000000u)) { ++n; v <<= 1; }
    return n;
#endif
}
static inline uint32_t rbrew_clo32(uint32_t v) {
    return rbrew_clz32(~v);
}
static inline uint32_t rbrew_popcount32(uint32_t v) {
#if defined(__GNUC__) || defined(__clang__)
    return (uint32_t)__builtin_popcount(v);
#else
    v = v - ((v >> 1) & 0x55555555u);
    v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u);
    return (((v + (v >> 4)) & 0x0F0F0F0Fu) * 0x01010101u) >> 24;
#endif
}

// rlwinm: rotate left, then mask bits [mb..me] (PowerPC semantics)
static inline uint32_t rbrew_rlwinm(uint32_t v, uint32_t sh, uint32_t mb, uint32_t me) {
    uint32_t r = rbrew_rotl32(v, sh);
    // PPC bit zero is the MSB. A wrapping interval selects both ends of
    // the word; adjacent MB/ME boundaries select all 32 bits.
    uint32_t from_begin = 0xFFFFFFFFu >> mb;
    uint32_t through_end = 0xFFFFFFFFu << (31u - me);
    uint32_t mask = mb <= me ? from_begin & through_end : from_begin | through_end;
    return r & mask;
}

// ---------------------------------------------------------------------------
// Comparison helpers
// Returns a 4-bit CR field value: bit3=LT, bit2=GT, bit1=EQ, bit0=SO
// Stored in cpu->cr[n] as a uint32_t with those bits.
// ---------------------------------------------------------------------------
static inline uint32_t rbrew_cmp_s32(uint32_t a, uint32_t b) {
    int32_t sa = (int32_t)a, sb = (int32_t)b;
    if (sa < sb) return 8u; // LT
    if (sa > sb) return 4u; // GT
    return 2u;              // EQ
}
static inline uint32_t rbrew_cmp_u32(uint32_t a, uint32_t b) {
    if (a < b) return 8u;
    if (a > b) return 4u;
    return 2u;
}
static inline uint32_t rbrew_cmp_f64(double a, double b) {
    if (a < b) return 8u;
    if (a > b) return 4u;
    if (a == b) return 2u;
    return 1u; // unordered (NaN)
}

// ---------------------------------------------------------------------------
// FP helper
// ---------------------------------------------------------------------------
static inline double rbrew_fround(double v) {
    return (double)(float)v;
}

// ---------------------------------------------------------------------------
// Dispatch — called for indirect jumps/calls (bctr/bctrl)
// Looks up the host implementation for the guest address in ctr.
// ---------------------------------------------------------------------------
#ifdef __cplusplus
extern "C" {
#endif

RBREW_HOST_API void rbrew_dispatch(CPUState* cpu, uint32_t guest_addr) noexcept(false);
// Only use at proven linked-call/entry boundaries with an explicit LR return.
// Legacy local/tail dispatch retains its existing behavior.
RBREW_HOST_API void rbrew_dispatch_linked(CPUState* cpu, uint32_t guest_addr) noexcept(false);
// Invoke the module's declared entry with loader arguments and return its result.
int32_t rbrew_run_entry(CPUState* cpu, uint32_t entry, uint32_t argc, uint32_t argv);
// GHS-style null-terminated constructor table; mark before calling so that
// reentrant initialization and a later guest startup cannot run it twice.
// The caller supplies the program's table and completion-flag addresses.
uint32_t rbrew_run_constructors(CPUState* cpu, uint32_t table, uint32_t done_flag);
RBREW_HOST_API void rbrew_call_indirect(CPUState* cpu, uint32_t guest_addr) noexcept(false);
RBREW_HOST_API void rbrew_call(CPUState* cpu, uint32_t guest_addr) noexcept(false);

// Called by OS stubs to register a host function at a guest address
RBREW_HOST_API void rbrew_register_func(CPUState* cpu, uint32_t guest_addr, HostFunc fn);

#ifdef __cplusplus
}
#endif

#include "rebrewu_coverage.h"
