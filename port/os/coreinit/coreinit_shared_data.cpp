#include "../../runtime/rbrew_diagnostics.h"
#include "coreinit_shared_data.h"
#include "../os_common.h"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace {
constexpr uint32_t kEntrySize = 0x1C, kEntries = 4, kDataStart = 0x70;
void be32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
}

bool rbrew_shared_data_build(uint8_t* area, uint32_t area_size, uint32_t base,
                             const std::array<std::vector<uint8_t>, 4>& fonts) {
    if (!area || area_size < kDataStart) return false;
    std::memset(area, 0, kEntries * kEntrySize);
    uint32_t offset = kDataStart, row = 0;
    for (uint32_t i = 0; i < kEntries; ++i) {
        if (fonts[i].empty()) continue;
        if (uint64_t(offset) + fonts[i].size() > area_size) return false;
        std::memcpy(area + offset, fonts[i].data(), fonts[i].size());
        uint8_t* e = area + row++ * kEntrySize;
        be32(e + 0x00, 0xFFCAFE01u + i);
        be32(e + 0x04, 2);                       // file type: font
        be32(e + 0x0C, base + offset);           // data
        be32(e + 0x10, uint32_t(fonts[i].size()));
        offset = (offset + uint32_t(fonts[i].size()) + 63u) & ~63u;
    }
    return true;
}

bool rbrew_shared_data_lookup(const uint8_t* area, uint32_t type, uint32_t* guest_ptr, uint32_t* size) {
    if (!area || type > 3) return false;
    for (uint32_t i = 0; i < kEntries; ++i) {
        const uint8_t* e = area + i * kEntrySize;
        if (rd32(e) != 0xFFCAFE01u + type) continue;
        *guest_ptr = rd32(e + 0x0C);
        *size = rd32(e + 0x10);
        return *size != 0;
    }
    return false;
}

// --- OSGetSharedData(type, flags, u32* ptr, u32* size) -> BOOL ------------
// The console's kernel preloads the fonts of system title 0005001b-10042400.
// Here they are read once from the system MLC (RBREW_SYSTEM_MLC_DIR, else
// RBREW_MLC_DIR) into the shared-data area.
static void load_shared_fonts(uint8_t* mem) {
    static const char* names[4] = {"CafeCn.ttf", "CafeKr.ttf", "CafeStd.ttf", "CafeTw.ttf"};
    const char* root = std::getenv("RBREW_SYSTEM_MLC_DIR");
    if (!root || !*root) root = std::getenv("RBREW_MLC_DIR");
    if (!root || !*root) root = "./mlc01";
    std::array<std::vector<uint8_t>, 4> fonts;
    for (int i = 0; i < 4; ++i) {
        auto path = std::filesystem::path(root) / "sys/title/0005001b/10042400/content" / names[i];
        std::ifstream in(path, std::ios::binary);
        if (!in) { RBREW_LOGF(stderr, "[coreinit] shared font missing: %s\n", path.string().c_str()); continue; }
        fonts[i].assign(std::istreambuf_iterator<char>(in), {});
    }
    if (!rbrew_shared_data_build(mem + RBREW_SHARED_DATA_BASE, RBREW_SHARED_DATA_SIZE, RBREW_SHARED_DATA_BASE, fonts))
        RBREW_LOGF(stderr, "[coreinit] shared fonts do not fit the shared-data area\n");
}

static void OSGetSharedData(CPUState* cpu) {
    const uint32_t type = ARG0, ptr_out = ARG2, size_out = ARG3;
    if (!ptr_out || !size_out) { RET = 0; return; }           // console: OSPanic, returns 0
    static std::once_flag once;
    std::call_once(once, [&] { load_shared_fonts(MEM); });
    uint32_t ptr = 0, size = 0;
    if (!rbrew_shared_data_lookup(MEM + RBREW_SHARED_DATA_BASE, type, &ptr, &size)) { RET = 0; return; }
    rbrew_write32(MEM, ptr_out, ptr);
    rbrew_write32(MEM, size_out, size);
    RET = 1;
}

#include "coreinit_addrs.h"
void coreinit_shared_data_register(CPUState* cpu) {
    rbrew_register_func(cpu, ADDR_OSGetSharedData, OSGetSharedData);
}
