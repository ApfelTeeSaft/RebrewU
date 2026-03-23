#pragma once
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
// We map [0x00000000, WIIU_MEM_SIZE) as a flat byte array.
// ---------------------------------------------------------------------------
#define WIIU_MEM_SIZE   (768u * 1024u * 1024u)   // 768 MB: covers code+data (0-0x1FFFFFFF) + heap (0x20000000-0x2FFFFFFF)

// ---------------------------------------------------------------------------
// CPUState — mirrors PowerPC Espresso register file
// ---------------------------------------------------------------------------
struct CPUState {
    uint32_t r[32];     // GPRs
    double   f[32];     // FPRs (double precision on Espresso)
    uint32_t cr[8];     // Condition Register fields CR0–CR7 (4 bits each, packed as uint32)
    uint32_t lr;        // Link Register
    uint32_t ctr;       // Count Register
    uint32_t xer;       // Integer Exception Register

    uint8_t* mem;       // pointer to base of 512MB guest memory block

    // Host function dispatch table: guest address → host implementation
    // Populated at startup for all OS import addresses.
    HostFunc* dispatch_table;  // indexed by guest_addr >> 2
    uint32_t  dispatch_size;   // number of entries

    // Scratch fields used by rbrew_dispatch / rbrew_call_indirect
    uint32_t dispatch_ret_addr;
};

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
    if ((uint64_t)addr + 1 > WIIU_MEM_SIZE) {
        fprintf(stderr, "[rbrew] OOB read8  addr=0x%08X\n", addr); return 0;
    }
    return mem[addr];
}
static inline uint16_t rbrew_read16(uint8_t* mem, uint32_t addr) {
    if ((uint64_t)addr + 2 > WIIU_MEM_SIZE) {
        fprintf(stderr, "[rbrew] OOB read16 addr=0x%08X\n", addr); return 0;
    }
    uint16_t v; memcpy(&v, mem + addr, 2); return rbrew_bswap16(v);
}
static inline uint32_t rbrew_read32(uint8_t* mem, uint32_t addr) {
    if ((uint64_t)addr + 4 > WIIU_MEM_SIZE) {
        fprintf(stderr, "[rbrew] OOB read32 addr=0x%08X\n", addr); return 0;
    }
    uint32_t v; memcpy(&v, mem + addr, 4); return rbrew_bswap32(v);
}
static inline uint64_t rbrew_read64(uint8_t* mem, uint32_t addr) {
    if ((uint64_t)addr + 8 > WIIU_MEM_SIZE) {
        fprintf(stderr, "[rbrew] OOB read64 addr=0x%08X\n", addr); return 0;
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
    if ((uint64_t)addr + 1 > WIIU_MEM_SIZE) {
        fprintf(stderr, "[rbrew] OOB write8  addr=0x%08X val=0x%02X\n", addr, v); return;
    }
    mem[addr] = v;
}
static inline void rbrew_write16(uint8_t* mem, uint32_t addr, uint16_t v) {
    if ((uint64_t)addr + 2 > WIIU_MEM_SIZE) {
        fprintf(stderr, "[rbrew] OOB write16 addr=0x%08X val=0x%04X\n", addr, v); return;
    }
    uint16_t be = rbrew_bswap16(v); memcpy(mem + addr, &be, 2);
}
static inline void rbrew_write32(uint8_t* mem, uint32_t addr, uint32_t v) {
    if ((uint64_t)addr + 4 > WIIU_MEM_SIZE) {
        fprintf(stderr, "[rbrew] OOB write32 addr=0x%08X val=0x%08X\n", addr, v); return;
    }
    uint32_t be = rbrew_bswap32(v); memcpy(mem + addr, &be, 4);
}
static inline void rbrew_write64(uint8_t* mem, uint32_t addr, uint64_t v) {
    if ((uint64_t)addr + 8 > WIIU_MEM_SIZE) {
        fprintf(stderr, "[rbrew] OOB write64 addr=0x%08X\n", addr); return;
    }
    uint64_t be = rbrew_bswap64(v); memcpy(mem + addr, &be, 8);
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
static inline uint32_t rbrew_abs32(uint32_t v) {
    return (uint32_t)abs((int32_t)v);
}
static inline uint32_t rbrew_rotl32(uint32_t v, uint32_t n) {
    n &= 31u;
    return (v << n) | (v >> (32u - n));
}
static inline uint32_t rbrew_rotr32(uint32_t v, uint32_t n) {
    n &= 31u;
    return (v >> n) | (v << (32u - n));
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
    uint32_t mask;
    if (mb <= me)
        mask = ((0xFFFFFFFFu >> mb) & ~(me < 31 ? (0xFFFFFFFFu >> (me + 1)) : 0u));
    else
        mask = ~((me < 31 ? (0xFFFFFFFFu >> (me + 1)) : 0u) & (0xFFFFFFFFu >> mb));
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

void rbrew_dispatch(CPUState* cpu, uint32_t guest_addr);
void rbrew_call_indirect(CPUState* cpu, uint32_t guest_addr);
void rbrew_call(CPUState* cpu, uint32_t guest_addr);

// Called by OS stubs to register a host function at a guest address
void rbrew_register_func(CPUState* cpu, uint32_t guest_addr, HostFunc fn);

#ifdef __cplusplus
}
#endif
