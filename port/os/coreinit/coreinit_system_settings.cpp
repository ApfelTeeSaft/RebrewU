#include "coreinit_system_settings.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace {
struct ArenaSettings {
    RbrewMcpSystemProfile profile;
    std::unordered_set<uint32_t> handles;
};
struct SettingsOwner {
    std::mutex mutex;
    std::unordered_map<const uint8_t*, ArenaSettings> arenas;
    uint32_t next_handle{1};
};
SettingsOwner& settings_owner() {
    static auto* owner = new SettingsOwner;
    return *owner;
}

void require_arena(const CPUState* cpu) {
    if (!cpu || !cpu->mem || !cpu->guest_memory_size || cpu->guest_memory_size > WIIU_MEM_SIZE)
        throw std::runtime_error("MCP system settings require a valid guest arena");
}
void require_profile(const RbrewMcpSystemProfile& profile) {
    const uint32_t area = profile.product_area, regions = profile.game_region;
    if (!area || (area & ~RBREW_MCP_REGION_MASK) || (area & (area - 1)))
        throw std::invalid_argument("MCP product area must select one supported console region");
    if (!regions || (regions & ~RBREW_MCP_REGION_MASK))
        throw std::invalid_argument("MCP game region must be a nonempty supported region mask");
}
void put_u32(RbrewMcpSysProdBytes& bytes, uint32_t offset, uint32_t value) {
    for (uint32_t i = 0; i < 4; ++i) bytes[offset + i] = uint8_t(value >> (24 - 8 * i));
}
template<std::size_t N>
void put_bytes(RbrewMcpSysProdBytes& bytes, uint32_t offset, const std::array<uint8_t, N>& value) {
    std::copy(value.begin(), value.end(), bytes.begin() + offset);
}
ArenaSettings& require_settings(SettingsOwner& owner, const CPUState* cpu) {
    auto found = owner.arenas.find(cpu->mem);
    if (found == owner.arenas.end())
        throw std::runtime_error("MCP system profile has not been configured");
    return found->second;
}
void require_handle(const ArenaSettings& settings, uint32_t handle) {
    if (!settings.handles.contains(handle))
        throw std::runtime_error("MCP handle is invalid, closed, or owned by another guest arena");
}
}

RbrewMcpSysProdBytes rbrew_mcp_serialize_profile(const RbrewMcpSystemProfile& profile) {
    require_profile(profile);
    RbrewMcpSysProdBytes bytes{};
    put_u32(bytes, 0x00, profile.product_area);
    bytes[0x04] = uint8_t(profile.eeprom_version >> 8);
    bytes[0x05] = uint8_t(profile.eeprom_version);
    put_bytes(bytes, 0x06, profile.padding_06);
    put_u32(bytes, 0x08, profile.game_region);
    put_bytes(bytes, 0x0c, profile.unknown_0c);
    put_bytes(bytes, 0x10, profile.ntsc_pal);
    put_bytes(bytes, 0x15, profile.wifi_5ghz_country_code);
    bytes[0x19] = profile.wifi_5ghz_country_code_revision;
    put_bytes(bytes, 0x1a, profile.code_id);
    put_bytes(bytes, 0x22, profile.serial_id);
    put_bytes(bytes, 0x2e, profile.unknown_2e);
    put_bytes(bytes, 0x32, profile.model_number);
    put_u32(bytes, 0x42, profile.version);
    return bytes;
}

void rbrew_mcp_configure_profile(CPUState* cpu, const RbrewMcpSystemProfile& profile) {
    require_arena(cpu);
    require_profile(profile);
    auto& owner = settings_owner();
    std::lock_guard lock(owner.mutex);
    const auto found = owner.arenas.find(cpu->mem);
    if (found != owner.arenas.end() && !found->second.handles.empty() && found->second.profile != profile)
        throw std::runtime_error("Cannot replace an MCP profile while sessions are open");
    auto& settings = owner.arenas[cpu->mem];
    settings.profile = profile;
}

RbrewMcpSystemProfile rbrew_mcp_profile_snapshot(const CPUState* cpu) {
    require_arena(cpu);
    auto& owner = settings_owner();
    std::lock_guard lock(owner.mutex);
    return require_settings(owner, cpu).profile;
}

void rbrew_mcp_open(CPUState* cpu) {
    require_arena(cpu);
    auto& owner = settings_owner();
    std::lock_guard lock(owner.mutex);
    auto& settings = require_settings(owner, cpu);
    if (owner.next_handle > uint32_t(std::numeric_limits<int32_t>::max()))
        throw std::runtime_error("Native MCP handle namespace exhausted");
    const uint32_t handle = owner.next_handle;
    settings.handles.insert(handle);
    ++owner.next_handle;
    cpu->r[3] = handle;
}

void rbrew_mcp_close(CPUState* cpu) {
    require_arena(cpu);
    auto& owner = settings_owner();
    std::lock_guard lock(owner.mutex);
    auto& settings = require_settings(owner, cpu);
    require_handle(settings, cpu->r[3]);
    settings.handles.erase(cpu->r[3]);
    cpu->r[3] = 0;
}

void rbrew_mcp_get_sys_prod_settings(CPUState* cpu) {
    require_arena(cpu);
    const uint32_t output = cpu->r[4];
    // MCPSysProdSettings is packed, so byte-aligned output is valid. Its exact
    // extent must be accessible; never round up to an unrelated stack size.
    if (!output || uint64_t(output) + RBREW_MCP_SYS_PROD_SIZE > cpu->guest_memory_size ||
        !rbrew_memory_accessible(cpu->mem, output, RBREW_MCP_SYS_PROD_SIZE))
        throw std::runtime_error("MCP system settings output is outside accessible guest memory");
    RbrewMcpSysProdBytes bytes;
    {
        auto& owner = settings_owner();
        std::lock_guard lock(owner.mutex);
        auto& settings = require_settings(owner, cpu);
        require_handle(settings, cpu->r[3]);
        bytes = rbrew_mcp_serialize_profile(settings.profile);
    }
    // The snapshot is complete before guest-memory publication; no provider
    // lock spans the shared memory writer or an optional observation callback.
    rbrew_write_memory(cpu->mem, output, bytes.data(), uint32_t(bytes.size()));
    cpu->r[3] = 0;
}

void rbrew_mcp_forget_arena(const CPUState* cpu) {
    if (!cpu || !cpu->mem) return;
    auto& owner = settings_owner();
    std::lock_guard lock(owner.mutex);
    owner.arenas.erase(cpu->mem);
}
