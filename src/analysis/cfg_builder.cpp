#include "cfg_builder.hpp"
#include "../ppc/semantics/ppc_semantics.hpp"
#include <cstdio>

namespace rebrewu::analysis {

CFGBuilder::CFGBuilder(const rpx::RpxModule& module, Config cfg)
    : m_module(module), m_cfg(std::move(cfg)) {}

std::optional<ir::IRFunction>
CFGBuilder::build(uint32_t entry_addr, std::string_view name) {
    if (!is_code_addr(entry_addr)) {
        m_last_error = "entry address is not in a code section";
        return {};
    }

    const auto* sec = m_module.section_at_addr(entry_addr);
    uint32_t text_end = sec ? sec->address + sec->size : entry_addr + 4;

    ir::IRFunction func;
    func.name       = name.empty() ? "fn_" + [&]{
        char buf[10]; std::snprintf(buf, sizeof(buf), "%08X", entry_addr); return std::string(buf);
    }() : std::string(name);
    func.entry_addr = entry_addr;

    auto leaders = find_leaders(entry_addr, text_end);
    if (leaders.empty()) {
        m_last_error = "no leaders found";
        return {};
    }

    build_blocks(func, leaders, text_end);
    resolve_edges(func);

    if (!func.empty() && !func.blocks.front().is_entry) {
        func.blocks.front().is_entry = true;
        func.addr_to_block[entry_addr] = func.blocks.front().id;
    }

    return func;
}

std::set<uint32_t>
CFGBuilder::find_leaders(uint32_t entry, uint32_t text_end) {
    // Worklist-based exploration: follows intra-function control flow only.
    // This keeps the leader set bounded to the current function rather than
    // scanning the entire text section like a linear sweep would.

    std::set<uint32_t> leaders;
    std::set<uint32_t> visited_starts; // block start addresses already enqueued
    std::vector<uint32_t> worklist;
    uint32_t total_instrs = 0;

    auto enqueue = [&](uint32_t addr) {
        if (addr == 0 || !is_code_addr(addr)) return;
        if (visited_starts.insert(addr).second) {
            leaders.insert(addr);
            worklist.push_back(addr);
        }
    };

    enqueue(entry);

    while (!worklist.empty()) {
        uint32_t pc = worklist.back();
        worklist.pop_back();

        // Linear scan from pc until a block terminator
        while (pc < text_end && total_instrs < m_cfg.max_instructions_per_function) {
            auto word = read_word(pc);
            if (!word) break;
            auto insn = ppc::decode(*word, pc);
            if (!insn) { pc += 4; ++total_instrs; continue; }
            ++total_instrs;

            // ---- Returns / indirect jumps: end of this path ----
            // BCTRL is an indirect CALL (lk=true); execution continues at
            // PC+4, so it falls through to the is_call() handler below.
            // BCTR (no link) is a true indirect jump — end the path.
            if (insn->is_return() ||
                insn->mnemonic == ppc::Mnemonic::BCTR) {
                break;
            }

            // ---- Unconditional direct branch (B / BA, not BL / BLA) ----
            if ((insn->mnemonic == ppc::Mnemonic::B ||
                 insn->mnemonic == ppc::Mnemonic::BA) && !insn->lk) {
                enqueue(insn->target);
                break; // stop linear scan — next instruction is unreachable
            }

            // ---- Calls (BL / BLA / BCTRL already handled above): ----
            // Don't follow the callee; continue to fallthrough.
            if (insn->is_call()) {
                pc += 4;
                continue;
            }

            // ---- BCLR / BCLRL: conditional return ----
            if (insn->mnemonic == ppc::Mnemonic::BCLR ||
                insn->mnemonic == ppc::Mnemonic::BCLRL) {
                // Fallthrough path is also live
                enqueue(pc + 4);
                break;
            }

            // ---- Conditional direct branch (BC / BCL / BCA / BCLA) ----
            if (insn->is_branch() && !insn->is_indirect_branch()) {
                enqueue(pc + 4);           // fallthrough is a leader
                enqueue(insn->target);     // branch target is a leader
                break;
            }

            pc += 4;
        }
    }

    return leaders;
}

void CFGBuilder::build_blocks(ir::IRFunction& func,
                               const std::set<uint32_t>& leaders,
                               uint32_t text_end) {
    // Convert leaders set to a sorted vector for easy next-leader lookup
    std::vector<uint32_t> leader_vec(leaders.begin(), leaders.end());

    for (size_t li = 0; li < leader_vec.size(); ++li) {
        uint32_t block_start = leader_vec[li];
        if (!is_code_addr(block_start)) continue;

        // For the last leader, cap block_end at the leader's own address + a
        // small window rather than scanning to text_end.  The inner loop stops
        // at the first branch/return anyway, so the cap just prevents runaway
        // scanning if the last block has no explicit terminator.
        uint32_t block_end = (li + 1 < leader_vec.size())
                              ? leader_vec[li + 1]
                              : std::min(text_end,
                                         block_start + m_cfg.max_instructions_per_function * 4u);

        auto& blk = func.add_block(block_start);
        if (block_start == func.entry_addr) blk.is_entry = true;

        ir::IRBuilder builder(func);
        builder.set_insert_point(blk);

        for (uint32_t pc = block_start; pc < block_end; pc += 4) {
            auto word = read_word(pc);
            if (!word) break;
            auto insn = ppc::decode(*word, pc);
            if (!insn) continue;

            ppc::lower_to_ir(*insn, builder, func);

            // Calls (BL/BLA/BCTRL) are not block terminators — execution
            // continues at the fallthrough (PC+4).  Only true branches and
            // returns end a block.
            if ((insn->is_branch() && !insn->is_call()) || insn->is_return()) {
                blk.guest_end = pc + 4;
                if (insn->is_return()) blk.is_exit = true;
                break;
            }
        }
        if (blk.guest_end == 0) blk.guest_end = block_end;
    }
}

void CFGBuilder::resolve_edges(ir::IRFunction& func) {
    // Helper: resolve an operand to a target block (handles both LabelOp and
    // ImmOp guest-address targets emitted by ppc_semantics lower_branch).
    auto resolve_op = [&](const ir::IROperand& op) -> ir::BasicBlock* {
        if (const auto* la = std::get_if<ir::LabelOp>(&op))
            return func.block_by_id(la->block_id);
        if (const auto* ia = std::get_if<ir::ImmOp>(&op))
            return func.block_at_addr(static_cast<uint32_t>(ia->value));
        return nullptr;
    };

    auto connect = [&](ir::BasicBlock& src, ir::BasicBlock* tgt) {
        if (!tgt) return;
        src.successors.push_back(tgt->id);
        tgt->predecessors.push_back(src.id);
    };

    for (auto& blk : func.blocks) {
        if (blk.instrs.empty()) continue;
        const auto& last = blk.instrs.back();

        if (last.opcode == ir::Opcode::Jump) {
            if (!last.operands.empty())
                connect(blk, resolve_op(last.operands[0]));

        } else if (last.opcode == ir::Opcode::Branch) {
            // operands: cond, true_target, false_target
            if (last.operands.size() >= 3) {
                connect(blk, resolve_op(last.operands[1]));
                connect(blk, resolve_op(last.operands[2]));
            }
        } else if (last.opcode == ir::Opcode::ConditionalReturn) {
            // operands: cond, fallthrough_addr — return to LR if cond, else fallthrough
            if (last.operands.size() >= 2)
                connect(blk, resolve_op(last.operands[1]));
        }
    }
}

std::optional<uint32_t> CFGBuilder::read_word(uint32_t addr) const {
    return m_module.read_word(addr);
}

bool CFGBuilder::is_code_addr(uint32_t addr) const {
    return m_module.is_code_addr(addr);
}

}