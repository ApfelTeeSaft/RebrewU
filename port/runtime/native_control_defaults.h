#pragma once

#include "rebrewu_runtime.h"
#include <filesystem>
#include <fstream>
#include <span>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

// Native port control preferences. These are the only two fields selected
// from the owner's example; no example save or progression data is shipped.
namespace rbrew_control_defaults {
constexpr size_t kSaveSize = 0xD280;
constexpr size_t kChecksum = 0x08, kPayload = 0x10;
constexpr size_t kSensitivity = 0x14, kMotion = 0x1A;
constexpr uint32_t kSensitivityBits = 0; // big-endian float 0.0, neutral camera sensitivity
constexpr uint32_t kTutorialSensitivityBits = 0xBE4CCCCDu; // original default -0.2 at 10023A04

inline uint32_t be32(const uint8_t* bytes) {
    return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
           (uint32_t(bytes[2]) << 8) | bytes[3];
}
inline void put_be32(uint8_t* bytes, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[i] = uint8_t(value >> (24 - i * 8));
}
inline uint32_t checksum(std::span<const uint8_t> bytes) {
    uint32_t crc = 0xFFFFFFFFu;
    for (const uint8_t byte : bytes) {
        crc ^= byte;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320u : 0);
    }
    return ~crc;
}

// Verified format for the supported game's saves. Unknown or damaged images
// retain their original bytes.
inline bool patch_image(std::span<uint8_t> image) {
    if (image.size() != kSaveSize || be32(image.data() + 4) != 6) return false;
    if (be32(image.data() + kSensitivity) == kSensitivityBits && image[kMotion] == 0) return false;
    if (be32(image.data() + kChecksum) != checksum(image.subspan(kPayload))) return false;
    put_be32(image.data() + kSensitivity, kSensitivityBits);
    image[kMotion] = 0;
    put_be32(image.data() + kChecksum, checksum(image.subspan(kPayload)));
    return true;
}

enum class FileResult { Unchanged, Updated, Failed };

// Called before opening a save and after closing a written save. Stage the
// edited copy and replace atomically; retain the first original as a backup.
// The entire original payload is preserved apart from the five setting bytes
// and the dependent four-byte checksum. No new/blank save is manufactured.
inline FileResult apply_save_file(const std::filesystem::path& path) noexcept {
    try {
        std::error_code error;
        if (!std::filesystem::exists(path, error))
            return error ? FileResult::Failed : FileResult::Unchanged;
        const auto size = std::filesystem::file_size(path, error);
        if (error) return FileResult::Failed;
        if (size != kSaveSize) return FileResult::Unchanged;
        std::vector<uint8_t> image(kSaveSize);
        {
            std::ifstream input(path, std::ios::binary);
            if (!input.read(reinterpret_cast<char*>(image.data()), image.size())) return FileResult::Failed;
        }
        if (!patch_image(image)) return FileResult::Unchanged;
        auto backup = path;
        backup += ".before-control-defaults";
        if (!std::filesystem::exists(backup, error)) {
            if (error || !std::filesystem::copy_file(path, backup, error) || error) return FileResult::Failed;
        } else if (error) return FileResult::Failed;
        auto temporary = path;
        temporary += ".control-defaults.tmp";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(image.data()), image.size());
            output.flush();
            if (!output) return FileResult::Failed;
            output.close();
            if (!output) return FileResult::Failed;
        }
#if defined(_WIN32)
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return FileResult::Failed;
#else
        std::filesystem::rename(temporary, path, error);
        if (error) return FileResult::Failed;
#endif
        return FileResult::Updated;
    } catch (...) {
        return FileResult::Failed;
    }
}

// Follow the original scene-kind lookup, not save progress or asset filenames:
// 027FADD4 -> *( *(0x101C9578) + 0x15C), then 02019814 maps that scene ID
// through *(0x101C0F28) + 0x18. The game's enum names identify kind 6 as
// Tutorial, kind 5 as TutForShow and kind 3 as Plaza (101C1F48).
// Return unknown while scene initialization/teardown is in progress.
inline int active_scene_type(uint8_t* mem) {
    if (!mem) return -1;
    const uint32_t scene_manager = rbrew_read32(mem, 0x101C9578u);
    const uint32_t scene_types = rbrew_read32(mem, 0x101C0F28u);
    if (!scene_manager || !scene_types ||
        uint64_t(scene_manager) + 0x160 > WIIU_MEM_SIZE ||
        uint64_t(scene_types) + 0x118 > WIIU_MEM_SIZE ||
        !rbrew_memory_accessible(mem, scene_manager + 0x15C, 4) ||
        !rbrew_memory_accessible(mem, scene_types + 0x18, 0x100)) return -1;
    const uint32_t scene_id = rbrew_read32(mem, scene_manager + 0x15C);
    if (scene_id >= 0x40) return -1;
    const uint32_t type = rbrew_read32(mem, scene_types + 0x18 + scene_id * 4);
    return type > 0 && type < 18 ? int(type) : -1;
}

inline void apply_live(uint8_t* mem) {
    if (!mem) return;
    // Original 020355C0 copies the eight-byte GamePad option into +0x17C.
    // Change only its float and gyro byte, preserving both reverse settings.
    // The tutorial locks stick aiming, so retain its original motion mode:
    // the existing VPAD code supplies that motion from the right stick/mouse.
    // Other active scenes, including Plaza, use the requested saved settings.
    const int scene_type = active_scene_type(mem);
    const bool tutorial = scene_type == 5 || scene_type == 6;
    const uint32_t sensitivity = tutorial ? kTutorialSensitivityBits : kSensitivityBits;
    const uint8_t motion = tutorial ? 1 : 0;
    constexpr uint32_t kControlManager = 0x101C1D34u;
    const uint32_t manager = rbrew_read32(mem, kControlManager);
    if (scene_type >= 0 && manager && uint64_t(manager) + 0x184 <= WIIU_MEM_SIZE &&
        rbrew_memory_accessible(mem, manager + 0x17C, 8)) {
        if (rbrew_read32(mem, manager + 0x17C) != sensitivity)
            rbrew_write32(mem, manager + 0x17C, sensitivity);
        if (mem[manager + 0x182] != motion) rbrew_write8(mem, manager + 0x182, motion);
    }
    // 021729AC -> 02173470 -> 02172AD8 proves that the saved options are
    // at *(save_manager + 0xC) + 0x14. Keep that cached save image in sync
    // too. Save motion off/0.0 even during the temporary tutorial override;
    // only the active GamePad options use the tutorial's original defaults.
    constexpr uint32_t kSaveManager = 0x101C9564u;
    const uint32_t save_manager = rbrew_read32(mem, kSaveManager);
    if (!save_manager || uint64_t(save_manager) + 0x10 > WIIU_MEM_SIZE ||
        !rbrew_memory_accessible(mem, save_manager + 0x0C, 4)) return;
    const uint32_t image = rbrew_read32(mem, save_manager + 0x0C);
    if (image && uint64_t(image) + kSaveSize <= WIIU_MEM_SIZE &&
        rbrew_memory_accessible(mem, image, kSaveSize))
        patch_image(std::span<uint8_t>(mem + image, kSaveSize));
}
} // namespace rbrew_control_defaults
