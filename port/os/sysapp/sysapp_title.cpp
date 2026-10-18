#include "sysapp_title.h"
#include "../coreinit/coreinit_system_settings.h"
#include "../stubs/stubs_addrs.h"
#include <stdexcept>

// Evidence: FFL FUN_02C72A30 calls 02C79888 (li r3,4; b _SYSGetSystemApplicationTitleId)
// and passes r3:r4 as the title of SAVEGetSharedSaveDataPath for db/FFL_*.dat.
// Cemu (EUR title) resolves this to /vol/storage_mlc01/usr/save/00050010/1004a200.

namespace {
// Full 64-bit ids per region column: Japan, USA, Europe.
constexpr uint64_t kTitles[][3] = {
    {0x5001010040000, 0x5001010040100, 0x5001010040200}, // Wii U Menu
    {0x5001010047000, 0x5001010047100, 0x5001010047200}, // System Settings
    {0x5001010048000, 0x5001010048100, 0x5001010048200}, // Parental Controls
    {0x5001010049000, 0x5001010049100, 0x5001010049200}, // User Settings
    {0x500101004A000, 0x500101004A100, 0x500101004A200}, // Mii Maker
    {0x500101004B000, 0x500101004B100, 0x500101004B200}, // Account Settings
    {0x500101004C000, 0x500101004C100, 0x500101004C200}, // Daily Log
    {0x500101004D000, 0x500101004D100, 0x500101004D200}, // Notifications
    {0x500101004E000, 0x500101004E100, 0x500101004E200}, // Health and Safety
    {0x5001B10059000, 0x5001B10059100, 0x5001B10059200}, // Electronic Manual
    {0x500101005A000, 0x500101005A100, 0x500101005A200}, // Wii U Chat
    {0x5001010062000, 0x5001010062100, 0x5001010062200}, // Data Transfer
};

void SYSGetSystemApplicationTitleId(CPUState* cpu) {
    const uint64_t id = rbrew_sysapp_title_id(cpu->r[3], rbrew_mcp_profile_snapshot(cpu).product_area);
    cpu->r[3] = uint32_t(id >> 32);
    cpu->r[4] = uint32_t(id);
}
}

uint64_t rbrew_sysapp_title_id(uint32_t index, uint32_t product_area) {
    if (index >= sizeof(kTitles) / sizeof(kTitles[0]))
        throw std::runtime_error("System application index is outside the known table");
    switch (product_area) {
    case 1: return kTitles[index][0];
    case 2: return kTitles[index][1];
    case 4: return kTitles[index][2];
    default: throw std::runtime_error("System application ids are unknown for this product area");
    }
}

void sysapp_title_register(CPUState* cpu) {
    rbrew_register_func(cpu, ADDR__SYSGetSystemApplicationTitleId, SYSGetSystemApplicationTitleId);
}
