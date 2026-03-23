#pragma once

#include "../elf/elf_types.hpp"
#include "../rpx/rpx_types.hpp"
#include <cstdint>
#include <string>
#include <vector>
#include <optional>

// ============================================================================
// RebrewU — Wii U static recompilation framework
// rpl_types.hpp — Decoded RPL (Wii U shared library) data structures
//
// An RPL is structurally identical to an RPX (both are ELF32/PPC), but it is
// a shared library rather than an executable.  We reuse the common types from
// rpx_types.hpp and add RPL-specific fields.
// ============================================================================

namespace rebrewu::rpl {

// An RPL exports a set of functions and data objects that other modules
// (RPX or other RPLs) can import.  The layout is the same as RpxExport
// but the semantics are subtly different (all exports are available at link
// time, not resolved lazily).

using RplExport  = rpx::RpxExport;
using RplImport  = rpx::RpxImport;
using RplSection = rpx::RpxSection;
using RplSymbol  = rpx::RpxSymbol;
using RplReloc   = rpx::RpxReloc;
using RplFileInfo= rpx::RplFileInfo;

// ============================================================================
// RplModule — fully loaded RPL shared library
// ============================================================================

/// Decoded representation of a Wii U RPL binary.
/// The linker aggregates multiple RplModules to resolve imports in an RPX.
struct RplModule {
    std::string name{};      // module name (no path, no .rpl extension)

    RplFileInfo             file_info{};

    std::vector<RplSection> sections{};
    std::vector<RplSymbol>  symbols{};
    std::vector<RplReloc>   relocations{};
    std::vector<RplExport>  exports{};
    std::vector<RplImport>  imports{};   // RPLs may themselves import from other RPLs

    // -----------------------------------------------------------------
    // Lookups
    // -----------------------------------------------------------------

    const RplSection* section_by_name(std::string_view nm) const noexcept {
        for (const auto& s : sections)
            if (s.name == nm) return &s;
        return nullptr;
    }

    const RplExport* export_named(std::string_view nm) const noexcept {
        for (const auto& e : exports)
            if (e.name == nm) return &e;
        return nullptr;
    }

    const RplSymbol* symbol_named(std::string_view nm) const noexcept {
        for (const auto& sym : symbols)
            if (sym.name == nm) return &sym;
        return nullptr;
    }

    /// Returns true if this RPL exports the named symbol (data or code).
    bool has_export(std::string_view nm) const noexcept {
        return export_named(nm) != nullptr;
    }
};

}