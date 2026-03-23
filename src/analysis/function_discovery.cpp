#include "function_discovery.hpp"
#include "../ppc/decoder/ppc_decode.hpp"

namespace rebrewu::analysis {

FunctionDiscovery::FunctionDiscovery(const rpx::RpxModule& module,
                                     const linker::Linker& linker,
                                     Config cfg)
    : m_module(module), m_linker(linker), m_cfg(std::move(cfg)) {}

void FunctionDiscovery::add_hint(FunctionHint hint) {
    add_candidate(hint.address, hint.name, hint.force);
}

void FunctionDiscovery::add_candidate(uint32_t addr, std::string name, bool force) {
    if (!force && !is_valid_code_addr(addr)) return;
    m_candidates.insert(addr);
    if (!name.empty() && m_names.find(addr) == m_names.end())
        m_names[addr] = std::move(name);
}

bool FunctionDiscovery::is_valid_code_addr(uint32_t addr) const {
    return m_module.is_code_addr(addr) && (addr & 3) == 0;
}

std::vector<FunctionBoundary> FunctionDiscovery::discover() {
    // always seed from the RPX entry point (_start)
    if (m_module.entry_point != 0)
        add_candidate(m_module.entry_point, "_start");

    // seed from exports
    if (m_cfg.analyze_exports) {
        for (const auto& exp : m_module.exports) {
            if (!exp.is_data) {
                add_candidate(exp.address, exp.name);
                ++m_stats.from_exports;
            }
        }
    }

    // seed from symbol table
    if (m_cfg.analyze_symbol_table) {
        for (const auto& sym : m_module.symbols) {
            if (sym.is_function() && sym.address != 0) {
                add_candidate(sym.address, sym.name);
                ++m_stats.from_symbols;
            }
        }
    }

    // Seed from user hints
    for (const auto& hint : m_cfg.hints) {
        add_candidate(hint.address, hint.name, hint.force);
        ++m_stats.from_hints;
    }

    //     Prologue scan: scan executable sections for stwu r1, -N(r1)
    //     (0x9421xxxx with negative displacement, i.e. bit 15 set).
    //     Always run — the binary is typically stripped so prologues are the
    //     most reliable function-start detector regardless of other seeds.
    scan_prologues();

    //     Data-pointer scan: scan non-executable allocated sections (.rodata,
    //     .data) for 4-byte values that are valid code addresses.  Leaf
    //     functions (e.g. GHS static constructor entries) are only reachable
    //     via function-pointer tables in .rodata and have no stwu prologue,
    //     so neither the prologue scan nor call-following can find them.
    scan_data_pointers();

    //     Follow calls from seeds — iterate until no new candidates are found
    if (m_cfg.follow_calls) {
        std::set<uint32_t> scanned;
        bool found_new = true;
        while (found_new) {
            found_new = false;
            std::vector<uint32_t> to_scan;
            for (uint32_t addr : m_candidates) {
                if (scanned.find(addr) == scanned.end()) {
                    to_scan.push_back(addr);
                    scanned.insert(addr);
                    found_new = true;
                }
            }
            for (uint32_t entry : to_scan) {
                const auto* sec = m_module.section_at_addr(entry);
                if (!sec || !sec->is_executable()) continue;
                uint32_t end = sec->address + sec->size;
                scan_calls(entry, end);
            }
        }
    }

    //    Build FunctionBoundary list
    std::vector<FunctionBoundary> result;
    result.reserve(m_candidates.size());
    for (uint32_t addr : m_candidates) {
        FunctionBoundary fb;
        fb.start = addr;
        fb.end   = 0;
        auto it = m_names.find(addr);
        if (it != m_names.end()) fb.name = it->second;
        result.push_back(fb);
    }

    m_stats.total = static_cast<uint32_t>(result.size());
    return result;
}

void FunctionDiscovery::scan_prologues() {
    // Scan all executable sections for two kinds of function-start evidence:
    //
    //    stwu r1, -N(r1)  (canonical PPC Espresso prologue)
    //    word & 0xFFFF8000 == 0x94218000
    //
    //    BL / BLA call targets — any address directly called via bl/bla is
    //    a function entry, even if it lacks a stack frame (leaf functions,
    //    C++ special functions, compiler-generated glue, etc.)
    //    PPC BL encoding: opcode=18 (bits 26-31), LK=1 (bit 0)
    //    word & 0xFC000003 == 0x48000001  (BL, PC-relative)
    //    word & 0xFC000003 == 0x48000003  (BLA, absolute)

    for (const auto& sec : m_module.sections) {
        if (!sec.is_executable() || sec.data.empty()) continue;
        const uint32_t base       = sec.address;
        const uint32_t byte_count = static_cast<uint32_t>(sec.data.size());

        for (uint32_t off = 0; off + 4 <= byte_count; off += 4) {
            const uint32_t word =
                (static_cast<uint32_t>(sec.data[off    ]) << 24) |
                (static_cast<uint32_t>(sec.data[off + 1]) << 16) |
                (static_cast<uint32_t>(sec.data[off + 2]) <<  8) |
                 static_cast<uint32_t>(sec.data[off + 3]);

            // --- prologue pattern ---
            if ((word & 0xFFFF8000u) == 0x94218000u) {
                const uint32_t addr = base + off;
                if (is_valid_code_addr(addr)) {
                    add_candidate(addr, {});
                    ++m_stats.from_prologues;
                }
                continue;
            }

            // --- BL / BLA target ---
            const uint32_t opcode = word >> 26;
            const bool lk = (word & 1u) != 0;
            if (opcode == 18 && lk) {
                const bool aa = (word & 2u) != 0;
                int32_t li    = static_cast<int32_t>((word >> 2) & 0xFFFFFF) << 8 >> 8; // sign-extend 24→32
                const uint32_t target = aa
                    ? static_cast<uint32_t>(li << 2)
                    : (base + off) + static_cast<uint32_t>(li << 2);
                if (is_valid_code_addr(target) &&
                        m_candidates.find(target) == m_candidates.end()) {
                    add_candidate(target, {});
                    ++m_stats.from_prologues;
                }
            }
        }
    }
}

void FunctionDiscovery::scan_data_pointers() {
    // Scan all non-executable, allocated sections for big-endian 32-bit words
    // that resolve to valid code addresses (4-byte aligned, inside a .text
    // section).  These represent function pointers stored in .rodata/.data —
    // most commonly static-constructor tables that have no BL call site and
    // whose target functions have no stwu prologue (leaf functions).
    //
    // PPC mflr/stwu split-prolog fixup:
    // Compilers sometimes emit:
    //   offset+0:  mflr r0        (7C0802A6)   ← real function entry in vtable
    //   offset+4:  stwu r1,-N(r1) (9421xxxx)
    // But vtable pointers sometimes point to offset+4 (stwu).  When we see a
    // data pointer to a stwu and the instruction at ptr-4 is mflr r0, add
    // ptr-4 as the actual function entry too.
    static constexpr uint32_t MFLR_R0  = 0x7C0802A6u;   // mflr r0
    static constexpr uint32_t STWU_MASK= 0xFFFF8000u;
    static constexpr uint32_t STWU_PAT = 0x94218000u;    // stwu r1,-N(r1)

    auto maybe_add_prev = [&](uint32_t val) {
        if (val < 4) return;
        uint32_t prev = val - 4;
        if (!is_valid_code_addr(prev)) return;
        auto w = m_module.read_word(val);
        auto w_prev = m_module.read_word(prev);
        if (!w || !w_prev) return;
        // If ptr points to stwu and ptr-4 is mflr r0, use ptr-4
        if ((*w & STWU_MASK) == STWU_PAT && *w_prev == MFLR_R0) {
            if (m_candidates.find(prev) == m_candidates.end()) {
                add_candidate(prev, {});
                ++m_stats.from_data_ptrs;
            }
        }
    };

    for (const auto& sec : m_module.sections) {
        if (sec.is_executable() || !sec.is_allocated() || sec.data.empty()) continue;
        const uint32_t byte_count = static_cast<uint32_t>(sec.data.size());
        for (uint32_t off = 0; off + 4 <= byte_count; off += 4) {
            const uint32_t val =
                (static_cast<uint32_t>(sec.data[off    ]) << 24) |
                (static_cast<uint32_t>(sec.data[off + 1]) << 16) |
                (static_cast<uint32_t>(sec.data[off + 2]) <<  8) |
                 static_cast<uint32_t>(sec.data[off + 3]);
            if ((val & 3u) != 0) continue;  // must be 4-byte aligned
            if (!is_valid_code_addr(val)) continue;
            if (m_candidates.find(val) == m_candidates.end()) {
                add_candidate(val, {});
                ++m_stats.from_data_ptrs;
            }
            // Also check if ptr-4 is the real entry (mflr r0 before stwu)
            maybe_add_prev(val);
        }
    }
}

void FunctionDiscovery::scan_calls(uint32_t start, uint32_t end) {
    for (uint32_t pc = start; pc < end && pc + 4 <= end; pc += 4) {
        auto word_opt = m_module.read_word(pc);
        if (!word_opt) break;
        auto insn = ppc::decode(*word_opt, pc);
        if (!insn) continue;

        // Direct calls (bl / bla)
        if (insn->is_call() && !insn->is_indirect_branch()) {
            uint32_t target = insn->target;
            if (is_valid_code_addr(target) &&
                m_candidates.find(target) == m_candidates.end()) {
                add_candidate(target, {});
                ++m_stats.from_call_targets;
            }
        }

        // Function terminators
        if (insn->is_return()) break;
        if (insn->mnemonic == ppc::Mnemonic::B ||
            insn->mnemonic == ppc::Mnemonic::BA) {
            // unconditional non-call branch — may be tail call
            if (!insn->lk) break;
        }
    }
}

}