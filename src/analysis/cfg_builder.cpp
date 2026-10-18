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

    auto leaders = find_leaders(entry_addr, text_end, func);
    if (leaders.empty()) {
        m_last_error = "no leaders found";
        return {};
    }

    build_blocks(func, leaders, text_end);
    resolve_edges(func);

    // build_blocks marks the block at entry_addr. The lowest-address block may
    // instead be a backward shared body; do not overwrite the entry's mapping.

    return func;
}

CFGBuilder::InlineTableAudit CFGBuilder::audit_inline_branch(uint32_t pc) const {
    using ppc::Mnemonic;
    if (pc < 32) return {{},"insufficient_prefix"};
    auto instruction = [&](uint32_t a) -> std::optional<ppc::Instruction> {
        auto word = read_word(a);
        return word ? ppc::decode(*word, a) : std::nullopt;
    };
    // Bounded inline code table: unsigned index guard, scale by four, form
    // table address, MTCTR/BCTR. Refuse unbounded or ambiguous patterns.
    auto guard = instruction(pc - 20), scale = instruction(pc - 16);
    auto high = instruction(pc - 12), low = instruction(pc - 8);
    auto mtctr = instruction(pc - 4), branch = instruction(pc);
    if (!guard || !scale || !high || !low || !mtctr || !branch) return {{},"decode_or_prefix_missing"};
    if (branch->mnemonic != Mnemonic::BCTR || mtctr->mnemonic != Mnemonic::MTCTR ||
        scale->mnemonic != Mnemonic::RLWINM || scale->rA != scale->rD ||
        scale->sh != 2 || scale->mb != 0 || scale->me != 29) return {{},"scale_or_ctr_producer"};
    uint32_t compare_distance=24, producer_start=pc-16;
    bool combined=high->mnemonic==Mnemonic::ADDIS && high->rA==scale->rA &&
                  low->mnemonic==Mnemonic::ADDI && low->rD==high->rD && low->rA==high->rD &&
                  mtctr->rD==high->rD;
    if (!combined) {
        // Alternative compiler producer: load a separate table constant,
        // scale the guarded index, then ADD them before MTCTR. ADDIC changes
        // carry but computes the same base constant; it does not consume CA.
        guard=instruction(pc-24);high=instruction(pc-20);low=instruction(pc-12);
        auto sum=instruction(pc-8);
        if (!guard || !high || !low || !sum || high->mnemonic!=Mnemonic::LIS ||
            high->rD==scale->rA ||
            (low->mnemonic!=Mnemonic::ADDI && low->mnemonic!=Mnemonic::ADDIC) ||
            low->rD!=high->rD || low->rA!=high->rD ||
            (low->mnemonic==Mnemonic::ADDI && low->rA==0) ||
            sum->mnemonic!=Mnemonic::ADD || mtctr->rD!=sum->rD ||
            !((sum->rA==scale->rA && sum->rB==high->rD) ||
              (sum->rB==scale->rA && sum->rA==high->rD))) return {{},"separate_base_producer"};
        compare_distance=28;producer_start=pc-20;
    }
    bool return_guard=guard->mnemonic==Mnemonic::BCLR;
    if ((!return_guard && guard->mnemonic!=Mnemonic::BC) ||
        (guard->bo&~1u)!=12 || guard->lk) return {{},"guard_condition"};
    uint32_t table = (static_cast<uint32_t>(high->imm) << 16) + static_cast<uint32_t>(low->imm);
    if (table != pc + 4 || scale->rA == 0) return {{},"table_base_or_index"};

    // Allow a few non-record register/immediate moves or ordinary stores
    // between compare and guard. STW/STH/STB only read GPRs; updating stores, index
    // or CR writes, calls, and other control flow remain unproved.
    std::optional<ppc::Instruction> compare;
    for (uint32_t distance = compare_distance; distance <= compare_distance+16; distance += 4) {
        auto candidate = instruction(pc - distance);
        if (!candidate) return {{},"guard_intervening_decode"};
        if (candidate->mnemonic == Mnemonic::CMPLI) { compare = candidate; break; }
        if (candidate->mnemonic == Mnemonic::STW || candidate->mnemonic == Mnemonic::STH ||
            candidate->mnemonic == Mnemonic::STB) continue;
        // Non-updating integer loads only write RT and leave the compared
        // selector and CR intact when that destination is independent.
        if ((candidate->mnemonic == Mnemonic::LWZ || candidate->mnemonic == Mnemonic::LBZ ||
             candidate->mnemonic == Mnemonic::LHZ || candidate->mnemonic == Mnemonic::LHA) &&
            candidate->rD != scale->rA) continue;
        // Non-record FP arithmetic and non-updating FP loads write only an
        // FPR: neither the selector GPR nor the guard's CR field changes.
        if (!candidate->rc) {
            switch (candidate->mnemonic) {
            case Mnemonic::FADD: case Mnemonic::FADDS: case Mnemonic::FSUB: case Mnemonic::FSUBS:
            case Mnemonic::FMUL: case Mnemonic::FMULS: case Mnemonic::FDIV: case Mnemonic::FDIVS:
            case Mnemonic::FMADD: case Mnemonic::FMADDS: case Mnemonic::FMSUB: case Mnemonic::FMSUBS:
            case Mnemonic::FNMADD: case Mnemonic::FNMADDS: case Mnemonic::FNMSUB: case Mnemonic::FNMSUBS:
            case Mnemonic::FMR: case Mnemonic::FNEG: case Mnemonic::FABS: case Mnemonic::FRSP:
            case Mnemonic::LFS: case Mnemonic::LFD:
                continue;
            default: break;
            }
        }
        // MR is an OR alias: decoded rA is the destination, rD the source.
        // Reading the selector is safe; overwriting it invalidates the bound.
        if (candidate->mnemonic == Mnemonic::MR && candidate->rA != scale->rA) continue;
        // Non-record rotate/mask writes RA, not RS (decoded rD). Compilers
        // schedule independent shifts between an unsigned bound and its
        // branch; reading the selector is harmless, replacing it is not.
        if (candidate->mnemonic == Mnemonic::RLWINM && !candidate->rc &&
            candidate->rA != scale->rA) continue;
        // Non-record X-form integer arithmetic writes only rD (and XER.CA),
        // never CR: harmless unless it overwrites the selector (Gambit
        // 029926C0: subfe r3,r12,r0 between cmplwi r31 and bgt).
        if ((candidate->mnemonic == Mnemonic::SUBF || candidate->mnemonic == Mnemonic::SUBFC ||
             candidate->mnemonic == Mnemonic::SUBFE || candidate->mnemonic == Mnemonic::ADD ||
             candidate->mnemonic == Mnemonic::ADDE) && candidate->rD != scale->rA) continue;
        if ((candidate->mnemonic != Mnemonic::LI && candidate->mnemonic != Mnemonic::LIS &&
             candidate->mnemonic != Mnemonic::ADDI && candidate->mnemonic != Mnemonic::ADDIS) ||
            candidate->rD == scale->rA) return {{},"guard_intervening_effect"};
    }
    if (!compare || compare->rA != scale->rA || (compare->rD & 3u) != 0 ||
        guard->bi != (compare->rD >> 2) * 4u + 1u || compare->uimm >= 256) return {{},"compare_bound_or_cr"};
    uint32_t count = compare->uimm + 1;
    // An unsigned greater-than LR return also excludes every out-of-range
    // selector. It has no local default target; preserve the actual BCLR.
    if (!return_guard && (!is_code_addr(guard->target) ||
        (guard->target >= producer_start && guard->target < table + count * 4))) return {{},"default_target"};
    std::vector<uint32_t> targets;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t entry = table + i * 4;
        auto stub = instruction(entry);
        if (!is_code_addr(entry) || !stub ||
            (stub->mnemonic != Mnemonic::B && stub->mnemonic != Mnemonic::BA) ||
            !is_code_addr(stub->target)) return {{},"table_slot_not_branch"};
        targets.push_back(entry);
    }
    return {std::move(targets),"accepted"};
}

std::vector<uint32_t> CFGBuilder::inline_branch_targets(uint32_t pc) const {
    return audit_inline_branch(pc).targets;
}

std::set<uint32_t>
CFGBuilder::find_leaders(uint32_t entry, uint32_t text_end, ir::IRFunction& func) {
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
            if (insn->mnemonic == ppc::Mnemonic::BCTR) {
                auto targets = inline_branch_targets(pc);
                if (!targets.empty()) {
                    func.indirect_jump_targets[pc] = targets;
                    for (uint32_t target : targets) enqueue(target);
                }
                break;
            }
            if (insn->is_return()) {
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
        } else if (last.opcode == ir::Opcode::IndirectJump) {
            auto targets = func.indirect_jump_targets.find(last.guest_addr);
            if (targets != func.indirect_jump_targets.end())
                for (uint32_t target : targets->second) connect(blk, func.block_at_addr(target));
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
