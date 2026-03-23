#pragma once

#include "rpx_types.hpp"
#include "../elf/elf_reader.hpp"
#include "../../diagnostics/diagnostics.hpp"
#include <filesystem>
#include <optional>
#include <span>

// ============================================================================
// RebrewU — Wii U static recompilation framework
// rpx_loader.hpp — Loads and decodes a Wii U RPX executable
//
// RpxLoader wraps ElfReader to produce a fully-decoded RpxModule:
//  1. Parse raw ELF32 structure (via ElfReader)
//  2. Decompress all SHF_RPL_ZLIB sections
//  3. Decode SHT_RPL_FILEINFO → RplFileInfo
//  4. Parse symbol table → RpxSymbol list
//  5. Parse SHT_RPL_EXPORTS / SHT_RPL_IMPORTS
//  6. Collect SHT_RELA relocations
// ============================================================================

namespace rebrewu::rpx {

class RpxLoader {
public:
    explicit RpxLoader(diagnostics::DiagEngine& diag) noexcept;

    /// Load an RPX from a file on disk.
    std::optional<RpxModule> load(const std::filesystem::path& path);

    /// Load an RPX from a memory buffer (useful for tests / embedded scenarios).
    std::optional<RpxModule> load(std::span<const uint8_t> bytes,
                                  std::string module_name = "unknown");

private:
    bool decode_file_info(const elf::ElfImage& img, RpxModule& out);
    bool decode_sections (const elf::ElfImage& img,
                          std::span<const uint8_t> raw, RpxModule& out);
    bool decode_symbols  (const elf::ElfImage& img, RpxModule& out);
    bool decode_exports  (const elf::ElfImage& img, RpxModule& out);
    bool decode_imports  (const elf::ElfImage& img, RpxModule& out);
    bool decode_relocs   (const elf::ElfImage& img, RpxModule& out);

    /// Decompress a zlib-compressed section.  Returns empty on failure.
    std::vector<uint8_t> decompress_section(const elf::RawSection& sec);

    diagnostics::DiagEngine& m_diag;
    elf::ElfReader            m_reader;
};

}