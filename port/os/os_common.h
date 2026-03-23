#pragma once
// Shared utilities for OS stub implementations
#include "rebrewu_runtime.h"
#include <stdio.h>
#include <stdint.h>

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

// Log unimplemented stubs without crashing (use inside a stub function body)
#define STUB_LOG(name) \
    do { fprintf(stderr, "[stub] %s called (r3=0x%08X)\n", #name, cpu->r[3]); } while(0)

// Null stub function — used as the HostFunc passed to rbrew_register_func
// for any import that has no host implementation yet.
static inline void STUB(CPUState* cpu) { (void)cpu; }

// Register a host implementation at a guest PLT stub address.
// Call once during module_register().
void os_register(CPUState* cpu, uint32_t addr, HostFunc fn);

// Each OS module exposes this to register its host functions.
typedef void (*OsRegisterFn)(CPUState* cpu);
