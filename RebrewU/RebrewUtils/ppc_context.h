#pragma once

#include <cstdint>
#include <cstring>
#include <immintrin.h>
#include <setjmp.h>

// PowerPC Register Union for WiiU
union PPCRegister {
    struct {
        uint32_t _pad;
        uint32_t u32;
    };
    struct {
        int32_t _pad2;
        int32_t s32;
    };
    struct {
        uint16_t _pad3[2];
        uint16_t u16;
    };
    struct {
        int16_t _pad4[2];
        int16_t s16;
    };
    struct {
        uint8_t _pad5[3];
        uint8_t u8;
    };
    struct {
        int8_t _pad6[3];
        int8_t s8;
    };
    uint64_t u64;
    int64_t s64;
    double f64;
    float f32;

    PPCRegister() : u64(0) {}
    PPCRegister(uint64_t value) : u64(value) {}

    template<typename T>
    void compare(T lhs, T rhs, const struct PPCXERRegister& xer);
};

// Floating point register (paired singles for WiiU)
union PPCFPRegister {
    struct {
        float ps0;  // Primary single
        float ps1;  // Secondary single (WiiU specific)
    };
    double f64;
    uint64_t u64;
    uint32_t u32[2];

    PPCFPRegister() : u64(0) {}
    PPCFPRegister(double value) : f64(value) {}
    PPCFPRegister(float ps0, float ps1) : ps0(ps0), ps1(ps1) {}
};

// Condition Register
struct PPCCRRegister {
    bool lt = false;
    bool gt = false;
    bool eq = false;
    bool so = false;

    void compare(float lhs, float rhs) {
        if (std::isnan(lhs) || std::isnan(rhs)) {
            lt = gt = eq = false;
            so = true;
        } else {
            lt = lhs < rhs;
            gt = lhs > rhs;
            eq = lhs == rhs;
            so = false;
        }
    }

    template<typename T>
    void compare(T lhs, T rhs) {
        lt = lhs < rhs;
        gt = lhs > rhs;
        eq = lhs == rhs;
        so = false;
    }

    uint32_t GetCRField() const {
        return (lt ? 8 : 0) | (gt ? 4 : 0) | (eq ? 2 : 0) | (so ? 1 : 0);
    }

    void SetCRField(uint32_t value) {
        lt = (value & 8) != 0;
        gt = (value & 4) != 0;
        eq = (value & 2) != 0;
        so = (value & 1) != 0;
    }
};

// XER Register
struct PPCXERRegister {
    bool so = false;  // Summary Overflow
    bool ov = false;  // Overflow
    bool ca = false;  // Carry
    uint8_t count = 0; // String/Load-Store count

    uint32_t Get() const {
        return (so ? 0x80000000 : 0) | (ov ? 0x40000000 : 0) | (ca ? 0x20000000 : 0) | count;
    }

    void Set(uint32_t value) {
        so = (value & 0x80000000) != 0;
        ov = (value & 0x40000000) != 0;
        ca = (value & 0x20000000) != 0;
        count = value & 0x7F;
    }
};

// FPSCR Register
struct PPCFPSCRRegister {
    union {
        struct {
            uint32_t fx : 1;    // Floating-point exception summary
            uint32_t fex : 1;   // Floating-point enabled exception summary
            uint32_t vx : 1;    // Floating-point invalid operation exception summary
            uint32_t ox : 1;    // Floating-point overflow exception
            uint32_t ux : 1;    // Floating-point underflow exception
            uint32_t zx : 1;    // Floating-point zero divide exception
            uint32_t xx : 1;    // Floating-point inexact exception
            uint32_t vxsnan : 1; // Invalid operation exception for SNaN
            uint32_t vxisi : 1;  // Invalid operation exception for ∞ - ∞
            uint32_t vxidi : 1;  // Invalid operation exception for ∞ / ∞
            uint32_t vxzdz : 1;  // Invalid operation exception for 0 / 0
            uint32_t vximz : 1;  // Invalid operation exception for ∞ * 0
            uint32_t vxvc : 1;   // Invalid operation exception for invalid compare
            uint32_t fr : 1;     // Fraction rounded
            uint32_t fi : 1;     // Fraction inexact
            uint32_t fprf : 5;   // Floating-point result flags
            uint32_t reserved1 : 1;
            uint32_t vxsoft : 1; // Invalid operation exception for software request
            uint32_t vxsqrt : 1; // Invalid operation exception for invalid square root
            uint32_t vxcvi : 1;  // Invalid operation exception for invalid integer convert
            uint32_t ve : 1;     // Invalid operation exception enable
            uint32_t oe : 1;     // Overflow exception enable
            uint32_t ue : 1;     // Underflow exception enable
            uint32_t ze : 1;     // Zero divide exception enable
            uint32_t xe : 1;     // Inexact exception enable
            uint32_t ni : 1;     // Non-IEEE mode
            uint32_t rn : 2;     // Rounding control
        };
        uint32_t raw;
    };

