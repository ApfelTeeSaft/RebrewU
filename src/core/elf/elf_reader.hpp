#pragma once

#include "elf_types.hpp"
#include "../../diagnostics/diagnostics.hpp"
#include <span>
#include <vector>
#include <string>
#include <optional>
#include <filesystem>

// ============================================================================
// RebrewU — Wii U static recompilation framework
// elf_reader.hpp — Low-level ELF32 file reader
//
// ElfReader validates and parses the on-disk ELF32 structure, byte-swaps all
// header fields, and exposes sections/symbols/relocations for higher-level
// loaders (rpx_loader, rpl_loader).  It does NOT decompress zlib-compressed
// sections — that is the responsibility of the callers.
// ============================================================================

namespace rebrewu::elf {

/// Raw (undecompressed) section data as seen inside the file.
struct RawSection {
    Elf32_Shdr header{};      // byte-swapped section header
    std::string name{};       // resolved from .shstrtab
    std::vector<uint8_t> data{}; // raw section bytes (may be zlib-compressed)
};

/// Result of loading and parsing an ELF file.
struct ElfImage {
    Elf32_Ehdr              ehdr{};        // byte-swapped ELF header
    std::vector<Elf32_Phdr> phdrs{};       // byte-swapped program headers
    std::vector<RawSection> sections{};    // all sections, in order

    // Convenience lookups
    const RawSection* section_by_name(std::string_view nm) const noexcept;
    const RawSection* section_by_index(uint32_t idx)       const noexcept;
    const RawSection* first_section_of_type(Elf32_Word type) const noexcept;
};

/// ELF32 parser for Wii U RPX/RPL big-endian binaries.
class ElfReader {
public:
    explicit ElfReader(diagnostics::DiagEngine& diag) noexcept;

    /// Parse an ELF image from a memory buffer.
    /// Returns nullopt on any validation error (errors emitted via diag).
    std::optional<ElfImage> parse(std::span<const uint8_t> bytes);

    /// Convenience: load a file from disk then parse.
    std::optional<ElfImage> load(const std::filesystem::path& path);

private:
    bool validate_ident(std::span<const uint8_t> bytes);
    bool parse_shdrs(std::span<const uint8_t> bytes,
                     const Elf32_Ehdr& ehdr,
                     ElfImage& out);
    bool parse_phdrs(std::span<const uint8_t> bytes,
                     const Elf32_Ehdr& ehdr,
                     ElfImage& out);
    bool resolve_section_names(std::span<const uint8_t> bytes, ElfImage& out);

    diagnostics::DiagEngine& m_diag;
};

// Inline implementations

inline const RawSection*
ElfImage::section_by_name(std::string_view nm) const noexcept {
    for (const auto& s : sections)
        if (s.name == nm) return &s;
    return nullptr;
}

inline const RawSection*
ElfImage::section_by_index(uint32_t idx) const noexcept {
    return idx < sections.size() ? &sections[idx] : nullptr;
}

inline const RawSection*
ElfImage::first_section_of_type(Elf32_Word type) const noexcept {
    for (const auto& s : sections)
        if (s.header.type() == type) return &s;
    return nullptr;
}

}