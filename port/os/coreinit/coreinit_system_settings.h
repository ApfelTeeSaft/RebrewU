#pragma once

#include "rebrewu_runtime.h"
#include <array>
#include <cstdint>

// Cafe's packed MCPSysProdSettings is 0x46 bytes. This host profile is not
// overlaid on guest memory: numeric fields are serialized explicitly in BE.
inline constexpr uint32_t RBREW_MCP_SYS_PROD_SIZE = 0x46;
inline constexpr uint32_t RBREW_MCP_REGION_MASK = 0x77;
using RbrewMcpSysProdBytes = std::array<uint8_t, RBREW_MCP_SYS_PROD_SIZE>;

struct RbrewMcpSystemProfile {
    uint32_t product_area{};                       // +00: selected console region
    uint16_t eeprom_version{};                     // +04
    std::array<uint8_t, 2> padding_06{};
    uint32_t game_region{};                        // +08: title allowed-region mask
    std::array<uint8_t, 4> unknown_0c{};
    std::array<uint8_t, 5> ntsc_pal{};              // +10
    std::array<uint8_t, 4> wifi_5ghz_country_code{}; // +15
    uint8_t wifi_5ghz_country_code_revision{};      // +19
    std::array<uint8_t, 8> code_id{};               // +1a
    std::array<uint8_t, 12> serial_id{};            // +22
    std::array<uint8_t, 4> unknown_2e{};
    std::array<uint8_t, 16> model_number{};         // +32
    uint32_t version{};                            // +42, packed/unaligned
    bool operator==(const RbrewMcpSystemProfile&) const = default;
};

// A console product area selects one of Japan=1, USA=2, Europe=4, China=0x10,
// Korea=0x20, Taiwan=0x40. The title mask may combine these supported regions.
// Unspecified profile bytes retain caller-provided zero defaults; serials,
// model IDs and other console-specific values are never invented here.
RbrewMcpSysProdBytes rbrew_mcp_serialize_profile(const RbrewMcpSystemProfile& profile);
void rbrew_mcp_configure_profile(CPUState* cpu, const RbrewMcpSystemProfile& profile);
RbrewMcpSystemProfile rbrew_mcp_profile_snapshot(const CPUState* cpu);

// Native MCP sessions are backed per arena, with independent guest workers
// sharing the profile/handle namespace. No guessed IOS error codes: unsupported
// configuration, invalid handles and inaccessible outputs throw before mutation.
// Open() returns a nonnegative handle in r3; Close(handle) and
// GetSysProdSettings(handle, out=r4) return the proven success status zero.
void rbrew_mcp_open(CPUState* cpu);
void rbrew_mcp_close(CPUState* cpu);
void rbrew_mcp_get_sys_prod_settings(CPUState* cpu);

// Owner-only teardown after guest workers are quiescent. Process-lifetime
// registry storage avoids static-destruction races with still-active workers.
void rbrew_mcp_forget_arena(const CPUState* cpu);
