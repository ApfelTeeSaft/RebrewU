#pragma once
// Cafe shared-data area (system fonts) - layout as on the console (Cemu
// coreinit OSGetSharedData notes): at the area base a table of 0x1C-byte
// entries {name=0xFFCAFE01.., fileType=2 (font), kernelName, data, size, 0, 0},
// font files from +0x70, each 64-byte aligned.
#include <array>
#include <cstdint>
#include <vector>

// Writes the table and fonts into `area` (host bytes of guest address
// `base`). fonts[i] is entry 0xFFCAFE01+i (Cn, Kr, Std, Tw); empty = absent.
// Returns false if they do not fit.
bool rbrew_shared_data_build(uint8_t* area, uint32_t area_size, uint32_t base,
                             const std::array<std::vector<uint8_t>, 4>& fonts);

// OSGetSharedData lookup (coreinit.rpl 02029e84): type 0..3 -> name
// 0xFFCAFE01+type. Returns false when the type is invalid or not loaded.
bool rbrew_shared_data_lookup(const uint8_t* area, uint32_t type, uint32_t* guest_ptr, uint32_t* size);
