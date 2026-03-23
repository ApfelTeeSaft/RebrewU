#pragma once

// ============================================================================
// RebrewU — Wii U static recompilation framework
// rebrewu_runtime.h — C runtime shim included by generated recompiled code
//
// Generated .cpp files #include this header.  It provides:
//    Host integer types matching Wii U ABI widths
//    Memory access helpers that translate guest virtual addresses to host
//    pointers (via a flat mapping or a segmented mapping)
//    Helpers for calling back into the host OS / RPL thunks
// ============================================================================

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Guest memory layout
// ============================================================================

/// Base pointer of the flat guest memory mapping on the host.
/// The generated code accesses all guest memory through this pointer.
extern uint8_t* rebrewu_mem_base;

/// Total size of the guest memory mapping (bytes).
extern size_t   rebrewu_mem_size;

// ============================================================================
// Memory access helpers
//
// All guest addresses are 32-bit big-endian values; helpers byte-swap on
// little-endian hosts.
// ============================================================================

static inline uint8_t  mem_read_u8 (uint32_t addr)
    { return rebrewu_mem_base[addr]; }

static inline uint16_t mem_read_u16(uint32_t addr) {
    uint16_t v;
    memcpy(&v, rebrewu_mem_base + addr, 2);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    v = (uint16_t)((v >> 8) | (v << 8));
#endif
    return v;
}

static inline uint32_t mem_read_u32(uint32_t addr) {
    uint32_t v;
    memcpy(&v, rebrewu_mem_base + addr, 4);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    v = ((v & 0xFF000000u) >> 24) | ((v & 0x00FF0000u) >> 8)
      | ((v & 0x0000FF00u) <<  8) | ((v & 0x000000FFu) << 24);
#endif
    return v;
}

static inline float mem_read_f32(uint32_t addr) {
    uint32_t raw = mem_read_u32(addr);
    float f; memcpy(&f, &raw, 4); return f;
}

static inline double mem_read_f64(uint32_t addr) {
    uint32_t hi = mem_read_u32(addr);
    uint32_t lo = mem_read_u32(addr + 4);
    uint64_t raw = ((uint64_t)hi << 32) | lo;
    double d; memcpy(&d, &raw, 8); return d;
}

static inline void mem_write_u8 (uint32_t addr, uint8_t v)
    { rebrewu_mem_base[addr] = v; }

static inline void mem_write_u16(uint32_t addr, uint16_t v) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    v = (uint16_t)((v >> 8) | (v << 8));
#endif
    memcpy(rebrewu_mem_base + addr, &v, 2);
}

static inline void mem_write_u32(uint32_t addr, uint32_t v) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    v = ((v & 0xFF000000u) >> 24) | ((v & 0x00FF0000u) >> 8)
      | ((v & 0x0000FF00u) <<  8) | ((v & 0x000000FFu) << 24);
#endif
    memcpy(rebrewu_mem_base + addr, &v, 4);
}

static inline void mem_write_f32(uint32_t addr, float f) {
    uint32_t raw; memcpy(&raw, &f, 4);
    mem_write_u32(addr, raw);
}

// ============================================================================
// CPU state
//
// Generated functions receive/return guest state via a compact struct so
// they can be called without a trampoline for simple cases.
// ============================================================================

typedef struct {
    uint32_t gpr[32];   ///< General purpose registers r0-r31
    double   fpr[32];   ///< Floating-point registers f0-f31
    uint32_t cr;        ///< Condition register (8 × 4-bit fields)
    uint32_t lr;        ///< Link register
    uint32_t ctr;       ///< Count register
    uint32_t xer;       ///< XER (fixed-point exception register)
    uint32_t pc;        ///< Program counter (current guest PC)
} GuestCPU;

// ============================================================================
// RPL import thunk table
//
// When the recompiler encounters an import stub it emits a call to
// rebrewu_call_import(), which dispatches to a registered host function.
// ============================================================================

typedef void (*RebrewuThunkFn)(GuestCPU* cpu);

void rebrewu_register_import(const char* module, const char* symbol,
                              RebrewuThunkFn fn);

void rebrewu_call_import(const char* module, const char* symbol,
                          GuestCPU* cpu);

// ============================================================================
// Initialisation / teardown
// ============================================================================

/// Initialise the runtime with a pre-allocated guest memory region.
void rebrewu_init(void* mem, size_t mem_size);

/// Tear down the runtime and free resources.
void rebrewu_shutdown(void);

#ifdef __cplusplus
} // extern "C"
#endif