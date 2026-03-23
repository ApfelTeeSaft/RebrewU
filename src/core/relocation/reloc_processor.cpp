#include "reloc_processor.hpp"
#include <cstring>
#include <cstdio>

namespace rebrewu::reloc {

// ============================================================================
// reloc_type_name
// ============================================================================

const char* reloc_type_name(uint32_t type) noexcept {
    switch (type) {
    case R_PPC_NONE:           return "R_PPC_NONE";
    case R_PPC_ADDR32:         return "R_PPC_ADDR32";
    case R_PPC_ADDR24:         return "R_PPC_ADDR24";
    case R_PPC_ADDR16:         return "R_PPC_ADDR16";
    case R_PPC_ADDR16_LO:      return "R_PPC_ADDR16_LO";
    case R_PPC_ADDR16_HI:      return "R_PPC_ADDR16_HI";
    case R_PPC_ADDR16_HA:      return "R_PPC_ADDR16_HA";
    case R_PPC_REL24:          return "R_PPC_REL24";
    case R_PPC_REL14:          return "R_PPC_REL14";
    case R_PPC_REL32:          return "R_PPC_REL32";
    case R_PPC_RELATIVE:       return "R_PPC_RELATIVE";
    case R_PPC_JMP_SLOT:       return "R_PPC_JMP_SLOT";
    case R_PPC_GLOB_DAT:       return "R_PPC_GLOB_DAT";
    case R_PPC_EMB_SDA21:      return "R_PPC_EMB_SDA21";
    case R_PPC_EMB_RELSDA:     return "R_PPC_EMB_RELSDA";
    default:                   return "R_PPC_UNKNOWN";
    }
}

// ============================================================================
// Helpers
// ============================================================================

static uint32_t read_be32(const std::vector<uint8_t>& d, uint32_t off) {
    return (uint32_t(d[off]) << 24) | (uint32_t(d[off+1]) << 16) |
           (uint32_t(d[off+2]) << 8) | uint32_t(d[off+3]);
}

// ============================================================================
// RelocationProcessor
// ============================================================================

RelocationProcessor::RelocationProcessor(diagnostics::DiagEngine& diag,
                                         SymbolResolver resolver,
                                         Config cfg) noexcept
    : m_diag(diag), m_resolver(std::move(resolver)), m_cfg(cfg) {}

bool RelocationProcessor::process(rpx::RpxModule& module) {
    bool ok = true;
    for (auto& reloc : module.relocations) {
        // Resolve symbol value
        uint32_t sym_val = 0;
        if (reloc.sym_index < module.symbols.size()) {
            sym_val = module.symbols[reloc.sym_index].address;
        } else if (reloc.sym_name && m_resolver) {
            auto resolved = m_resolver("", *reloc.sym_name);
            if (resolved) {
                sym_val = *resolved;
            } else if (!m_cfg.ignore_unresolved_symbols) {
                m_diag.error(std::string("unresolved symbol: ") + reloc.sym_name->data());
                ok = false;
                ++m_stats.skipped_unresolved;
                continue;
            } else {
                ++m_stats.skipped_unresolved;
                continue;
            }
        }

        // Find the section containing the relocation target address
        rpx::RpxSection* target_sec = nullptr;
        for (auto& sec : module.sections) {
            if (sec.contains(reloc.offset)) {
                target_sec = &sec;
                break;
            }
        }
        if (!target_sec) {
            if (!m_cfg.ignore_unknown_types) {
                char buf[12];
                std::snprintf(buf, sizeof(buf), "0x%08X", reloc.offset);
                m_diag.warning(std::string("relocation at ") + buf + " targets unmapped address");
            }
            continue;
        }

        auto result = apply(reloc.type, reloc.offset, sym_val, reloc.addend,
                            target_sec->data, target_sec->address);
        if (!result.ok()) {
            if (result.status == RelocStatus::UnknownType) {
                ++m_stats.skipped_unknown;
                if (!m_cfg.ignore_unknown_types)
                    m_diag.warning(result.message);
            } else {
                ++m_stats.errors;
                m_diag.error(result.message);
                ok = false;
            }
        } else {
            ++m_stats.applied;
        }
    }
    return ok;
}

RelocResult RelocationProcessor::apply(uint32_t type,
                                        uint32_t offset,
                                        uint32_t sym_val,
                                        int32_t  addend,
                                        std::vector<uint8_t>& data,
                                        uint32_t base_addr) {
    uint32_t local_off = offset - base_addr;
    if (local_off + 4 > data.size())
        return {RelocStatus::WriteError, "relocation offset out of section bounds"};

    uint32_t S = sym_val;
    int32_t  A = addend;
    uint32_t P = offset;

    switch (type) {
    case R_PPC_NONE:
        return {};

    case R_PPC_ADDR32:
        patch_be32(data, local_off, S + A);
        return {};

    case R_PPC_ADDR16_LO: {
        uint16_t val = static_cast<uint16_t>((S + A) & 0xFFFF);
        patch_be16(data, local_off + 2, val);
        return {};
    }

    case R_PPC_ADDR16_HI: {
        uint16_t val = static_cast<uint16_t>((S + A) >> 16);
        patch_be16(data, local_off + 2, val);
        return {};
    }

    case R_PPC_ADDR16_HA: {
        uint32_t val = S + A;
        uint16_t hi  = static_cast<uint16_t>(((val + 0x8000) >> 16) & 0xFFFF);
        patch_be16(data, local_off + 2, hi);
        return {};
    }

    case R_PPC_REL24: {
        int32_t disp = static_cast<int32_t>(S + A - P);
        if (disp < -(1 << 25) || disp > ((1 << 25) - 1))
            return {RelocStatus::OutOfRange, "R_PPC_REL24 displacement out of range"};
        uint32_t instr = read_be32(data, local_off);
        instr = (instr & 0xFC000003u) | (static_cast<uint32_t>(disp) & 0x03FFFFFC);
        patch_be32(data, local_off, instr);
        return {};
    }

    case R_PPC_REL32:
        patch_be32(data, local_off, static_cast<uint32_t>(S + A - P));
        return {};

    case R_PPC_RELATIVE:
        patch_be32(data, local_off, A);
        return {};

    default:
        return {RelocStatus::UnknownType,
                std::string("unsupported relocation type ") + reloc_type_name(type)};
    }
}

void RelocationProcessor::patch_be32(std::vector<uint8_t>& data,
                                      uint32_t off, uint32_t val) {
    data[off]   = static_cast<uint8_t>(val >> 24);
    data[off+1] = static_cast<uint8_t>(val >> 16);
    data[off+2] = static_cast<uint8_t>(val >>  8);
    data[off+3] = static_cast<uint8_t>(val);
}

void RelocationProcessor::patch_be16(std::vector<uint8_t>& data,
                                      uint32_t off, uint16_t val) {
    data[off]   = static_cast<uint8_t>(val >> 8);
    data[off+1] = static_cast<uint8_t>(val);
}

} // namespace rebrewu::reloc
