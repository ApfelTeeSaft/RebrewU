#include "coreinit_dynload.h"

#include <array>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace {
struct ExportRecord {
    uint32_t address;
    HostFunc function; // nullptr for guest data
    bool operator==(const ExportRecord&) const = default;
};
using ExportMap = std::unordered_map<std::string, ExportRecord>;
struct ModuleRecord {
    uint32_t handle{};
    uint32_t references{};
    std::array<ExportMap, 2> exports;
};
struct ArenaRegistry { std::unordered_map<std::string, ModuleRecord> modules; };
struct RegistryOwner {
    std::mutex mutex;
    std::unordered_map<uint8_t*, ArenaRegistry> arenas;
    uint32_t next_handle = 1;
};
RegistryOwner& registry_owner() {
    // Native guest workers may still use this provider while the launch
    // thread exits. Destroy per-arena records explicitly after quiescence,
    // keeping their shared owner alive through process termination.
    static auto* owner = new RegistryOwner;
    return *owner;
}

bool valid_range(const CPUState* cpu, uint32_t address, uint32_t size) {
    return cpu && cpu->mem && cpu->guest_memory_size <= WIIU_MEM_SIZE &&
        uint64_t(address) + size <= cpu->guest_memory_size &&
        rbrew_memory_accessible(cpu->mem,address,size);
}

bool valid_output(const CPUState* cpu, uint32_t address) {
    return address && !(address & 3u) && valid_range(cpu, address, 4);
}

bool read_string(const CPUState* cpu, uint32_t address, std::string& result) {
    if (!address || !valid_range(cpu, address, 1)) return false;
    result.clear();
    // Scan only this declared guest arena; no host strlen or arbitrary bound
    // that might change valid Cafe strings. Strings are byte-encoded API names.
    for (uint64_t cursor = address; cursor < cpu->guest_memory_size; ++cursor) {
        if (!rbrew_memory_accessible(cpu->mem,uint32_t(cursor),1)) return false;
        const char value = static_cast<char>(cpu->mem[cursor]);
        if (!value) return true;
        result.push_back(value);
    }
    return false;
}

void write_output(CPUState* cpu, uint32_t address, uint32_t value) {
    const uint8_t bytes[] = {uint8_t(value >> 24), uint8_t(value >> 16),
        uint8_t(value >> 8), uint8_t(value)};
    rbrew_write_memory(cpu->mem, address, bytes, 4);
}

uint32_t fresh_handle() {
    auto& next_handle = registry_owner().next_handle;
    // Handles are opaque IDs backed by records, never guest or host pointers.
    if (!next_handle || next_handle == std::numeric_limits<uint32_t>::max())
        throw std::overflow_error("native module handle space exhausted");
    return next_handle++;
}

HostFunc registered_function(const CPUState* cpu, uint32_t address) {
    if (!cpu || !address || (address & 3u) || !cpu->dispatch_table ||
        (address >> 2) >= cpu->dispatch_size) return nullptr;
    return cpu->dispatch_table[address >> 2];
}

ModuleRecord* find_handle(ArenaRegistry& registry, uint32_t handle) {
    for (auto& [name, module] : registry.modules)
        if (module.handle == handle && module.references) return &module;
    return nullptr;
}
}

uint32_t rbrew_dynload_register_module(CPUState* cpu, std::string_view module_name,
    std::span<const RbrewDynLoadExport> exports) {
    if (!cpu || !cpu->mem || !cpu->guest_memory_size ||
        cpu->guest_memory_size > WIIU_MEM_SIZE || module_name.empty() ||
        module_name.find('\0') != std::string_view::npos)
        throw std::invalid_argument("native module declaration requires a valid arena and exact name");
    std::array<ExportMap, 2> maps;
    for (const auto& item : exports) {
        const auto kind = static_cast<uint32_t>(item.kind);
        if (kind > 1 || item.name.empty() || item.name.find('\0') != std::string_view::npos)
            throw std::invalid_argument("invalid native module export name or kind");
        ExportRecord record{item.guest_address, nullptr};
        if (kind == 0) {
            record.function = registered_function(cpu, item.guest_address);
            if (!record.function)
                throw std::invalid_argument("native code export has no registered callable implementation");
        } else if (!item.guest_address || !valid_range(cpu, item.guest_address, 1)) {
            throw std::invalid_argument("native data export is outside guest memory");
        }
        if (!maps[kind].emplace(std::string(item.name), record).second)
            throw std::invalid_argument("duplicate native module export name and kind");
    }
    auto& owner = registry_owner();
    std::lock_guard lock(owner.mutex);
    auto& modules = owner.arenas[cpu->mem].modules;
    const std::string name(module_name);
    if (const auto existing = modules.find(name); existing != modules.end()) {
        if (existing->second.exports != maps)
            throw std::invalid_argument("conflicting native module declaration");
        if (!existing->second.handle) existing->second.handle = fresh_handle();
        return existing->second.handle;
    }
    ModuleRecord module;
    module.handle = fresh_handle();
    module.exports = std::move(maps);
    const uint32_t handle = module.handle;
    modules.emplace(name, std::move(module));
    return handle;
}

