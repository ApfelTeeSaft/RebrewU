#pragma once

#include "../elf/elf_types.hpp"
#include "../rpx/rpx_types.hpp"
#include "../../diagnostics/diagnostics.hpp"
#include <cstdint>
#include <vector>
#include <optional>
#include <string>
#include <functional>

// ============================================================================
// RebrewU — Wii U static recompilation framework
// reloc_processor.hpp — PowerPC ELF relocation types and processor
//
// Wii U RPX/RPL files use ELF RELA (explicit-addend) relocations with the
// PowerPC relocation types defined by the PowerPC ABI / System V ABI for PPC.
// This header defines the relocation types and the RelocationProcessor that
// patches a loaded image's sections.
// ============================================================================

namespace rebrewu::reloc {

// ============================================================================
// PowerPC EABI relocation type codes (r_type in Elf32_Rela)
// ============================================================================

inline constexpr uint32_t R_PPC_NONE          =  0;
inline constexpr uint32_t R_PPC_ADDR32        =  1;  // S + A
inline constexpr uint32_t R_PPC_ADDR24        =  2;  // (S + A) >> 2 (26-bit)
inline constexpr uint32_t R_PPC_ADDR16        =  3;  // S + A (16-bit)
inline constexpr uint32_t R_PPC_ADDR16_LO     =  4;  // (S + A) & 0xFFFF
inline constexpr uint32_t R_PPC_ADDR16_HI     =  5;  // (S + A) >> 16
inline constexpr uint32_t R_PPC_ADDR16_HA     =  6;  // ((S + A + 0x8000) >> 16)
inline constexpr uint32_t R_PPC_ADDR14        =  7;
inline constexpr uint32_t R_PPC_ADDR14_BRTAKEN=  8;
inline constexpr uint32_t R_PPC_ADDR14_BRNTAKEN= 9;
inline constexpr uint32_t R_PPC_REL24         = 10;  // (S + A - P) >> 2
inline constexpr uint32_t R_PPC_REL14         = 11;
inline constexpr uint32_t R_PPC_REL14_BRTAKEN = 12;
inline constexpr uint32_t R_PPC_REL14_BRNTAKEN= 13;
inline constexpr uint32_t R_PPC_GOT16         = 14;
inline constexpr uint32_t R_PPC_GOT16_LO      = 15;
inline constexpr uint32_t R_PPC_GOT16_HI      = 16;
inline constexpr uint32_t R_PPC_GOT16_HA      = 17;
inline constexpr uint32_t R_PPC_PLTREL24      = 18;
inline constexpr uint32_t R_PPC_COPY          = 19;
inline constexpr uint32_t R_PPC_GLOB_DAT      = 20;
inline constexpr uint32_t R_PPC_JMP_SLOT      = 21;
inline constexpr uint32_t R_PPC_RELATIVE      = 22;
inline constexpr uint32_t R_PPC_LOCAL24PC     = 23;
inline constexpr uint32_t R_PPC_UADDR32       = 24;
inline constexpr uint32_t R_PPC_UADDR16       = 25;
inline constexpr uint32_t R_PPC_REL32         = 26;
inline constexpr uint32_t R_PPC_PLT32         = 27;
inline constexpr uint32_t R_PPC_PLTREL32      = 28;
inline constexpr uint32_t R_PPC_PLT16_LO      = 29;
inline constexpr uint32_t R_PPC_PLT16_HI      = 30;
inline constexpr uint32_t R_PPC_PLT16_HA      = 31;
inline constexpr uint32_t R_PPC_SDAREL16      = 32;
inline constexpr uint32_t R_PPC_SECTOFF       = 33;
inline constexpr uint32_t R_PPC_SECTOFF_LO    = 34;
inline constexpr uint32_t R_PPC_SECTOFF_HI    = 35;
inline constexpr uint32_t R_PPC_SECTOFF_HA    = 36;
inline constexpr uint32_t R_PPC_ADDR30        = 37;
// Wii U / Cafe OS specific
inline constexpr uint32_t R_PPC_EMB_SDA21     = 109; // 21-bit small-data relative
inline constexpr uint32_t R_PPC_EMB_RELSDA    = 116;
// GCC extensions
inline constexpr uint32_t R_PPC_DIAB_SDA21_LO = 180;
inline constexpr uint32_t R_PPC_DIAB_SDA21_HI = 181;
inline constexpr uint32_t R_PPC_DIAB_SDA21_HA = 182;
inline constexpr uint32_t R_PPC_DIAB_RELSDA_LO= 183;
inline constexpr uint32_t R_PPC_DIAB_RELSDA_HI= 184;
inline constexpr uint32_t R_PPC_DIAB_RELSDA_HA= 185;

/// Returns a human-readable name for a relocation type.
const char* reloc_type_name(uint32_t type) noexcept;

// ============================================================================
// RelocResult — outcome of applying a single relocation
// ============================================================================

enum class RelocStatus {
    Ok,
    UnknownType,
    UnresolvedSymbol,
    OutOfRange,
    UnalignedTarget,
    WriteError,
};

struct RelocResult {
    RelocStatus status{RelocStatus::Ok};
    std::string message{};

    bool ok() const noexcept { return status == RelocStatus::Ok; }
};

// ============================================================================
// SymbolResolver — callback to look up a symbol's virtual address
// ============================================================================

/// Returns the virtual address of a named symbol, or nullopt if not found.
using SymbolResolver =
    std::function<std::optional<uint32_t>(const std::string& module,
                                          const std::string& name)>;

// ============================================================================
// RelocationProcessor
// ============================================================================

/// Applies ELF RELA relocations to a loaded RPX/RPL image in-memory.
///
/// Wii U relocations are always RELA (explicit addend).  The processor
/// iterates over all SHT_RELA sections, resolves symbols via the provided
/// callback, and patches section data in place.
class RelocationProcessor {
public:
    struct Config {
        bool   ignore_unknown_types;
        bool   ignore_unresolved_symbols;
        bool   verbose;
        Config() noexcept
            : ignore_unknown_types(false)
            , ignore_unresolved_symbols(false)
            , verbose(false) {}
    };

    RelocationProcessor(diagnostics::DiagEngine& diag,
                        SymbolResolver resolver,
                        Config cfg = Config{}) noexcept;

    /// Process all relocations in the given RPX module.
    /// Patches section data in place.  Returns false on fatal errors.
    bool process(rpx::RpxModule& module);

    /// Process a single relocation entry against a mutable section data buffer.
    RelocResult apply(uint32_t type,
                      uint32_t offset,
                      uint32_t sym_value,
                      int32_t  addend,
                      std::vector<uint8_t>& section_data,
                      uint32_t section_base_addr);

    struct Stats {
        uint32_t applied{0};
        uint32_t skipped_unknown{0};
        uint32_t skipped_unresolved{0};
        uint32_t errors{0};
    };

    Stats stats() const noexcept { return m_stats; }

private:
    void patch_be32(std::vector<uint8_t>& data, uint32_t offset, uint32_t val);
    void patch_be16(std::vector<uint8_t>& data, uint32_t offset, uint16_t val);

    diagnostics::DiagEngine& m_diag;
    SymbolResolver m_resolver;
    Config m_cfg;
    Stats  m_stats{};
};

}