#pragma once

#include "../elf/elf_types.hpp"
#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <unordered_map>

// ============================================================================
// RebrewU — Wii U static recompilation framework
// rpx_types.hpp — Decoded RPX (Wii U executable) data structures
//
// An RPX file is a specialised ELF32/PPC big-endian executable.  After the
// ElfReader has parsed the raw on-disk layout, RpxLoader populates the types
// defined here with host-endian, fully-decoded information ready for the
// disassembler and recompiler.
// ============================================================================

namespace rebrewu::rpx {

// ============================================================================
// Decoded RplFileInfo
// ============================================================================

/// Host-order (decoded) version of the SHT_RPL_FILEINFO section.
struct RplFileInfo {
    uint32_t text_size{0};
    uint32_t text_align{0};
    uint32_t data_size{0};
    uint32_t data_align{0};
    uint32_t load_size{0};
    uint32_t load_align{0};
    uint32_t temp_size{0};
    uint32_t tramp_adjust{0};
    uint32_t sda_base{0};    // r13
    uint32_t sda2_base{0};   // r2
    uint32_t stack_size{0};
    uint32_t flags{0};
    uint32_t heap_size{0};
    uint32_t min_version{0};
    uint32_t compression_level{0};
    uint32_t cafe_sdk_version{0};
    uint32_t cafe_sdk_revision{0};
    uint16_t tls_module_index{0};
    uint16_t tls_align_shift{0};

    bool is_rpx()      const noexcept { return (flags & elf::RPL_FLAG_IS_RPX) != 0; }
    bool has_tls()     const noexcept { return (flags & elf::RPL_FLAG_HAS_TLS) != 0; }
    bool uses_sda()    const noexcept { return (flags & elf::RPL_FLAG_USED_SDA) != 0; }
};

// ============================================================================
// Symbol
// ============================================================================

struct RpxSymbol {
    uint32_t    address{0};   // virtual address (host order)
    uint32_t    size{0};
    std::string name{};
    uint8_t     binding{0};   // STB_*
    uint8_t     type{0};      // STT_*
    uint16_t    section_index{0};

    bool is_function() const noexcept { return type == elf::STT_FUNC; }
    bool is_object()   const noexcept { return type == elf::STT_OBJECT; }
    bool is_global()   const noexcept { return binding == elf::STB_GLOBAL; }
    bool is_local()    const noexcept { return binding == elf::STB_LOCAL; }
    bool is_weak()     const noexcept { return binding == elf::STB_WEAK; }
};

// ============================================================================
// Relocation
// ============================================================================

/// Decoded relocation entry (Rela form — Wii U exclusively uses RELA).
struct RpxReloc {
    uint32_t offset{0};      // target virtual address to patch
    uint32_t sym_index{0};   // index into symbol table
    uint32_t type{0};        // R_PPC_* relocation type
    int32_t  addend{0};

    // Resolved after symbol table is loaded:
    std::optional<std::string> sym_name{};
    std::optional<uint32_t>    sym_value{};
};

// ============================================================================
// Export / Import entries (SHT_RPL_EXPORTS / SHT_RPL_IMPORTS)
// ============================================================================

/// One entry in an .fexport section.
struct RpxExport {
    uint32_t    tls_offset{0};   // offset into TLS area, or 0
    uint32_t    address{0};      // virtual address of the exported symbol
    std::string name{};
    bool        is_data{false};  // if false, it is a code export
};

/// One entry in an .fimport_<module> section.
struct RpxImport {
    uint32_t    stub_address{0}; // virtual address of the import stub
    uint32_t    tls_offset{0};
    std::string name{};
    std::string from_module{};   // name of the exporting RPL (without .rpl)
    bool        is_data{false};
};

// ============================================================================
// Memory Section
// ============================================================================

/// A decoded, decompressed binary section loaded into the virtual address space.
struct RpxSection {
    std::string          name{};
    uint32_t             address{0};     // virtual base address
    uint32_t             size{0};        // size in bytes
    uint32_t             alignment{1};
    std::vector<uint8_t> data{};         // decompressed content

    bool is_executable() const noexcept { return flags & elf::SHF_EXECINSTR; }
    bool is_writable()   const noexcept { return flags & elf::SHF_WRITE; }
    bool is_allocated()  const noexcept { return flags & elf::SHF_ALLOC; }

    bool contains(uint32_t addr) const noexcept {
        return addr >= address && addr < address + size;
    }

    const uint8_t* ptr_at(uint32_t addr) const noexcept {
        if (!contains(addr)) return nullptr;
        return data.data() + (addr - address);
    }

    uint32_t flags{0}; // SHF_*
    uint32_t type{0};  // SHT_*
    uint32_t index{0}; // section index in original ELF
};

// ============================================================================
// RpxModule — fully loaded RPX executable
// ============================================================================

/// The decoded, decompressed, and linked representation of a Wii U RPX binary.
struct RpxModule {
    std::string name{};   // module name derived from file path (no extension)

    RplFileInfo         file_info{};

    std::vector<RpxSection> sections{};
    std::vector<RpxSymbol>  symbols{};
    std::vector<RpxReloc>   relocations{};
    std::vector<RpxExport>  exports{};
    std::vector<RpxImport>  imports{};

    uint32_t entry_point{0};  // virtual address of _start / entrypoint

    // -----------------------------------------------------------------
    // Lookups
    // -----------------------------------------------------------------

    const RpxSection* section_by_name(std::string_view nm) const noexcept {
        for (const auto& s : sections)
            if (s.name == nm) return &s;
        return nullptr;
    }

    const RpxSection* section_at_addr(uint32_t addr) const noexcept {
        for (const auto& s : sections)
            if (s.contains(addr)) return &s;
        return nullptr;
    }

    std::optional<uint32_t> read_word(uint32_t addr) const noexcept {
        const auto* sec = section_at_addr(addr);
        if (!sec) return {};
        const uint8_t* p = sec->ptr_at(addr);
        if (!p) return {};
        // Data is stored as big-endian in the raw section bytes; convert.
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
               (uint32_t(p[2]) <<  8) |  uint32_t(p[3]);
    }

    bool is_code_addr(uint32_t addr) const noexcept {
        const auto* s = section_at_addr(addr);
        return s && s->is_executable();
    }

    const RpxSymbol* symbol_at(uint32_t addr) const noexcept {
        for (const auto& sym : symbols)
            if (sym.address == addr) return &sym;
        return nullptr;
    }

    const RpxSymbol* symbol_named(std::string_view nm) const noexcept {
        for (const auto& sym : symbols)
            if (sym.name == nm) return &sym;
        return nullptr;
    }

    const RpxExport* export_named(std::string_view nm) const noexcept {
        for (const auto& e : exports)
            if (e.name == nm) return &e;
        return nullptr;
    }
};

}