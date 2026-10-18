#include "rebrewu_differential.h"
#include "rebrewu_memory_observer.h"
#include <bit>
#include <exception>

namespace {
struct WriteJournal {
    uint32_t size;
    std::vector<uint64_t> bits;
    bool outside_arena{};
    explicit WriteJournal(uint32_t count) : size(count), bits((std::size_t(count) + 63u) / 64u) {}
    static void observe(uint32_t address, uint32_t count, void* user) noexcept {
        auto& journal = *static_cast<WriteJournal*>(user);
        if (uint64_t(address) + count > journal.size) {
            journal.outside_arena = true;
            return;
        }
        for (uint32_t offset = 0; offset < count; ++offset) {
            const uint32_t byte = address + offset;
            journal.bits[byte >> 6] |= uint64_t(1) << (byte & 63u);
        }
    }
    bool contains(uint32_t address) const noexcept {
        return (bits[address >> 6] >> (address & 63u)) & 1u;
    }
};
struct WriteObserverScope {
    RbrewMemoryWriteObserver previous;
    WriteObserverScope(uint8_t* mem, WriteJournal& journal) noexcept
        : previous(rbrew_set_write_observer({mem, WriteJournal::observe, &journal})) {}
    ~WriteObserverScope() { rbrew_set_write_observer(previous); }
    WriteObserverScope(const WriteObserverScope&) = delete;
    WriteObserverScope& operator=(const WriteObserverScope&) = delete;
};
}

RbrewDifferentialReport rbrew_ppc_compare(const CPUState& initial,
    HostFunc native_callback, uint32_t entry, uint32_t return_pc,
    const RbrewDifferentialOptions& options) {
    RbrewDifferentialReport report;
    if (!initial.mem || !native_callback || !initial.guest_memory_size ||
        initial.guest_memory_size > options.max_memory_bytes) {
        report.detail = "invalid replay input or guest image exceeds explicit snapshot limit";
        return report;
    }
    if (initial.reservation_valid) {
        report.detail = "active reservation replay is outside the prototype instruction subset";
        return report;
    }
    try {
        const std::vector<uint8_t> baseline(initial.mem, initial.mem + initial.guest_memory_size);
        std::vector<uint8_t> compiled_memory = baseline, interpreted_memory = baseline;
        CPUState compiled = initial, interpreted = initial;
        auto detach = [](CPUState& cpu, uint8_t* bytes) {
            cpu.mem = bytes;
            cpu.dispatch_table = nullptr;
            cpu.dispatch_size = 0;
            cpu.ppc_fallback_enabled = false;
            cpu.differential_validation = true;
            cpu.reservation_mem = nullptr;
            cpu.reservation_valid = false;
        };
        detach(compiled, compiled_memory.data());
        detach(interpreted, interpreted_memory.data());
        WriteJournal compiled_writes(initial.guest_memory_size), interpreted_writes(initial.guest_memory_size);
        // The selected entry is invoked directly; all nested runtime dispatch
        // must fail closed, before any HLE or shared host state can be touched.
        {
            WriteObserverScope native_observer(compiled.mem, compiled_writes);
            native_callback(&compiled);
        }
        {
            WriteObserverScope reference_observer(interpreted.mem, interpreted_writes);
            report.interpreter = rbrew_ppc_run(&interpreted, entry, return_pc,
                options.instruction_budget, false);
        }
        if (report.interpreter.stop != RbrewHybridStop::Returned) {
            report.detail = "reference run stopped before the supplied return boundary";
            return report;
        }
        if (compiled_writes.outside_arena || interpreted_writes.outside_arena) {
            report.detail = "write journal observed a store outside the detached arena";
            return report;
        }
        report.comparable = true;
        auto compare = [&](const char* component, uint32_t index,
                           uint64_t expected, uint64_t actual) {
            if (expected == actual) return;
            ++report.mismatch_count;
            if (report.mismatches.size() < options.max_mismatches)
                report.mismatches.push_back({component, index, expected, actual});
        };
        for (unsigned i = 0; i < 32; ++i) {
            compare("GPR", i, interpreted.r[i], compiled.r[i]);
            compare("FPR bits", i, std::bit_cast<uint64_t>(interpreted.f[i]),
                std::bit_cast<uint64_t>(compiled.f[i]));
        }
        for (unsigned i = 0; i < 8; ++i) compare("CR", i, interpreted.cr[i], compiled.cr[i]);
        compare("LR", 0, interpreted.lr, compiled.lr);
        compare("CTR", 0, interpreted.ctr, compiled.ctr);
        compare("XER", 0, interpreted.xer, compiled.xer);
        compare("FPSCR", 0, interpreted.fpscr, compiled.fpscr);
        bool in_range = false, in_touched_range = false;
        for (uint32_t i = 0; i < initial.guest_memory_size; ++i) {
            const bool interpreted_touched = interpreted_writes.contains(i);
            const bool compiled_touched = compiled_writes.contains(i);
            report.interpreted_touched_memory_bytes += unsigned(interpreted_touched);
            report.compiled_touched_memory_bytes += unsigned(compiled_touched);
            if (interpreted_touched != compiled_touched) {
                ++report.write_set_mismatch_count;
                if (options.compare_write_sets)
                    compare("write set", i, unsigned(interpreted_touched), unsigned(compiled_touched));
            }
            const bool touched = interpreted_touched || compiled_touched;
            if (touched) {
                ++report.touched_memory_bytes;
                if (!in_touched_range) {
                    report.touched_memory_ranges.push_back({i, i + 1u});
                    in_touched_range = true;
                } else report.touched_memory_ranges.back().end = i + 1u;
            } else in_touched_range = false;
            const bool changed = interpreted_memory[i] != baseline[i] || compiled_memory[i] != baseline[i];
            if (changed) {
                ++report.changed_memory_bytes;
                if (!in_range) {
                    report.changed_memory_ranges.push_back({i, i + 1u});
                    in_range = true;
                } else report.changed_memory_ranges.back().end = i + 1u;
            } else in_range = false;
            if (touched || changed) compare("memory", i, interpreted_memory[i], compiled_memory[i]);
        }
        report.write_sets_match = report.write_set_mismatch_count == 0;
        report.matches = report.mismatch_count == 0;
        report.detail = report.matches ? "architectural state and touched/changed memory agree" :
            "architectural state or touched/changed memory differs";
        if (!report.write_sets_match && !options.compare_write_sets)
            report.detail += "; write sets differ (excluded from final-state match)";
    } catch (const std::exception& error) {
        report.comparable = report.matches = false;
        report.detail = std::string("isolated replay rejected: ") + error.what();
    } catch (...) {
        report.comparable = report.matches = false;
        report.detail = "isolated replay rejected by unknown native exception";
    }
    return report;
}
