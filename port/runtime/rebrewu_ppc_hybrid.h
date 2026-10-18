#pragma once
#include "rebrewu_ppc_interpreter.h"
#include <exception>

enum class RbrewHybridStop { Returned, Unsupported, InvalidPC, MemoryFault, BudgetExceeded };
struct RbrewHybridResult {
    RbrewHybridStop stop;
    uint32_t pc, word;
    uint64_t instructions, native_transfers;
    const char* detail;
};

// return_pc is an explicit invocation boundary, not an inferred function end.
// Native bridging occurs only on explicit taken transfers, never fallthrough.
// The budget covers interpreter instructions across nested fallback calls;
// existing whole-function native code/HLE cannot yet be preempted here.
RbrewHybridResult rbrew_ppc_run(CPUState* cpu, uint32_t entry, uint32_t return_pc,
    uint64_t budget, bool bridge_native = true);

class RbrewHybridFailure : public std::exception {
public:
    explicit RbrewHybridFailure(RbrewHybridResult value) : result(value) {}
    const char* what() const noexcept override { return result.detail; }
    RbrewHybridResult result;
};
