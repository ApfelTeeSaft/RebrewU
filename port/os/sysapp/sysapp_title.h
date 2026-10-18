#pragma once
#include "rebrewu_runtime.h"
#include <cstdint>

// System application title ids by _SYSGetSystemApplicationTitleId index for a
// console product area (Japan=1, USA=2, Europe=4). Table and region columns
// follow Cemu sysapp. Indexes past the table or other product areas throw.
uint64_t rbrew_sysapp_title_id(uint32_t index, uint32_t product_area);

// Registers _SYSGetSystemApplicationTitleId, which reads the native MCP
// profile's product area and returns the 64-bit id in r3 (high) / r4 (low).
void sysapp_title_register(CPUState* cpu);
