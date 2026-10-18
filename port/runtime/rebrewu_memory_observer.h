#pragma once
#include <cstdint>

// Inactive by default. Detached validation may observe shared-writer byte
// ranges on this thread and arena only. Callbacks must not allocate, throw,
// change guest state or recursively invoke a writer. Installing returns the
// previous observer so a scope can restore it even when execution throws.
struct RbrewMemoryWriteObserver {
    uint8_t* mem{};
    void (*callback)(uint32_t address, uint32_t size, void* user) noexcept {};
    void* user{};
};
RbrewMemoryWriteObserver rbrew_set_write_observer(RbrewMemoryWriteObserver observer) noexcept;

// Plain guest stores break other CPUs' reservations only in strict mode (off
// by default; RBREW_STRICT_RESERVATIONS=1). lwarx/stwcx. pairs are exact
// against each other either way.
extern "C" void rbrew_set_strict_reservations(bool enabled);
