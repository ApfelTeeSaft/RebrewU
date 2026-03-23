#include "jump_table.hpp"
#include "../ppc/decoder/ppc_decode.hpp"
#include <algorithm>

namespace rebrewu::analysis {

JumpTableAnalyzer::JumpTableAnalyzer(const rpx::RpxModule& module)
    : m_module(module) {}

std::vector<JumpTable>
JumpTableAnalyzer::analyze(const ir::IRFunction& func) {
    std::vector<JumpTable> result;
    for (const auto& blk : func.blocks) {
        if (blk.instrs.empty()) continue;
        const auto& last = blk.instrs.back();
        if (last.opcode == ir::Opcode::IndirectJump) {
            uint32_t bctr_addr = blk.guest_end > 0 ? blk.guest_end - 4 : 0;
            auto jt = try_resolve(bctr_addr, func);
            if (jt.has_value())
                result.push_back(std::move(*jt));
        }
    }
    return result;
}

std::optional<JumpTable>
JumpTableAnalyzer::try_resolve(uint32_t indirect_branch_addr,
                                const ir::IRFunction& /*func*/) {
    JumpTable jt{};
    if (scan_pattern(indirect_branch_addr, jt))
        return jt;
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// scan_pattern
//
// Recognise the classic Wii U (PPC Espresso) switch-dispatch sequence:
//
//   slwi   r<idx>, r<idx>, 2        ; index *= 4
//   lis    r<base>, hi(table)       ; load table address (2-insn pair)
//   addi   r<base>, r<base>, lo(table)
//   lwzx   r<target>, r<base>, r<idx>  ; load entry from table
//   mtctr  r<target>                ; move to CTR
//   bctr                            ; indirect jump
//
// We scan backwards from the bctr for up to 32 instructions.
// ---------------------------------------------------------------------------

bool JumpTableAnalyzer::scan_pattern(uint32_t bctr_addr, JumpTable& out) {
    if (bctr_addr == 0) return false;

    // Decode up to 32 instructions preceding (and including) the bctr.
    constexpr int kWindow = 32;
    uint32_t scan_start = (bctr_addr >= static_cast<uint32_t>(kWindow * 4))
                          ? (bctr_addr - kWindow * 4) : 0;

    struct DecodedInsn {
        uint32_t addr;
        ppc::Instruction insn;
    };
    std::vector<DecodedInsn> window;
    window.reserve(kWindow + 1);

    for (uint32_t pc = scan_start; pc <= bctr_addr; pc += 4) {
        auto w = read_word(pc);
        if (!w) continue;
        auto di = ppc::decode(*w, pc);
        if (!di) continue;
        window.push_back({pc, *di});
    }

    if (window.empty()) return false;

    // ---- State we are trying to fill ----
    uint32_t table_base = 0;
    uint32_t index_reg  = 0xFF;
    uint32_t base_reg   = 0xFF;
    uint32_t target_reg = 0xFF; // register loaded from table
    bool found_mtctr    = false;
    bool found_lwzx     = false;
    bool found_table    = false;

    // Walk backwards from bctr
    for (int i = static_cast<int>(window.size()) - 1; i >= 0; --i) {
        const auto& di = window[i].insn;
        const uint32_t addr = window[i].addr;
        (void)addr;

        // bctr itself — skip
        if (di.mnemonic == ppc::Mnemonic::BCTR) continue;

        // mtctr rX  →  target_reg = rX
        if (di.mnemonic == ppc::Mnemonic::MTCTR && !found_mtctr) {
            target_reg  = di.rD; // rD holds the source in MTCTR encoding
            found_mtctr = true;
            continue;
        }

        // lwzx rTarget, rBase, rIndex  → load from table
        if (di.mnemonic == ppc::Mnemonic::LWZX && !found_lwzx && found_mtctr) {
            if (di.rD == target_reg) {
                base_reg  = di.rA;
                index_reg = di.rB;
                found_lwzx = true;
                continue;
            }
        }

        // lis rBase, hi  →  start of table address materialisation
        if ((di.mnemonic == ppc::Mnemonic::LIS ||
             di.mnemonic == ppc::Mnemonic::ADDIS) &&
            found_lwzx && !found_table) {
            if (di.rD == base_reg) {
                uint32_t hi_part = static_cast<uint32_t>(di.imm) << 16;

                // Look forward for addi rBase, rBase, lo
                for (int j = i + 1; j < static_cast<int>(window.size()); ++j) {
                    const auto& adj = window[j].insn;
                    if ((adj.mnemonic == ppc::Mnemonic::ADDI ||
                         adj.mnemonic == ppc::Mnemonic::ORI) &&
                        adj.rD == base_reg && adj.rA == base_reg) {
                        uint32_t lo_part = static_cast<uint32_t>(adj.imm);
                        table_base = hi_part + lo_part;
                        found_table = true;
                        break;
                    }
                    // If something unrelated writes base_reg, stop
                    if (adj.rD == base_reg) break;
                }
                if (found_table) break;
            }
        }
    }

    if (!found_mtctr || !found_lwzx || !found_table) return false;
    if (!is_rodata_addr(table_base)) return false;

    // ---- Read table entries ----
    // Entries are 4-byte absolute guest addresses in big-endian.
    // Count entries until we hit something that doesn't look like a code address.
    std::vector<JumpTableEntry> entries;
    for (uint32_t off = 0; ; off += 4) {
        auto w = read_word(table_base + off);
        if (!w) break;
        uint32_t target = *w;
        if (!m_module.is_code_addr(target)) break;
        entries.push_back({off, target});
        // Sanity cap
        if (entries.size() >= 512) break;
    }

    if (entries.empty()) return false;

    out.dispatch_addr = bctr_addr;
    out.table_addr    = table_base;
    out.num_entries   = static_cast<uint32_t>(entries.size());
    out.entries       = std::move(entries);
    out.index_reg     = index_reg;
    out.base_reg      = base_reg;
    return true;
}


std::optional<uint32_t> JumpTableAnalyzer::read_word(uint32_t addr) const {
    return m_module.read_word(addr);
}

bool JumpTableAnalyzer::is_rodata_addr(uint32_t addr) const {
    const auto* sec = m_module.section_at_addr(addr);
    return sec && (sec->name == ".rodata" || sec->name == ".data");
}

}