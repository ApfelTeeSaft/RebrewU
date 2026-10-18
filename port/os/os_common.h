#pragma once
#include "../runtime/rbrew_diagnostics.h"
// Shared utilities for OS stub implementations
#include "rebrewu_runtime.h"
#include <stdio.h>
#include <stdint.h>
#include <atomic>
#include <chrono>

// Read/write convenience aliases using cpu->mem
#define MEM    (cpu->mem)
#define R(n)   (cpu->r[n])
#define F(n)   (cpu->f[n])
#define CR(n)  (cpu->cr[n])
#define LR     (cpu->lr)
#define CTR    (cpu->ctr)

// Wii U calling convention: args in r3-r10, return in r3 (r4 for 64-bit)
#define ARG0   (cpu->r[3])
#define ARG1   (cpu->r[4])
#define ARG2   (cpu->r[5])
#define ARG3   (cpu->r[6])
#define ARG4   (cpu->r[7])
#define ARG5   (cpu->r[8])
#define ARG6   (cpu->r[9])
#define ARG7   (cpu->r[10])
#define RET    (cpu->r[3])
#define RET_HI (cpu->r[3])
#define RET_LO (cpu->r[4])

// Espresso clocks (decaf libcpu/state.h, Cemu ESPRESSO_BUS_CLOCK): the OS
// timer (OSGetTime/OSGetSystemTime/OSGetTick) runs at bus clock / 4.
constexpr uint32_t RBREW_CORE_CLOCK  = 1243125000u;
constexpr uint32_t RBREW_BUS_CLOCK   = 248625000u;
constexpr uint32_t RBREW_TIMER_CLOCK = RBREW_BUS_CLOCK / 4u; // 62,156,250 Hz

// OSGetSystemTime: timer ticks since boot. One clock for every library so
// coreinit times and GX2 flip/vsync stamps are comparable.
inline const auto rbrew_boot_time = std::chrono::steady_clock::now();
inline uint64_t rbrew_system_ticks() {
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - rbrew_boot_time).count();
    return uint64_t(us) * RBREW_TIMER_CLOCK / 1000000ULL;
}

// Core the calling guest thread runs on (OSGetCoreId). Guest threads point
// this at their own slot (coreinit_threads: chosen from the affinity bits);
// the main thread runs on core 1 (decaf: main thread affinity CPU1).
inline std::atomic<uint32_t> rbrew_main_thread_core{1};
inline thread_local std::atomic<uint32_t>* rbrew_thread_core_slot = nullptr;
// Identity for a native library-owned thread entering guest callbacks.
inline thread_local uint32_t rbrew_system_thread = 0;
inline uint32_t rbrew_current_core() {
    return rbrew_thread_core_slot ? rbrew_thread_core_slot->load() : rbrew_main_thread_core.load();
}

// Log unimplemented stubs without crashing (use inside a stub function body)
#define STUB_LOG(name) \
    do { RBREW_LOGF(stderr, "[stub] %s called (r3=0x%08X)\n", #name, cpu->r[3]); } while(0)

// Null stub function — used as the HostFunc passed to rbrew_register_func
// for any import that has no host implementation yet.
static inline void STUB(CPUState* cpu) { (void)cpu; }

// Register a host implementation at a guest PLT stub address.
// Call once during module_register().
void os_register(CPUState* cpu, uint32_t addr, HostFunc fn);

// Each OS module exposes this to register its host functions.
typedef void (*OsRegisterFn)(CPUState* cpu);
