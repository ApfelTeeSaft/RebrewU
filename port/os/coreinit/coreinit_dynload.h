#pragma once

#include "rebrewu_runtime.h"
#include <cstdint>
#include <span>
#include <string_view>

// Native-backed modules only. This is an exact export registry, not an RPL
// parser, an executable loader, or a fallback for unknown export names.
enum class RbrewDynLoadExportKind : uint32_t { Function = 0, Data = 1 };

struct RbrewDynLoadExport {
    std::string_view name;
    RbrewDynLoadExportKind kind;
    uint32_t guest_address;
};

namespace rbrew_dynload_error {
inline constexpr uint32_t ok = 0;
inline constexpr uint32_t invalid_handle = 0xBAD10003u;
inline constexpr uint32_t invalid_module_name_pointer = 0xBAD1000Fu;
inline constexpr uint32_t invalid_module_name = 0xBAD10010u;
inline constexpr uint32_t invalid_acquire_pointer = 0xBAD10011u;
inline constexpr uint32_t module_has_no_data_exports = 0xBAD1001Bu;
inline constexpr uint32_t module_has_no_code_exports = 0xBAD1001Cu;
inline constexpr uint32_t export_not_found = 0xBAD1001Du;
inline constexpr uint32_t invalid_parameter = 0xBAD1001Eu;
// WUT coreinit/dynload.h; Decaf's named ModuleNotFound is a different status.
inline constexpr uint32_t module_not_found = 0xFFFFFFFAu;
}

// Declares a host-provided module for this guest arena, copying all names.
// Function addresses must already have an actual HostFunc in cpu's dispatcher;
// data addresses must name an existing guest byte. No addresses are invented.
// Returns the opaque handle reserved for the first Acquire. A declaration is
// not a guest reference: FindExport requires at least one successful Acquire.
// Duplicate identical declarations are idempotent; conflicting ones throw.
uint32_t rbrew_dynload_register_module(CPUState* cpu, std::string_view module_name,
    std::span<const RbrewDynLoadExport> exports);

// Cafe ABI: Acquire(r3=name, r4=out handle); FindExport(r3=handle, r4=kind,
// r5=exact export name, r6=out guest address). Status is returned in r3.
// FindExport preserves output on every failure. A missing valid module clears
// Acquire's output, matching the observed loader failure-publication contract.
void rbrew_dynload_acquire(CPUState* cpu);
void rbrew_dynload_find_export(CPUState* cpu);

// Release is void in Cafe's ABI: it reads r3 and preserves architectural
// registers. The last release retires the guest handle; later Acquire receives
// a fresh handle for the same native declaration. There are no RPL entrypoints
// or allocator callbacks in this registry-only layer.
void rbrew_dynload_release(CPUState* cpu);
uint32_t rbrew_dynload_reference_count(const CPUState* cpu, uint32_t handle);

// Arena owners call this before destroying/reusing the guest memory allocation.
// Worker CPUStates sharing that arena must not independently tear it down.
void rbrew_dynload_forget_arena(const CPUState* cpu);
