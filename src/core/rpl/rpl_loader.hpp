#pragma once

#include "rpl_types.hpp"
#include "../elf/elf_reader.hpp"
#include "../../diagnostics/diagnostics.hpp"
#include <filesystem>
#include <optional>
#include <span>

// ============================================================================
// RebrewU — Wii U static recompilation framework
// rpl_loader.hpp — Loads and decodes a Wii U RPL shared library
//
// RplLoader is the library-side counterpart of RpxLoader.  Its pipeline is
// identical (ELF parse → decompress → decode metadata), but it produces an
// RplModule instead of an RpxModule.
// ============================================================================

namespace rebrewu::rpl {

class RplLoader {
public:
    explicit RplLoader(diagnostics::DiagEngine& diag) noexcept;

    /// Load an RPL from a file on disk.
    std::optional<RplModule> load(const std::filesystem::path& path);

    /// Load an RPL from a memory buffer.
    std::optional<RplModule> load(std::span<const uint8_t> bytes,
                                  std::string module_name = "unknown");

private:
    bool decode_file_info(const elf::ElfImage& img, RplModule& out);
    bool decode_sections (const elf::ElfImage& img,
                          std::span<const uint8_t> raw, RplModule& out);
    bool decode_symbols  (const elf::ElfImage& img, RplModule& out);
    bool decode_exports  (const elf::ElfImage& img, RplModule& out);
    bool decode_imports  (const elf::ElfImage& img, RplModule& out);
    bool decode_relocs   (const elf::ElfImage& img, RplModule& out);

    std::vector<uint8_t> decompress_section(const elf::RawSection& sec);

    diagnostics::DiagEngine& m_diag;
    elf::ElfReader            m_reader;
};

}