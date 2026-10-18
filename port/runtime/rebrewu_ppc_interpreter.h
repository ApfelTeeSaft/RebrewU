#pragma once

#include "rebrewu_runtime.h"

// This is a deliberately partial, independently decoded PPC32 reference core.
// Unsupported encodings and memory faults leave all architectural state intact.
// The caller owns execution bounds, native/HLE dispatch and exception policy.
enum class RbrewPPCStop {
    Continue,
    Unsupported,
    InvalidPC,
    MemoryFault,
};

enum class RbrewPPCTransfer {
    Sequential,
    Branch,
    Call,
};

struct RbrewPPCStep {
    RbrewPPCStop stop;
    RbrewPPCTransfer transfer;
    uint32_t next_pc;
    uint32_t word;
    const char* detail;
};

// Fetches one original big-endian instruction from the same arena as generated
// code. Does not change CPUState.pc: next_pc explicitly belongs to the caller.
// Data access requires natural alignment; fault delivery/MMU are not modeled.
RbrewPPCStep rbrew_ppc_step(CPUState* cpu, uint32_t pc);
