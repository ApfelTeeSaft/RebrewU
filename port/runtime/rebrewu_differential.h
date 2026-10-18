#pragma once
#include "rebrewu_ppc_hybrid.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Offline, pure guest-block replay only. The native callback must contain no
// host APIs/global side effects, must terminate, and must access valid guest
// bytes. Dispatch/HLE is rejected in both detached runs. This is not a live
// shadow executor, a second OS instance, or a sandbox for untrusted native code.
struct RbrewDifferentialOptions {
    uint64_t instruction_budget{10000};
    uint32_t max_memory_bytes{64u * 1024u * 1024u};
    std::size_t max_mismatches{32};
    // Optional forensic rule. Equal final flat RAM can be valid even when a
    // compiler omits a redundant store; differing write sets are always listed.
    bool compare_write_sets{false};
};

struct RbrewDifferentialMismatch {
    std::string component;
    uint32_t index{}; // register index, or guest byte address for memory
    uint64_t expected{}; // independent interpreter
    uint64_t actual{};   // generated callback
};

struct RbrewDifferentialRange { uint32_t begin{}, end{}; }; // exclusive end

struct RbrewDifferentialReport {
    bool comparable{};
    bool matches{};
    std::string detail;
    RbrewHybridResult interpreter{};
    uint64_t mismatch_count{};
    bool write_sets_match{};
    uint64_t write_set_mismatch_count{};
    uint64_t touched_memory_bytes{};
    uint64_t compiled_touched_memory_bytes{};
    uint64_t interpreted_touched_memory_bytes{};
    uint64_t changed_memory_bytes{};
    std::vector<RbrewDifferentialMismatch> mismatches;
    // Shared-writer journal, including writes that retain or restore old bytes.
    // Raw host pointer writes bypass this hook; final changed bytes still scan.
    std::vector<RbrewDifferentialRange> touched_memory_ranges;
    // Union of bytes whose final value differs from the shared initial image.
    // Stores restored to their original value are not observed by this scan.
    std::vector<RbrewDifferentialRange> changed_memory_ranges;
};

RbrewDifferentialReport rbrew_ppc_compare(const CPUState& initial,
    HostFunc native_callback, uint32_t entry, uint32_t return_pc,
    const RbrewDifferentialOptions& options = {});
