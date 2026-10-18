#pragma once
// Read-only decoder for STATIC display lists shipped pre-built in game files
// (e.g. shader records relocated by FUN_02C625B4 and patched with
// GX2PatchDisplayList). It only extracts register-setting packets so the
// native renderer can load that state into its register file; nothing is
// executed and no GPU is emulated. Draw/event packets are listed, not run.
#include <cstdint>
#include <vector>

struct Gx2RegisterWrite {
    uint32_t index; // Latte register index (config 0x2000, context 0xA000, ...)
    uint32_t value;
    bool operator==(const Gx2RegisterWrite& o) const { return index == o.index && value == o.value; }
};

struct Gx2StaticListState {
    bool ok = false;
    std::vector<Gx2RegisterWrite> writes;  // in list order
    std::vector<uint32_t> other_opcodes;   // type-3 packets that are not register sets (not executed)
};

// Register file size used by the renderer (indices must stay below it).
constexpr uint32_t GX2_LATTE_REGISTER_COUNT = 0x10000;

// bytes: big-endian command words as stored in guest memory.
Gx2StaticListState gx2_decode_static_display_list(const uint8_t* bytes, uint32_t size);
