#include "rebrewu_ppc_hybrid.h"

namespace {
struct Budget {
    CPUState* cpu;
    uint64_t remaining;
    uint64_t instructions{};
    uint64_t native_transfers{};
};
thread_local Budget* active_budget = nullptr;
struct BudgetScope {
    Budget own;
    Budget* previous;
    Budget* current;
    BudgetScope(CPUState* cpu, uint64_t maximum)
        : own{cpu, maximum}, previous(active_budget),
          current(previous && previous->cpu == cpu ? previous : &own) {
        active_budget = current;
    }
    ~BudgetScope() { active_budget = previous; }
};
bool executable(const CPUState* cpu, uint32_t pc) {
    if (!pc || (pc & 3u) || uint64_t(pc) + 4 > cpu->guest_memory_size) return false;
    for (uint32_t i = 0; i < cpu->guest_code_range_count; ++i) {
        const auto& range = cpu->guest_code_ranges[i];
        if (range.begin <= pc && uint64_t(pc) + 4 <= range.end) return true;
    }
    return false;
}
HostFunc native_entry(CPUState* cpu, uint32_t pc) {
    if (!pc || (pc & 3u) || !cpu->dispatch_table) return nullptr;
    return (pc >> 2) < cpu->dispatch_size ? cpu->dispatch_table[pc >> 2] : nullptr;
}
}

RbrewHybridResult rbrew_ppc_run(CPUState* cpu, uint32_t entry, uint32_t return_pc,
    uint64_t maximum, bool bridge_native) {
    BudgetScope scope(cpu, maximum);
    Budget& budget = *scope.current;
    const uint64_t initial_steps = budget.instructions;
    const uint64_t initial_native = budget.native_transfers;
    auto result = [&](RbrewHybridStop stop, uint32_t pc, uint32_t word, const char* detail) {
        return RbrewHybridResult{stop, pc, word, budget.instructions - initial_steps,
            budget.native_transfers - initial_native, detail};
    };
    if (!cpu || !cpu->mem || cpu->guest_memory_size > WIIU_MEM_SIZE ||
        (cpu->guest_code_range_count && !cpu->guest_code_ranges))
        return result(RbrewHybridStop::InvalidPC, entry, 0, "invalid guest image metadata");
    // Even an entry equal to the sentinel must be a valid executable target.
    if (!executable(cpu, entry))
        return result(RbrewHybridStop::InvalidPC, entry, 0, "entry outside executable image");
    if (entry == return_pc)
        return result(RbrewHybridStop::InvalidPC, entry, 0, "entry aliases invocation return boundary");
    uint32_t pc = entry;
    for (;;) {
        cpu->pc = pc;
        if (pc == return_pc)
            return result(RbrewHybridStop::Returned, pc, 0, "invocation returned");
        if (!executable(cpu, pc))
            return result(RbrewHybridStop::InvalidPC, pc, 0, "PC outside executable image");
        if (!budget.remaining)
            return result(RbrewHybridStop::BudgetExceeded, pc, 0, "interpreter instruction budget exhausted");
        const auto step = rbrew_ppc_step(cpu, pc);
        if (step.stop != RbrewPPCStop::Continue) {
            RbrewHybridStop stop = step.stop == RbrewPPCStop::Unsupported ? RbrewHybridStop::Unsupported :
                step.stop == RbrewPPCStop::MemoryFault ? RbrewHybridStop::MemoryFault : RbrewHybridStop::InvalidPC;
            return result(stop, pc, step.word, step.detail);
        }
        --budget.remaining;
        ++budget.instructions;
        RBREW_COVERAGE_EVENT('I',entry,pc);
        pc = step.next_pc;
        cpu->pc = pc;
        if (pc == return_pc) continue;
        if (bridge_native && step.transfer != RbrewPPCTransfer::Sequential) {
            if (HostFunc function = native_entry(cpu, pc)) {
                ++budget.native_transfers;
                try {
                    // Dispatch is also the detached differential/HLE side-effect guard.
                    rbrew_dispatch(cpu, pc);
                } catch (const RbrewHybridFailure& failure) {
                    return result(failure.result.stop, failure.result.pc,
                        failure.result.word, failure.result.detail);
                }
                pc = cpu->lr;
            }
        }
    }
}