void rbrew_dynload_acquire(CPUState* cpu) {
    if (!cpu) return;
    const uint32_t name_address = cpu->r[3], output = cpu->r[4];
    if (!valid_output(cpu, output)) {
        cpu->r[3] = rbrew_dynload_error::invalid_acquire_pointer;
        return;
    }
    std::string name;
    if (!read_string(cpu, name_address, name)) {
        cpu->r[3] = rbrew_dynload_error::invalid_module_name_pointer;
        return;
    }
    if (name.empty()) {
        cpu->r[3] = rbrew_dynload_error::invalid_module_name;
        return;
    }
    auto& owner = registry_owner();
    std::lock_guard lock(owner.mutex);
    const auto arena = owner.arenas.find(cpu->mem);
    if (arena == owner.arenas.end() || !arena->second.modules.contains(name)) {
        write_output(cpu, output, 0);
        cpu->r[3] = rbrew_dynload_error::module_not_found;
        return;
    }
    auto& module = arena->second.modules.at(name);
    if (module.references == std::numeric_limits<uint32_t>::max()) {
        // Do not wrap a live reference count into an unloaded record.
        cpu->r[3] = 0xBAD1002Fu; // OutOfSysMemory, no reference/publication
        return;
    }
    if (!module.handle) module.handle = fresh_handle();
    ++module.references;
    write_output(cpu, output, module.handle);
    cpu->r[3] = rbrew_dynload_error::ok;
}

void rbrew_dynload_find_export(CPUState* cpu) {
    if (!cpu) return;
    const uint32_t handle = cpu->r[3], kind = cpu->r[4];
    const uint32_t name_address = cpu->r[5], output = cpu->r[6];
    if (!valid_output(cpu, output) || kind > 1) {
        cpu->r[3] = rbrew_dynload_error::invalid_parameter;
        return;
    }
    std::string name;
    if (!read_string(cpu, name_address, name) || name.empty()) {
        cpu->r[3] = rbrew_dynload_error::invalid_parameter;
        return;
    }
    auto& owner = registry_owner();
    std::lock_guard lock(owner.mutex);
    const auto arena = owner.arenas.find(cpu->mem);
    ModuleRecord* module = arena == owner.arenas.end() ? nullptr : find_handle(arena->second, handle);
    if (!module) {
        cpu->r[3] = rbrew_dynload_error::invalid_handle;
        return;
    }
    const auto& map = module->exports[kind];
    if (map.empty()) {
        cpu->r[3] = kind ? rbrew_dynload_error::module_has_no_data_exports
                        : rbrew_dynload_error::module_has_no_code_exports;
        return;
    }
    const auto entry = map.find(name);
    if (entry == map.end() || (kind == 0 &&
        registered_function(cpu, entry->second.address) != entry->second.function)) {
        cpu->r[3] = rbrew_dynload_error::export_not_found;
        return;
    }
    if (kind && !valid_range(cpu, entry->second.address, 1)) {
        cpu->r[3] = rbrew_dynload_error::invalid_parameter;
        return;
    }
    write_output(cpu, output, entry->second.address);
    cpu->r[3] = rbrew_dynload_error::ok;
}

void rbrew_dynload_release(CPUState* cpu) {
    if (!cpu) return;
    auto& owner = registry_owner();
    std::lock_guard lock(owner.mutex);
    const auto arena = owner.arenas.find(cpu->mem);
    if (arena == owner.arenas.end()) return;
    if (auto* module = find_handle(arena->second, cpu->r[3])) {
        if (!--module->references) module->handle = 0;
    }
}

uint32_t rbrew_dynload_reference_count(const CPUState* cpu, uint32_t handle) {
    if (!cpu) return 0;
    auto& owner = registry_owner();
    std::lock_guard lock(owner.mutex);
    const auto arena = owner.arenas.find(cpu->mem);
    if (arena == owner.arenas.end()) return 0;
    const auto module = find_handle(arena->second, handle);
    return module ? module->references : 0;
}

void rbrew_dynload_forget_arena(const CPUState* cpu) {
    if (!cpu) return;
    auto& owner = registry_owner();
    std::lock_guard lock(owner.mutex);
    owner.arenas.erase(cpu->mem);
}