    PPCFPSCRRegister() : raw(0) {}

    void enableFlushMode() {
        // Enable denormal flushing for paired singles
    }

    void disableFlushMode() {
        // Disable denormal flushing for FPU operations
    }

    void enableFlushModeUnconditional() {
        enableFlushMode();
    }

    void disableFlushModeUnconditional() {
        disableFlushMode();
    }

    uint32_t loadFromHost() const {
        return raw;
    }

    void storeFromGuest(uint32_t value) {
        raw = value;
    }
};

// MSR Register
struct PPCMSRRegister {
    union {
        struct {
            uint32_t le : 1;    // Little-endian mode
            uint32_t ri : 1;    // Recoverable interrupt
            uint32_t reserved1 : 2;
            uint32_t dr : 1;    // Data address translation
            uint32_t ir : 1;    // Instruction address translation
            uint32_t ip : 1;    // Interrupt prefix
            uint32_t reserved2 : 1;
            uint32_t fe1 : 1;   // Floating-point exception mode 1
            uint32_t be : 1;    // Branch trace enable
            uint32_t se : 1;    // Single-step trace enable
            uint32_t fe0 : 1;   // Floating-point exception mode 0
            uint32_t me : 1;    // Machine check enable
            uint32_t fp : 1;    // Floating-point available
            uint32_t pr : 1;    // Privilege level
            uint32_t ee : 1;    // External interrupt enable
            uint32_t ile : 1;   // Interrupt little-endian mode
            uint32_t reserved3 : 1;
            uint32_t pow : 1;   // Power management enable
            uint32_t reserved4 : 13;
        };
        uint32_t raw;
    };

    PPCMSRRegister() : raw(0x8000) {} // Default state
};

// Main PowerPC Context for WiiU
struct PPCContext {
    // General Purpose Registers
    PPCRegister r[32];

    // Floating Point Registers (with paired singles support)
    PPCFPRegister f[32];

    // Condition Registers
    PPCCRRegister cr[8];

    // Special Purpose Registers
    PPCRegister lr;     // Link Register
    PPCRegister ctr;    // Count Register
    PPCXERRegister xer; // XER Register
    PPCFPSCRRegister fpscr; // FPSCR Register
    PPCMSRRegister msr; // MSR Register

    // Special registers for WiiU
    PPCRegister gqr[8]; // Graphics Quantization Registers (for paired singles)
    PPCRegister hid[5]; // Hardware Implementation Dependent registers

    // Reserved register for atomic operations
    PPCRegister reserved;

    // Exception handling context
    jmp_buf exception_jmpbuf;
    bool exception_pending = false;

    PPCContext() {
        // Initialize registers to zero
        std::memset(r, 0, sizeof(r));
        std::memset(f, 0, sizeof(f));
        std::memset(cr, 0, sizeof(cr));
        std::memset(gqr, 0, sizeof(gqr));
        std::memset(hid, 0, sizeof(hid));
        
        // Set default values for some registers
        msr.raw = 0x8000; // Default MSR state
        
        // Initialize GQRs for paired single operations
        for (int i = 0; i < 8; i++) {
            gqr[i].u32 = 0x40004; // Default quantization settings
        }
    }
};

// Template implementations
template<typename T>
void PPCRegister::compare(T lhs, T rhs, const PPCXERRegister& xer) {
    // This would be implemented in the source file
}

