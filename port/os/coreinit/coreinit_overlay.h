#pragma once

#include "rebrewu_runtime.h"

// Backend contract: true means the entire fixed overlay aperture is now
// accessible (enable) or inaccessible (disable). False/exception must leave
// the prior mapping intact. The callback must not reenter overlay APIs.
// This maps a Cafe virtual aperture; it does not resize the application heap.
using RbrewOverlayMapper = bool (*)(void* user, uint32_t base, uint32_t size, bool enable);

struct RbrewOverlaySnapshot {
    bool enabled{};
    uint32_t base{};
    uint32_t size{};
};

// CPUStates sharing cpu->mem share the mapping and its enabled state. Install
// the backend before guest execution. Replacing a live mapping's backend fails.
void rbrew_overlay_set_mapper(CPUState* cpu, RbrewOverlayMapper mapper, void* user);
RbrewOverlaySnapshot rbrew_overlay_snapshot(const CPUState* cpu);

// Genuine imported PLT addresses come from coreinit_addrs.h. Only Enable and
// IsEnabled are imported by the current title; do not invent addresses for the
// reusable Disable/GetRange handlers below.
void coreinit_overlay_register(CPUState* cpu);

// Cafe ABI: query returns Boolean in r3. Enable is void: r3 is an opaque
// argument, r4/r5 are optional BE base/size outputs. GetRange is void with
// optional outputs in r3/r4. Disable is void. Host mapping/argument failures
// throw, preserving guest state/outputs; no guest success result is fabricated.
void rbrew_overlay_is_enabled(CPUState* cpu);
void rbrew_overlay_enable(CPUState* cpu);
void rbrew_overlay_get_range(CPUState* cpu);
void rbrew_overlay_disable(CPUState* cpu);

// Arena-owner teardown only, after workers are quiescent. Forgets metadata;
// owner must release the whole host reservation, or Disable first. Worker
// CPUStates must not independently forget a shared arena.
void rbrew_overlay_forget_arena(const CPUState* cpu);
