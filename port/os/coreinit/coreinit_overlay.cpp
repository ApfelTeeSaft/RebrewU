#include "coreinit_overlay.h"
#include "coreinit_addrs.h"

#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace {
struct OverlayState {
    RbrewOverlayMapper mapper{};
    void* user{};
    bool enabled{};
};

struct StateOwner {
    std::mutex mutex;
    std::unordered_map<uint8_t*,OverlayState> states;
};
StateOwner& state_owner() {
    // Workers can still use mappings during launch-thread/process exit.
    // Arena owners erase records explicitly after their workers quiesce.
    static auto* owner = new StateOwner;
    return *owner;
}

void require_arena(const CPUState* cpu) {
    if (!cpu || !cpu->mem || !cpu->guest_memory_size ||
        cpu->guest_memory_size>WIIU_MEM_SIZE)
        throw std::runtime_error("OverlayArena requires a valid guest arena");
}

// The native runtime currently commits its initial low arena and an optional
// overlay aperture, not the reserved hole between them. CPU-local bounds also
// keep the same API safe in small authored fixtures. Enable may write outputs
// in the overlay only after its successful backend mapping.
bool valid_output(const CPUState* cpu,uint32_t address,bool overlay_mapped) {
    if (!address) return true; // GetRange's outputs are optional in Cafe.
    const uint64_t end=uint64_t(address)+4;
    if ((address&3u) || end>cpu->guest_memory_size) return false;
    constexpr uint64_t initial_end=uint64_t(RBREW_APPLICATION_HEAP_BASE)+RBREW_APPLICATION_HEAP_SIZE;
    return end<=initial_end || (overlay_mapped && address>=RBREW_OVERLAY_BASE &&
        end<=uint64_t(RBREW_OVERLAY_BASE)+RBREW_OVERLAY_SIZE);
}

void require_outputs(CPUState* cpu,uint32_t base_out,uint32_t size_out,bool overlay_mapped) {
    if (!valid_output(cpu,base_out,overlay_mapped) ||
        !valid_output(cpu,size_out,overlay_mapped))
        throw std::runtime_error("OverlayArena output is unaligned, out of bounds, or unmapped");
}

void publish(CPUState* cpu,uint32_t base_out,uint32_t size_out,bool enabled) {
    if (base_out) rbrew_write32(cpu->mem,base_out,enabled?RBREW_OVERLAY_BASE:0);
    if (size_out) rbrew_write32(cpu->mem,size_out,enabled?RBREW_OVERLAY_SIZE:0);
}
}

void rbrew_overlay_set_mapper(CPUState* cpu,RbrewOverlayMapper mapper,void* user) {
    require_arena(cpu);
    auto& owner=state_owner();
    std::lock_guard lock(owner.mutex);
    auto& state=owner.states[cpu->mem];
    if (state.enabled && (mapper!=state.mapper || user!=state.user))
        throw std::runtime_error("Cannot replace the backend of an enabled OverlayArena");
    state.mapper=mapper;state.user=user;
}

RbrewOverlaySnapshot rbrew_overlay_snapshot(const CPUState* cpu) {
    require_arena(cpu);
    auto& owner=state_owner();
    std::lock_guard lock(owner.mutex);
    const auto found=owner.states.find(cpu->mem);
    if (found==owner.states.end() || !found->second.enabled) return {};
    return {true,RBREW_OVERLAY_BASE,RBREW_OVERLAY_SIZE};
}

void rbrew_overlay_is_enabled(CPUState* cpu) {
    cpu->r[3]=rbrew_overlay_snapshot(cpu).enabled?1:0;
}

void rbrew_overlay_enable(CPUState* cpu) {
    require_arena(cpu);
    const uint32_t base_out=cpu->r[4],size_out=cpu->r[5];
    // Validate both destinations before the mapping transition or either write.
    require_outputs(cpu,base_out,size_out,true);
    auto& owner=state_owner();
    std::lock_guard lock(owner.mutex);
    auto& state=owner.states[cpu->mem];
    if (!state.enabled) {
        if (!state.mapper)
            throw std::runtime_error("OverlayArena has no host mapping backend");
        if (!state.mapper(state.user,RBREW_OVERLAY_BASE,RBREW_OVERLAY_SIZE,true))
            throw std::runtime_error("OverlayArena host mapping failed");
        state.enabled=true;
    }
    publish(cpu,base_out,size_out,true);
    // Enable has no guest return value. The opaque incoming r3 is preserved.
}

void rbrew_overlay_get_range(CPUState* cpu) {
    require_arena(cpu);
    const uint32_t base_out=cpu->r[3],size_out=cpu->r[4];
    auto& owner=state_owner();
    std::lock_guard lock(owner.mutex);
    const auto found=owner.states.find(cpu->mem);
    const bool enabled=found!=owner.states.end() && found->second.enabled;
    require_outputs(cpu,base_out,size_out,enabled);
    publish(cpu,base_out,size_out,enabled);
}

void rbrew_overlay_disable(CPUState* cpu) {
    require_arena(cpu);
    auto& owner=state_owner();
    std::lock_guard lock(owner.mutex);
    const auto found=owner.states.find(cpu->mem);
    if (found==owner.states.end() || !found->second.enabled) return;
    auto& state=found->second;
    if (!state.mapper || !state.mapper(state.user,RBREW_OVERLAY_BASE,RBREW_OVERLAY_SIZE,false))
        throw std::runtime_error("OverlayArena host unmapping failed");
    state.enabled=false;
}

void rbrew_overlay_forget_arena(const CPUState* cpu) {
    if (!cpu || !cpu->mem) return;
    auto& owner=state_owner();
    std::lock_guard lock(owner.mutex);
    owner.states.erase(cpu->mem);
}

void coreinit_overlay_register(CPUState* cpu) {
    require_arena(cpu);
    if (!cpu->dispatch_table || (ADDR_OSEnableOverlayArena>>2)>=cpu->dispatch_size ||
        (ADDR_OSIsEnabledOverlayArena>>2)>=cpu->dispatch_size)
        throw std::runtime_error("OverlayArena import registration requires a valid dispatch registry");
    rbrew_register_func(cpu,ADDR_OSEnableOverlayArena,rbrew_overlay_enable);
    rbrew_register_func(cpu,ADDR_OSIsEnabledOverlayArena,rbrew_overlay_is_enabled);
}