// Function pointer type for PPC functions
typedef void (*PPCFunc)(PPCContext& ctx, uint8_t* base);

// Function mapping structure
struct PPCFuncMapping {
    uint32_t address;
    PPCFunc func;
};

// Macros for function declarations
#ifdef REBREW_RECOMP_USE_ALIAS
#define PPC_FUNC(name) __attribute__((alias("__imp__" #name))) void name(PPCContext& ctx, uint8_t* base)
#define PPC_WEAK_FUNC(name) __attribute__((weak)) void name(PPCContext& ctx, uint8_t* base)
#define PPC_FUNC_IMPL(name) void name(PPCContext& ctx, uint8_t* base)
#else
#define PPC_FUNC(name) void name(PPCContext& ctx, uint8_t* base)
#define PPC_WEAK_FUNC(name) __attribute__((weak)) void name(PPCContext& ctx, uint8_t* base)
#define PPC_FUNC_IMPL(name) void __imp__##name(PPCContext& ctx, uint8_t* base)
#endif

#define PPC_EXTERN_FUNC(name) extern void name(PPCContext& ctx, uint8_t* base)

// Memory access macros (accounting for WiiU memory layout)
#define PPC_LOAD_U8(addr)   (*(volatile uint8_t*)(base + (addr)))
#define PPC_LOAD_U16(addr)  __builtin_bswap16(*(volatile uint16_t*)(base + (addr)))
#define PPC_LOAD_U32(addr)  __builtin_bswap32(*(volatile uint32_t*)(base + (addr)))
#define PPC_LOAD_U64(addr)  __builtin_bswap64(*(volatile uint64_t*)(base + (addr)))

#define PPC_STORE_U8(addr, val)   (*(volatile uint8_t*)(base + (addr)) = (val))
#define PPC_STORE_U16(addr, val)  (*(volatile uint16_t*)(base + (addr)) = __builtin_bswap16(val))
#define PPC_STORE_U32(addr, val)  (*(volatile uint32_t*)(base + (addr)) = __builtin_bswap32(val))
#define PPC_STORE_U64(addr, val)  (*(volatile uint64_t*)(base + (addr)) = __builtin_bswap64(val))

// Memory-mapped I/O macros (for hardware registers)
#define PPC_MM_STORE_U8(addr, val)   PPC_STORE_U8(addr, val)
#define PPC_MM_STORE_U16(addr, val)  PPC_STORE_U16(addr, val)
#define PPC_MM_STORE_U32(addr, val)  PPC_STORE_U32(addr, val)
#define PPC_MM_STORE_U64(addr, val)  PPC_STORE_U64(addr, val)

// Function call macros
#define PPC_CALL_INDIRECT_FUNC(addr) \
    do { \
        extern PPCFuncMapping PPCFuncMappings[]; \
        for (auto* mapping = PPCFuncMappings; mapping->func; mapping++) { \
            if (mapping->address == (addr)) { \
                mapping->func(ctx, base); \
                return; \
            } \
        } \
        __builtin_debugtrap(); \
    } while(0)

// Prologue macro for function setup
#define PPC_FUNC_PROLOGUE() \
    do { \
        (void)ctx; (void)base; \
    } while(0)

// Instruction implementation helpers
inline __m128 _mm_cvtepu32_ps_(__m128i a) {
    return _mm_cvtepi32_ps(a);
}

// Paired single specific helpers
inline void ps_merge00(__m128& dst, const __m128& a, const __m128& b) {
    dst = _mm_shuffle_ps(a, b, _MM_SHUFFLE(0, 0, 0, 0));
}

inline void ps_merge01(__m128& dst, const __m128& a, const __m128& b) {
    dst = _mm_shuffle_ps(a, b, _MM_SHUFFLE(1, 0, 0, 0));
}

inline void ps_merge10(__m128& dst, const __m128& a, const __m128& b) {
    dst = _mm_shuffle_ps(a, b, _MM_SHUFFLE(0, 1, 0, 0));
}

inline void ps_merge11(__m128& dst, const __m128& a, const __m128& b) {
    dst = _mm_shuffle_ps(a, b, _MM_SHUFFLE(1, 1, 0, 0));
}