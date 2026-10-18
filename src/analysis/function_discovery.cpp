#include "function_discovery.hpp"
#include "../ppc/decoder/ppc_decode.hpp"
#include <unordered_map>
#include <array>
#include <tuple>
#include <map>

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

    // Relocation-backed address-taken leaves need not have a prologue, data
    // pointer or direct call. Run after the existing seeds/call-following.
    scan_code_pointer_relocations();
    scan_frameless_callbacks();
    scan_address_taken_tail_callbacks();
    scan_address_taken_backward_leaf_thunks();
    scan_corroborated_entries();
    // Cross-function tail entries can complete an address-taken callback
    // whose tails were not yet callable; existing candidates are skipped.
    if (!m_cross_function_tails.empty()) scan_address_taken_tail_callbacks();

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
    using ppc::Mnemonic;
    // Reference evidence lets us recognize a prefix even without a return
    // immediately before it. This does not broaden the leaf-pointer pass.
    std::set<uint32_t> entry_references=m_candidates;
    for (const auto& relocation : m_module.relocations) {
        if (relocation.type!=elf::R_PPC_ADDR32 && relocation.type!=elf::R_PPC_ADDR16_LO &&
            relocation.type!=elf::R_PPC_ADDR16_HI && relocation.type!=elf::R_PPC_ADDR16_HA) continue;
        if (relocation.sym_index>=m_module.symbols.size()) continue;
        uint32_t value=m_module.symbols[relocation.sym_index].address+uint32_t(relocation.addend);
        if (is_valid_code_addr(value)) entry_references.insert(value);
    }
    // Local/table/tail branch destinations are not evidence for merging a
    // setup prefix. Keep this separate from independently callable seeds.
    std::set<uint32_t> local_targets;
    for (const auto& section : m_module.sections) {
        if (!section.is_executable()) continue;
        for (size_t off=0;off+4<=section.data.size();off+=4) {
            uint32_t word=(uint32_t(section.data[off])<<24)|(uint32_t(section.data[off+1])<<16)|
                          (uint32_t(section.data[off+2])<<8)|section.data[off+3];
            if ((word&1) || (word>>26!=18 && word>>26!=16)) continue;
            auto branch=ppc::decode(word,section.address+uint32_t(off));
            if (branch) local_targets.insert(branch->target);
        }
    }
    auto pre_stack_entry = [&](const rpx::RpxSection& section,uint32_t stack_pc) {
        auto instruction = [&](uint32_t pc) -> std::optional<ppc::Instruction> {
            if (pc<section.address || uint64_t(pc-section.address)+4>section.data.size()) return {};
            auto word=m_module.read_word(pc);
            return word ? ppc::decode(*word,pc) : std::nullopt;
        };
        // This family saves LR AFTER allocating the frame. Validate the save
        // and every reachable unwind, rather than shifting STWU back blindly.
        auto post_stack_entry = [&]() -> uint32_t {
            if (stack_pc<section.address+4) return stack_pc;
            uint32_t start=stack_pc-4;
            auto capture=instruction(start), allocation=instruction(stack_pc);
            if (!capture || capture->mnemonic!=Mnemonic::MFLR || capture->rD==1 || capture->rD==2 ||
                capture->rD>=13 || !allocation) return stack_pc;
            uint32_t proof_pc=start;
            auto rejected=[&](std::string reason, uint32_t pc) {
                if (m_cfg.frame_audit) m_cfg.frame_audit({start,stack_pc,pc,std::move(reason),false});
                return stack_pc;
            };
            auto previous=start>=section.address+4 ? instruction(start-4) : std::nullopt;
            if (start!=section.address && !entry_references.contains(start) &&
                (!previous || previous->mnemonic!=Mnemonic::BLR || previous->lk)) return rejected("frame_structure_or_unwind",proof_pc);
            int32_t frame=-allocation->imm;
            if (frame<8 || (frame&3)) return rejected("frame_structure_or_unwind",proof_pc);
            int32_t lr_slot=frame+4;
            std::array<int32_t,32> slots;
            slots.fill(-1);
            uint32_t saved=0, body=0;
            auto compare = [](Mnemonic mnemonic) {
                return mnemonic==Mnemonic::CMP || mnemonic==Mnemonic::CMPI ||
                       mnemonic==Mnemonic::CMPL || mnemonic==Mnemonic::CMPLI;
            };
            auto destination = [](const ppc::Instruction& insn) -> std::optional<uint8_t> {
                // Rotate and D-form logical immediates write RA (rD holds RS);
                // non-updating indexed load writes RT. ANDI./ANDIS. also write
                // CR0 and stay outside this family.
                switch (insn.mnemonic) {
                case Mnemonic::MR: case Mnemonic::MR_: case Mnemonic::RLWINM:
                case Mnemonic::ORI: case Mnemonic::ORIS: case Mnemonic::XORI: case Mnemonic::XORIS: return insn.rA;
                case Mnemonic::LI: case Mnemonic::LIS: case Mnemonic::ADDI:
                case Mnemonic::ADDIS: case Mnemonic::ADDIC: case Mnemonic::ADDIC_:
                case Mnemonic::ADD: case Mnemonic::ADD_:
                case Mnemonic::LWZ: case Mnemonic::LWZX: return insn.rD;
                default: return {};
                }
            };
            auto fp_arithmetic = [](Mnemonic m) {
                switch (m) {
                case Mnemonic::FADD: case Mnemonic::FADDS: case Mnemonic::FSUB: case Mnemonic::FSUBS:
                case Mnemonic::FMUL: case Mnemonic::FMULS: case Mnemonic::FDIV: case Mnemonic::FDIVS:
                case Mnemonic::FMADD: case Mnemonic::FMADDS: case Mnemonic::FMSUB: case Mnemonic::FMSUBS:
                case Mnemonic::FNMADD: case Mnemonic::FNMADDS: case Mnemonic::FNMSUB: case Mnemonic::FNMSUBS:
                case Mnemonic::FSEL: case Mnemonic::FRSP: case Mnemonic::FCTIW: case Mnemonic::FCTIWZ:
                case Mnemonic::FABS: case Mnemonic::FABS_: case Mnemonic::FNABS: case Mnemonic::FNABS_:
                case Mnemonic::FNEG: case Mnemonic::FNEG_: case Mnemonic::FMR: case Mnemonic::FMR_:
                    return true;
                default: return false;
                }
            };
            // Body-only extension: pure single-GPR integer operations. They
            // never touch memory, SP, LR or a nonvolatile CR field (record forms
            // write volatile CR0; carrying forms write XER.CA). OE forms, multiple
            // and indexed memory operations stay outside this family.
            auto body_destination = [&](const ppc::Instruction& insn) -> std::optional<uint8_t> {
                if (auto reg=destination(insn)) return reg;
                switch (insn.mnemonic) {
                case Mnemonic::ADD_: case Mnemonic::ADDC: case Mnemonic::ADDC_: case Mnemonic::ADDE: case Mnemonic::ADDE_:
                case Mnemonic::ADDME: case Mnemonic::ADDME_: case Mnemonic::ADDZE: case Mnemonic::ADDZE_:
                case Mnemonic::SUBF: case Mnemonic::SUBF_: case Mnemonic::SUBFC: case Mnemonic::SUBFC_:
                case Mnemonic::SUBFE: case Mnemonic::SUBFE_: case Mnemonic::SUBFIC:
                case Mnemonic::SUBFME: case Mnemonic::SUBFME_: case Mnemonic::SUBFZE: case Mnemonic::SUBFZE_:
                case Mnemonic::NEG: case Mnemonic::NEG_: case Mnemonic::MULLI: case Mnemonic::MULLW: case Mnemonic::MULLW_:
                case Mnemonic::MULHW: case Mnemonic::MULHW_: case Mnemonic::MULHWU: case Mnemonic::MULHWU_:
                case Mnemonic::DIVW: case Mnemonic::DIVW_: case Mnemonic::DIVWU: case Mnemonic::DIVWU_:
                case Mnemonic::LHZ: case Mnemonic::LHA:
                    return insn.rD;
                case Mnemonic::AND: case Mnemonic::AND_: case Mnemonic::ANDC: case Mnemonic::ANDC_:
                case Mnemonic::OR: case Mnemonic::OR_: case Mnemonic::ORC: case Mnemonic::ORC_:
                case Mnemonic::XOR: case Mnemonic::XOR_: case Mnemonic::NAND: case Mnemonic::NAND_:
                case Mnemonic::NOR: case Mnemonic::NOR_: case Mnemonic::EQV: case Mnemonic::EQV_:
                case Mnemonic::ANDI_: case Mnemonic::ANDIS_:
                case Mnemonic::EXTSB: case Mnemonic::EXTSB_: case Mnemonic::EXTSH: case Mnemonic::EXTSH_:
                case Mnemonic::CNTLZW: case Mnemonic::CNTLZW_:
                case Mnemonic::RLWINM_: case Mnemonic::RLWIMI: case Mnemonic::RLWIMI_: case Mnemonic::RLWNM: case Mnemonic::RLWNM_:
                case Mnemonic::SLW: case Mnemonic::SLW_: case Mnemonic::SRW: case Mnemonic::SRW_:
                case Mnemonic::SRAW: case Mnemonic::SRAW_: case Mnemonic::SRAWI: case Mnemonic::SRAWI_:
                    return insn.rA;
                default: return {};
                }
            };
            // STMW rS,imm(r1) saves rS..r31 in consecutive words; each must
            // obey the same rules as an individual STW save.
            auto record_saves=[&](unsigned first, int32_t imm) {
                if (first<14) return false;
                for (unsigned reg=first;reg<32;reg++) {
                    const int32_t slot=imm+int32_t(4*(reg-first));
                    if (slot<8 || slot>=frame || (slot&3) || slots[reg]!=-1 ||
                        std::find(slots.begin(),slots.end(),slot)!=slots.end()) return false;
                    slots[reg]=slot;saved|=uint32_t{1}<<reg;
                }
                return true;
            };
            for (uint32_t pc=stack_pc+4;pc<=stack_pc+32;pc+=4) {
                proof_pc=pc;
                auto insn=instruction(pc);
                if (!insn || local_targets.contains(pc) ||
                    (pc!=start && entry_references.contains(pc))) return rejected("frame_structure_or_unwind",proof_pc);
                if (insn->mnemonic==Mnemonic::STMW && insn->rA==1) {
                    if (!record_saves(insn->rD,insn->imm)) return rejected("frame_structure_or_unwind",proof_pc);
                } else if (insn->mnemonic==Mnemonic::STW && insn->rA==1) {
                    if (insn->rD==capture->rD && insn->imm==lr_slot) {body=pc+4;break;}
                    if (insn->rD<14 || insn->imm<8 || insn->imm>=frame || (insn->imm&3) ||
                        slots[insn->rD]!=-1 || std::find(slots.begin(),slots.end(),insn->imm)!=slots.end())
                        return rejected("frame_structure_or_unwind",proof_pc);
                    slots[insn->rD]=insn->imm;
                    saved|=uint32_t{1}<<insn->rD;
                } else if (insn->mnemonic!=Mnemonic::NOP && !compare(insn->mnemonic)) {
                    auto reg=destination(*insn);
                    if (!reg || insn->mnemonic==Mnemonic::LWZ || insn->mnemonic==Mnemonic::LWZX ||
                        insn->mnemonic==Mnemonic::RLWINM || insn->mnemonic==Mnemonic::ADDIC ||
                        insn->mnemonic==Mnemonic::ADDIC_ || *reg==1 || *reg==capture->rD ||
                        (*reg>=14 && !(saved&(uint32_t{1}<<*reg)))) return rejected("frame_structure_or_unwind",proof_pc);
                }
            }
            if (!body || local_targets.contains(stack_pc)) return rejected("frame_structure_or_unwind",proof_pc);
            // GHS also emits the STMW immediately after the LR save.
            for (auto next=instruction(body); next && next->mnemonic==Mnemonic::STMW && next->rA==1 &&
                 !local_targets.contains(body) && !entry_references.contains(body); next=instruction(body)) {
                proof_pc=body;
                if (!record_saves(next->rD,next->imm)) return rejected("frame_structure_or_unwind",proof_pc);
                body+=4;
            }
            // An explicit interior entry stays independently callable; the
            // prologue heuristic contributes only the proven capture entry.
            struct State {
                uint32_t pc, loaded_lr, restored; bool lr_restored;
                std::array<uint32_t,32> constants{};
                uint32_t known=0, ctr_count=0;
            };
            // Recognize a sentinel traversal with a call-preserved next link.
            // This proves the loop's frame effects, not arbitrary data-dependent
            // termination. Feed the backedge into the same bounded state proof
            // so every reachable exit still needs the complete LR/SP/NV unwind.
            auto sentinel_backedge = [&](const ppc::Instruction& branch) {
                if (!entry_references.contains(start) || branch.mnemonic!=Mnemonic::BC || branch.lk ||
                    (branch.bo&~1u)!=4 || branch.bi%4!=2 || branch.target<body+12 ||
                    branch.target>=branch.addr-12) return false;
                auto advance=instruction(branch.addr-4), test=instruction(branch.addr-8),
                     move=instruction(branch.addr-12), guard=instruction(branch.target-4),
                     prefetch=instruction(branch.target-8), initial=instruction(branch.target-12);
                if (!advance || !test || !move || !guard || !prefetch || !initial ||
                    test->mnemonic!=Mnemonic::CMPL || initial->mnemonic!=Mnemonic::CMPL ||
                    test->rD!=initial->rD || (test->rD&3) || (test->rD>>2)!=branch.bi/4 ||
                    test->rA!=initial->rA || test->rB!=initial->rB ||
                    guard->mnemonic!=Mnemonic::BC || guard->lk || (guard->bo&~1u)!=12 ||
                    guard->bi!=branch.bi || guard->target!=branch.addr+4 ||
                    move->mnemonic!=Mnemonic::MR || move->rA!=test->rA ||
                    advance->mnemonic!=Mnemonic::LWZ || advance->rD!=move->rD ||
                    advance->rA!=move->rD || advance->imm<0 || (advance->imm&3) ||
                    prefetch->mnemonic!=Mnemonic::LWZ || prefetch->rD!=move->rD ||
                    prefetch->rA!=test->rA || prefetch->imm!=advance->imm) return false;
                unsigned current=test->rA, next=move->rD, sentinel=test->rB;
                if (current==1 || current==2 || current>=13 || next<14 || sentinel<14 ||
                    next==sentinel || !(saved&(uint32_t{1}<<next)) ||
                    !(saved&(uint32_t{1}<<sentinel))) return false;
                for (uint32_t pc=branch.target;pc<branch.addr-12;pc+=4) {
                    auto op=instruction(pc);
                    if (!op || entry_references.contains(pc)) return false;
                    bool call=op->mnemonic==Mnemonic::BL || op->mnemonic==Mnemonic::BLA ||
                              op->mnemonic==Mnemonic::BCTRL;
                    if (call) continue; // next/sentinel are nonvolatile across calls
                    if (op->is_branch()) {
                        if (op->lk || (op->mnemonic!=Mnemonic::BC && op->mnemonic!=Mnemonic::B) ||
                            op->target<=pc || op->target>branch.addr-12) return false;
                        continue;
                    }
                    if (op->mnemonic==Mnemonic::MTCTR) continue;
                    bool store=op->mnemonic==Mnemonic::STW || op->mnemonic==Mnemonic::STH ||
                               op->mnemonic==Mnemonic::STB;
                    auto reg=destination(*op);
                    if (op->mnemonic==Mnemonic::LBZ) reg=op->rD;
                    if (reg) {
                        if (*reg==1 || *reg==2 || *reg==13 || *reg==next || *reg==sentinel)
                            return false;
                        // No frame accesses or address aliases inside this family.
                        if (op->rA==1 || (op->mnemonic==Mnemonic::MR && op->rD==1) ||
                            ((op->mnemonic==Mnemonic::LWZX || op->mnemonic==Mnemonic::ADD ||
                              op->mnemonic==Mnemonic::ADD_) && op->rB==1)) return false;
                    } else if (store) {
                        if (op->rA==1) return false;
                    } else if (op->mnemonic!=Mnemonic::NOP && !compare(op->mnemonic)) return false;
                }
                return true;
            };
            struct CountedLoop {uint32_t changed=0; std::set<uint32_t> exits;};
            auto counted_loop = [&](const ppc::Instruction& latch) -> std::optional<CountedLoop> {
                // Membership is head-reachable AND can reach this latch. An
                // early epilogue may be physically inside the address interval
                // without executing on any path that repeats the loop.
                std::map<uint32_t,std::vector<uint32_t>> edges;
                std::vector<uint32_t> todo{latch.target};
                while (!todo.empty()) {
                    uint32_t pc=todo.back();todo.pop_back();
                    if (pc<body || uint64_t(pc)>=uint64_t(start)+512 || (pc&3) || edges.contains(pc)) continue;
                    auto op=instruction(pc);
                    if (!op || op->mnemonic==Mnemonic::UNKNOWN) return {};
                    auto& successors=edges[pc];
                    if (edges.size()>128) return {};
                    if (pc==latch.addr) continue;
                    if (op->mnemonic==Mnemonic::B || op->mnemonic==Mnemonic::BA)
                        successors.push_back(op->target);
                    else if (op->mnemonic==Mnemonic::BC || op->mnemonic==Mnemonic::BCA) {
                        successors.push_back(op->target);successors.push_back(pc+4);
                    } else if (!op->is_branch() || op->mnemonic==Mnemonic::BL ||
                               op->mnemonic==Mnemonic::BLA || op->mnemonic==Mnemonic::BCTRL)
                        successors.push_back(pc+4);
                    for (auto target:successors) todo.push_back(target);
                }
                if (!edges.contains(latch.addr)) return {};
                std::map<uint32_t,std::vector<uint32_t>> predecessors;
                for (const auto& [pc,successors]:edges)
                    for (auto target:successors) predecessors[target].push_back(pc);
                std::set<uint32_t> members;
                todo={latch.addr};
                while (!todo.empty()) {
                    uint32_t pc=todo.back();todo.pop_back();
                    if (!members.insert(pc).second) continue;
                    for (auto source:predecessors[pc]) todo.push_back(source);
                }
                if (!members.contains(latch.target)) return {};
                CountedLoop proof;
                auto protected_reg=[](unsigned reg) {return reg==1 || reg==2 || reg==13;};
                auto write=[&](unsigned reg) {
                    if (protected_reg(reg) || reg>=14) return false;
                    proof.changed|=uint32_t{1}<<reg;return true;
                };
                for (auto pc:members) {
                    if (pc==latch.addr) continue;
                    auto op=instruction(pc);
                    if (entry_references.contains(pc)) return {};
                    if (op->is_branch()) {
                        // Only meaningful forward ordinary/CR-only edges are
                        // admitted. No calls, extra backedges or CTR conditions.
                        bool direct=op->mnemonic==Mnemonic::B || op->mnemonic==Mnemonic::BA;
                        bool conditional=(op->mnemonic==Mnemonic::BC || op->mnemonic==Mnemonic::BCA) &&
                            ((op->bo&~1u)==4 || (op->bo&~1u)==12);
                        if (op->lk || (!direct && !conditional) || op->target<=pc+4) return {};
                    } else {
                        bool load=op->mnemonic==Mnemonic::LWZ || op->mnemonic==Mnemonic::LWZX ||
                                  op->mnemonic==Mnemonic::LBZ || op->mnemonic==Mnemonic::LBZX;
                        bool load_update=op->mnemonic==Mnemonic::LWZU || op->mnemonic==Mnemonic::LBZU;
                        bool store_update=op->mnemonic==Mnemonic::STBU || op->mnemonic==Mnemonic::STHU ||
                                          op->mnemonic==Mnemonic::STWU;
                        bool store=store_update || op->mnemonic==Mnemonic::STW ||
                                   op->mnemonic==Mnemonic::STH || op->mnemonic==Mnemonic::STB;
                        bool indexed=op->mnemonic==Mnemonic::LWZX || op->mnemonic==Mnemonic::LBZX;
                        if (load || load_update || store) {
                            if (protected_reg(op->rA) || (indexed && protected_reg(op->rB))) return {};
                            if (load_update && (!op->rA || op->rA==op->rD)) return {};
                            if (store_update && !op->rA) return {};
                            if ((load || load_update) && !write(op->rD)) return {};
                            if ((load_update || store_update) && !write(op->rA)) return {};
                            if (store && protected_reg(op->rD)) return {};
                        } else if (op->mnemonic==Mnemonic::LFS || op->mnemonic==Mnemonic::LFD ||
                                   op->mnemonic==Mnemonic::STFS || op->mnemonic==Mnemonic::STFD ||
                                   op->mnemonic==Mnemonic::LFSU || op->mnemonic==Mnemonic::LFDU ||
                                   op->mnemonic==Mnemonic::STFSU || op->mnemonic==Mnemonic::STFDU) {
                            // FP memory: GPR effects only through an updating base,
                            // which must be a volatile register; loads target f0-f13.
                            bool fp_load=op->mnemonic==Mnemonic::LFS || op->mnemonic==Mnemonic::LFD ||
                                         op->mnemonic==Mnemonic::LFSU || op->mnemonic==Mnemonic::LFDU;
                            bool fp_update=op->mnemonic==Mnemonic::LFSU || op->mnemonic==Mnemonic::LFDU ||
                                           op->mnemonic==Mnemonic::STFSU || op->mnemonic==Mnemonic::STFDU;
                            if (protected_reg(op->rA) || (fp_load && op->rD>=14)) return {};
                            if (fp_update && (!op->rA || !write(op->rA))) return {};
                        } else if (auto reg=destination(*op)) {
                            bool move=op->mnemonic==Mnemonic::MR || op->mnemonic==Mnemonic::MR_ ||
                                      op->mnemonic==Mnemonic::RLWINM;
                            bool immediate=op->mnemonic==Mnemonic::LI || op->mnemonic==Mnemonic::LIS;
                            bool binary=op->mnemonic==Mnemonic::ADD || op->mnemonic==Mnemonic::ADD_;
                            if (!write(*reg) || (!immediate && protected_reg(move ? op->rD : op->rA)) ||
                                (binary && protected_reg(op->rB))) return {};
                        } else if (op->mnemonic!=Mnemonic::NOP && !compare(op->mnemonic)) return {};
                    }
                    for (auto target:edges[pc]) if (!members.contains(target)) proof.exits.insert(target);
                }
                proof.exits.insert(latch.addr+4);
                return proof;
            };
            std::vector<State> pending{{body,0,0,false}};
            // Join body constants at merges; retain distinct LR/save proofs.
            // Unsafe paths must never borrow another predecessor's restore.
            using FrameKey=std::tuple<uint32_t,uint32_t,uint32_t,bool>;
            std::map<FrameKey,State> visited;
            bool returned=false;
            while (!pending.empty()) {
                auto state=pending.back(); pending.pop_back();
                if (uint64_t(state.pc)>=uint64_t(start)+512 || state.pc<body || (state.pc&3)) return rejected("frame_structure_or_unwind",proof_pc);
                FrameKey key{state.pc,state.loaded_lr,state.restored,state.lr_restored};
                auto [found,inserted]=visited.emplace(key,state);
                if (!inserted) {
                    auto joined=found->second;
                    joined.known&=state.known;
                    for (unsigned reg=0;reg<32;reg++) {
                        uint32_t bit=uint32_t{1}<<reg;
                        if (joined.constants[reg]!=state.constants[reg]) joined.known&=~bit;
                        if (!(joined.known&bit)) joined.constants[reg]=0;
                    }
                    if (joined.ctr_count!=state.ctr_count) joined.ctr_count=0;
                    if (joined.known==found->second.known &&
                        joined.constants==found->second.constants &&
                        joined.ctr_count==found->second.ctr_count) continue;
                    found->second=joined;
                    state=joined;
                }
                if (visited.size()>128) return rejected("frame_structure_or_unwind",proof_pc);
                proof_pc=state.pc;
                auto insn=instruction(state.pc);
                if (!insn || insn->mnemonic==Mnemonic::UNKNOWN) return rejected("frame_structure_or_unwind",proof_pc);
                if (insn->mnemonic==Mnemonic::ADDI && insn->rD==1 && insn->rA==1) {
                    if (insn->imm!=frame || !state.lr_restored || state.restored!=saved) return rejected("frame_structure_or_unwind",proof_pc);
                    // A compiler may assign return values after the unwind.
                    // Prove a short memory-free tail that changes only return
                    // registers, never SP/SDA/nonvolatiles/LR or control flow.
                    bool tail_return=false;
                    for (uint32_t distance=4;distance<=20;distance+=4) {
                        auto tail=instruction(state.pc+distance);
                        if (!tail) return rejected("frame_structure_or_unwind",proof_pc);
                        if (tail->mnemonic==Mnemonic::BLR && !tail->lk) {tail_return=true;break;}
                        if (tail->mnemonic==Mnemonic::NOP) continue;
                        bool move=tail->mnemonic==Mnemonic::MR;
                        bool immediate=tail->mnemonic==Mnemonic::LI || tail->mnemonic==Mnemonic::LIS ||
                                       tail->mnemonic==Mnemonic::ADDI || tail->mnemonic==Mnemonic::ADDIS;
                        auto reg=destination(*tail);
                        if ((!move && !immediate) || !reg || (*reg!=3 && *reg!=4) ||
                            (move ? tail->rD==1 : tail->rA==1)) return rejected("frame_structure_or_unwind",proof_pc);
                    }
                    if (!tail_return) return rejected("frame_structure_or_unwind",proof_pc);
                    returned=true;
                    continue;
                }
                auto forget=[&](unsigned reg) {
                    uint32_t bit=uint32_t{1}<<reg;
                    state.loaded_lr&=~bit;state.restored&=~bit;
                    state.known&=~bit;state.constants[reg]=0;
                };
                if (insn->mnemonic==Mnemonic::MTCTR) {
                    auto value=state.constants[insn->rD];
                    // No unrolling depends on the trip count. Retain the
                    // conservative positive signed range, not a 16-bit ceiling.
                    state.ctr_count=(state.known&(uint32_t{1}<<insn->rD)) && value && value<=0x7FFFFFFFu ? value : 0;
                } else if (insn->mnemonic==Mnemonic::MTLR) {
                    if (!(state.loaded_lr&(uint32_t{1}<<insn->rD))) return rejected("frame_structure_or_unwind",proof_pc);
                    state.lr_restored=true;
                } else if (insn->mnemonic==Mnemonic::BL || insn->mnemonic==Mnemonic::BLA ||
                           insn->mnemonic==Mnemonic::BCTRL) {
                    state.loaded_lr=0; state.lr_restored=false;
                    state.ctr_count=0;
                    for (unsigned reg=0;reg<13;reg++) if (reg!=1 && reg!=2) forget(reg);
                } else if (insn->mnemonic==Mnemonic::B || insn->mnemonic==Mnemonic::BA ||
                           insn->mnemonic==Mnemonic::BC || insn->mnemonic==Mnemonic::BCA) {
                    if (insn->target<=state.pc) {
                        if (sentinel_backedge(*insn)) {
                            if (state.loaded_lr || state.restored || state.lr_restored) return rejected("frame_structure_or_unwind",proof_pc);
                            auto repeat=state;repeat.pc=insn->target;
                            pending.push_back(repeat);
                            state.pc+=4;pending.push_back(state);
                            continue;
                        }
                        // Prove a finite BDNZ loop, not an arbitrary backedge.
                        // MTCTR must have a dominating known positive count.
                        // Each iteration changes only volatile GPRs/object
                        // memory: no calls, stack/LR/restores or CTR rewrites.
                        if ((insn->mnemonic!=Mnemonic::BC && insn->mnemonic!=Mnemonic::BCA) ||
                            (insn->bo&~1u)!=16 || !state.ctr_count || insn->target<body ||
                            insn->target==state.pc) return rejected("frame_structure_or_unwind",proof_pc);
                        auto loop=counted_loop(*insn);
                        if (!loop) return rejected("counted_loop_effect_or_control",proof_pc);
                        // The frame proof summarizes effects; emitted code
                        // still executes every iteration. Forget loop-written
                        // constants so later CTR proofs cannot reuse values
                        // observed only on the first iteration.
                        for (unsigned reg=0;reg<32;reg++) if (loop->changed&(uint32_t{1}<<reg)) forget(reg);
                        state.ctr_count=0;
                        // Any early exit can occur after another iteration.
                        // Revalidate it with summarized constants, never with
                        // first-iteration induction values or epilogue restores.
                        for (auto target:loop->exits) {
                            auto exit=state;exit.pc=target;pending.push_back(exit);
                        }
                        continue;
                    } else {
                        if (!(insn->bo&4u) && (insn->mnemonic==Mnemonic::BC || insn->mnemonic==Mnemonic::BCA))
                            state.ctr_count=0;
                        auto branch_state=state; branch_state.pc=insn->target;
                        pending.push_back(branch_state);
                        if (insn->mnemonic==Mnemonic::B || insn->mnemonic==Mnemonic::BA) continue;
                    }
                } else if (insn->mnemonic==Mnemonic::STW || insn->mnemonic==Mnemonic::STH ||
                           insn->mnemonic==Mnemonic::STB || insn->mnemonic==Mnemonic::STBU ||
                           insn->mnemonic==Mnemonic::STHU || insn->mnemonic==Mnemonic::STWU) {
                    // Non-updating stores change memory, not GPRs. Reject any
                    // byte-range overlap with the LR or nonvolatile save slots,
                    // including narrow/unaligned writes inside a saved word.
                    bool update=insn->mnemonic==Mnemonic::STBU || insn->mnemonic==Mnemonic::STHU ||
                                insn->mnemonic==Mnemonic::STWU;
                    if (update) {
                        auto reg=insn->rA;
                        if (reg==0 || reg==1 || reg==2 || reg==13 || (reg>=14 && !(saved&(uint32_t{1}<<reg))))
                            return rejected("frame_structure_or_unwind",proof_pc);
                        forget(reg);
                    } else if (insn->rA==1) {
                        int32_t width=insn->mnemonic==Mnemonic::STW ? 4 :
                                      insn->mnemonic==Mnemonic::STH ? 2 : 1;
                        auto overlaps=[&](int32_t slot) {
                            return slot!=-1 && insn->imm<slot+4 && insn->imm+width>slot;
                        };
                        if (overlaps(lr_slot) || std::any_of(slots.begin(),slots.end(),overlaps))
                            return rejected("frame_structure_or_unwind",proof_pc);
                    }
                } else if (insn->mnemonic==Mnemonic::LMW) {
                    // Restores rD..r31; each register must reload its own slot.
                    if (insn->rA!=1 || insn->rD<14) return rejected("frame_structure_or_unwind",proof_pc);
                    for (unsigned reg=insn->rD;reg<32;reg++) {
                        if (slots[reg]!=insn->imm+int32_t(4*(reg-insn->rD))) return rejected("frame_structure_or_unwind",proof_pc);
                        forget(reg);
                        state.restored|=uint32_t{1}<<reg;
                    }
                } else if (insn->mnemonic==Mnemonic::LFS || insn->mnemonic==Mnemonic::LFD) {
                    // Non-updating FP loads write an FPR, never the equally
                    // numbered GPR. Only volatile FPR destinations are proved
                    // here; no unsaved f14-f31 writes or indexed variants.
                    if (insn->rD>=14) return rejected("unsaved_fpr_write",proof_pc);
                } else if (insn->mnemonic==Mnemonic::LFSU || insn->mnemonic==Mnemonic::LFDU ||
                           insn->mnemonic==Mnemonic::STFSU || insn->mnemonic==Mnemonic::STFDU) {
                    // Updating FP accesses also write the GPR base. Only a
                    // volatile base (r3-r12) is admitted, so neither SP nor any
                    // nonvolatile object pointer changes. Loads need a volatile FPR.
                    const bool load=insn->mnemonic==Mnemonic::LFSU || insn->mnemonic==Mnemonic::LFDU;
                    if (load && insn->rD>=14) return rejected("unsaved_fpr_write",proof_pc);
                    auto reg=insn->rA;
                    if (reg<3 || reg>12) return rejected("frame_structure_or_unwind",proof_pc);
                    forget(reg);
                } else if (insn->mnemonic==Mnemonic::STFS || insn->mnemonic==Mnemonic::STFD) {
                    // FP stores read their FPR and GPR base. They cannot change
                    // SP/LR or integer restore evidence, but a stack write must
                    // not corrupt any saved integer value.
                    if (insn->rA==1) {
                        const int32_t width=insn->mnemonic==Mnemonic::STFD ? 8 : 4;
                        auto overlaps=[&](int32_t slot) {
                            return slot!=-1 && insn->imm<slot+4 && insn->imm+width>slot;
                        };
                        if (overlaps(lr_slot) || std::any_of(slots.begin(),slots.end(),overlaps))
                            return rejected("fp_store_save_slot_overlap",proof_pc);
                    }
                } else if (insn->mnemonic==Mnemonic::STWX || insn->mnemonic==Mnemonic::STHX ||
                           insn->mnemonic==Mnemonic::STBX) {
                    // Indexed stores change memory only. An SP base or index
                    // could reach a save slot at an unknown offset: reject it.
                    if (insn->rA==1 || insn->rB==1) return rejected("frame_structure_or_unwind",proof_pc);
                } else if (insn->mnemonic==Mnemonic::FCMPU || insn->mnemonic==Mnemonic::FCMPO) {
                    // FP compares write only a CR field, like integer compares.
                } else if (fp_arithmetic(insn->mnemonic)) {
                    // FP register arithmetic: FPR result only (CR1/FPSCR side
                    // effects do not participate in the unwind proof).
                    if (insn->rD>=14) return rejected("unsaved_fpr_write",proof_pc);
                } else if (insn->mnemonic==Mnemonic::LBZ || insn->mnemonic==Mnemonic::LBZX) {
                    // A byte load changes RT only. Even from a saved word's
                    // address it cannot restore the full LR/nonvolatile value.
                    auto reg=insn->rD;
                    if (reg==1 || reg==2 || reg==13 || (reg>=14 && !(saved&(uint32_t{1}<<reg))))
                        return rejected("frame_structure_or_unwind",proof_pc);
                    forget(reg);
                } else if (insn->mnemonic==Mnemonic::LWZU || insn->mnemonic==Mnemonic::LBZU) {
                    // Updating loads write both RT and RA. They cannot update
                    // SP/SDA or an unsaved nonvolatile, and PPC forbids RA=0
                    // or RA=RT. Neither result proves a constant or a restore.
                    auto writable=[&](unsigned reg) {
                        return reg!=1 && reg!=2 && reg!=13 &&
                            (reg<14 || (saved&(uint32_t{1}<<reg)));
                    };
                    if (!insn->rA || insn->rA==insn->rD ||
                        !writable(insn->rA) || !writable(insn->rD)) return rejected("frame_structure_or_unwind",proof_pc);
                    forget(insn->rD);
                    forget(insn->rA);
                } else if (insn->mnemonic!=Mnemonic::NOP && !compare(insn->mnemonic)) {
                    auto reg=body_destination(*insn);
                    if (!reg) return rejected("body_instruction_unmodelled",proof_pc);
                    if (*reg==1 || (*reg>=14 && !(saved&(uint32_t{1}<<*reg)))) return rejected("unsafe_gpr_write",proof_pc);
                    uint32_t bit=uint32_t{1}<<*reg;
                    std::optional<uint32_t> value;
                    if (insn->mnemonic==Mnemonic::LI) value=uint32_t(insn->imm);
                    else if (insn->mnemonic==Mnemonic::LIS) value=uint32_t(insn->imm)<<16;
                    else if ((insn->mnemonic==Mnemonic::MR || insn->mnemonic==Mnemonic::MR_) &&
                             (state.known&(uint32_t{1}<<insn->rD))) value=state.constants[insn->rD];
                    else if ((insn->mnemonic==Mnemonic::ADDI || insn->mnemonic==Mnemonic::ADDIS ||
                              insn->mnemonic==Mnemonic::ADDIC || insn->mnemonic==Mnemonic::ADDIC_) &&
                             (state.known&(uint32_t{1}<<insn->rA)))
                        value=state.constants[insn->rA]+(insn->mnemonic==Mnemonic::ADDIS ?
                            uint32_t(insn->imm)<<16 : uint32_t(insn->imm));
                    else if ((insn->mnemonic==Mnemonic::ORI || insn->mnemonic==Mnemonic::ORIS) &&
                             (state.known&(uint32_t{1}<<insn->rD)))
                        value=state.constants[insn->rD]|(insn->mnemonic==Mnemonic::ORIS ?
                            uint32_t(insn->uimm)<<16 : uint32_t(insn->uimm));
                    forget(*reg);
                    if (value) {state.known|=bit;state.constants[*reg]=*value;}
                    if (insn->mnemonic==Mnemonic::LWZ && insn->rA==1) {
                        if (insn->imm==lr_slot) state.loaded_lr|=bit;
                        if (slots[*reg]!=-1 && slots[*reg]==insn->imm) state.restored|=bit;
                    }
                }
                state.pc+=4;
                pending.push_back(state);
            }
            if (!returned) return rejected("no_proved_return",proof_pc);
            if (m_cfg.frame_audit) m_cfg.frame_audit({start,stack_pc,proof_pc,"frame_safe",true});
            return start;
        };
        uint32_t post_entry=post_stack_entry();
        if (post_entry!=stack_pc) {
            ++m_stats.post_stack_prologues;
            return post_entry;
        }
        // Bounded straight-line prefix: capture LR, save it to old SP+4,
        // optionally set up registers/CR, then allocate the stack frame.
        // No call/branch/SP update or clobber of the unsaved LR is allowed.
        for (uint32_t distance=8;distance<=32 && distance<=stack_pc-section.address;distance+=4) {
            uint32_t start=stack_pc-distance;
            auto first=instruction(start);
            if (!first || first->mnemonic!=Mnemonic::MFLR || first->rD==1) continue;
            auto previous=start>=section.address+4 ? instruction(start-4) : std::nullopt;
            if (start!=section.address && !entry_references.contains(start) &&
                (!previous || previous->mnemonic!=Mnemonic::BLR || previous->lk)) continue;
            bool saved=false, valid=true;
            for (uint32_t pc=start+4;pc<stack_pc;pc+=4) {
                auto insn=instruction(pc);
                if (!insn) {valid=false;break;}
                if (insn->mnemonic==Mnemonic::STW && insn->rD==first->rD &&
                    insn->rA==1 && insn->imm==4 && !saved) {saved=true;continue;}
                if (insn->mnemonic==Mnemonic::NOP || insn->mnemonic==Mnemonic::CMP ||
                    insn->mnemonic==Mnemonic::CMPI || insn->mnemonic==Mnemonic::CMPL ||
                    insn->mnemonic==Mnemonic::CMPLI) continue;
                bool move=insn->mnemonic==Mnemonic::MR || insn->mnemonic==Mnemonic::MR_;
                bool immediate=insn->mnemonic==Mnemonic::LI || insn->mnemonic==Mnemonic::LIS ||
                               insn->mnemonic==Mnemonic::ADDI || insn->mnemonic==Mnemonic::ADDIS;
                uint8_t destination=move ? insn->rA : insn->rD;
                if ((!move && !immediate) || destination==1 ||
                    (!saved && destination==first->rD)) {valid=false;break;}
            }
            if (valid && saved) return start;
        }
        return stack_pc;
    };
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
                const uint32_t addr = pre_stack_entry(sec,base+off);
                if (is_valid_code_addr(addr)) {
                    add_candidate(addr, {});
                    ++m_stats.from_prologues;
                    if (addr!=base+off && base+off-addr>4) ++m_stats.pre_stack_prologues;
                }
                continue;
            }

            // --- large-frame prologue (frames beyond the 16-bit stwu range) ---
            //    lis rX,-N / [mflr r0] / addi|ori rX,rX,imm / stwux r1,r1,rX
            // right after an unconditional terminator (Gambit 0275E1A4, a
            // member-function callback with a 45 KB frame and no BL caller).
            if ((word & 0xFFFF07FFu) == 0x7C21016Eu && off >= 12) {
                const uint32_t reg = (word >> 11) & 31u;
                auto at = [&](uint32_t o) {
                    return (uint32_t(sec.data[o]) << 24) | (uint32_t(sec.data[o + 1]) << 16) |
                           (uint32_t(sec.data[o + 2]) << 8) | sec.data[o + 3];
                };
                const uint32_t add = at(off - 4);
                const bool low = reg != 0 && ((add >> 26) == 14 || (add >> 26) == 24) &&
                                 ((add >> 21) & 31u) == reg && ((add >> 16) & 31u) == reg;
                uint32_t entry = off - 4;
                bool high = false;
                for (unsigned i = 0; i < 2 && entry >= 4; ++i) {
                    const uint32_t prev = at(entry - 4);
                    if (prev == 0x7C0802A6u) { entry -= 4; continue; }   // mflr r0
                    if ((prev >> 16) == (0x3C000000u | reg << 21) >> 16 && (prev & 0x8000u)) {
                        entry -= 4; high = true; continue;                // lis rX,-N
                    }
                    break;
                }
                if (low && high && entry >= 4) {
                    const uint32_t before = at(entry - 4);
                    const bool terminator = before == 0x4E800020u || before == 0x4E800420u ||
                                            ((before >> 26) == 18 && !(before & 1u));
                    const uint32_t addr = base + entry;
                    if (terminator && is_valid_code_addr(addr)) {
                        add_candidate(addr, {});
                        ++m_stats.from_prologues;
                    }
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
    // Follow the function's local control flow, bounded by the next known
    // entry, collecting direct call targets (bl / bla / conditional bcl).
    // Calls after an early return, a loop or a local jump are reached
    // through the branches that lead to them (Gambit 029D329C b +8 before
    // bnel 029D2734; 0297F024 bnel 0297EBA0 after an early return).
    auto next = m_candidates.upper_bound(start);
    const uint32_t limit = next == m_candidates.end() ? end : std::min(*next, end);
    auto local = [&](uint32_t target) { return target >= start && target < limit; };
    std::vector<uint32_t> work{start};
    std::set<uint32_t> seen;
    while (!work.empty()) {
        uint32_t pc = work.back();
        work.pop_back();
        for (; local(pc) && pc + 4 <= end; pc += 4) {
            if (!seen.insert(pc).second) break;
            auto word_opt = m_module.read_word(pc);
            if (!word_opt) break;
            auto insn = ppc::decode(*word_opt, pc);
            if (!insn) continue;

            // Direct calls (bl / bla / bcl)
            if (insn->is_call() && !insn->is_indirect_branch()) {
                uint32_t target = insn->target;
                if (is_valid_code_addr(target) &&
                    m_candidates.find(target) == m_candidates.end()) {
                    add_candidate(target, {});
                    ++m_stats.from_call_targets;
                }
            }

            // Function terminators and local control flow
            if (insn->is_return() || insn->mnemonic == ppc::Mnemonic::BCTR) break;
            if ((insn->mnemonic == ppc::Mnemonic::B || insn->mnemonic == ppc::Mnemonic::BA) && !insn->lk) {
                if (local(insn->target)) work.push_back(insn->target);   // else: tail call
                break;
            }
            if ((insn->mnemonic == ppc::Mnemonic::BC || insn->mnemonic == ppc::Mnemonic::BCA) &&
                !insn->lk && local(insn->target))
                work.push_back(insn->target);                              // and fall through
        }
    }
}

bool FunctionDiscovery::is_isolated_leaf(uint32_t addr,
                                        const std::set<uint32_t>& branch_targets) const {
    using ppc::Mnemonic;
    auto instruction = [&](uint32_t pc) -> std::optional<ppc::Instruction> {
        const auto* sec = m_module.section_at_addr(pc);
        if (!is_valid_code_addr(pc) || !sec || uint64_t(pc-sec->address)+4>sec->data.size()) return {};
        auto word = m_module.read_word(pc);
        return word ? ppc::decode(*word,pc) : std::nullopt;
    };
    // No fallthrough from a previous body, direct local entry, or jump-table
    // stub. This deliberately rejects ambiguous shared/interior entries.
    if (addr<4 || branch_targets.contains(addr)) return false;
    auto previous=instruction(addr-4), first=instruction(addr);
    if (!previous || previous->mnemonic!=Mnemonic::BLR || previous->lk ||
        !first || first->is_branch()) return false;
    constexpr uint32_t max_leaf_bytes=64*4;
    std::unordered_map<uint32_t,uint8_t> state;
    std::function<bool(uint32_t)> visit = [&](uint32_t pc) {
        if (pc<addr || uint64_t(pc)-addr>=max_leaf_bytes) return false;
        auto found=state.find(pc);
        if (found!=state.end()) return found->second==2; // reject cycles
        auto insn=instruction(pc);
        if (!insn || insn->mnemonic==Mnemonic::UNKNOWN || insn->is_call()) return false;
        state[pc]=1;
        bool valid=false;
        if (insn->mnemonic==Mnemonic::BLR) valid=true;
        else if (insn->mnemonic==Mnemonic::BCLR) valid=visit(pc+4); // return or fallthrough
        else if (insn->is_indirect_branch()) valid=false;
        else if (insn->mnemonic==Mnemonic::B || insn->mnemonic==Mnemonic::BA) valid=visit(insn->target);
        else if (insn->is_branch()) valid=visit(insn->target) && visit(pc+4);
        else if (insn->iclass!=ppc::InstrClass::System) valid=visit(pc+4);
        if (valid) state[pc]=2;
        return valid;
    };
    return visit(addr);
}

void FunctionDiscovery::scan_code_pointer_relocations() {
    using ppc::Mnemonic;
    // Metadata is the source of candidates; never scan arbitrary text
    // constants. The matching decoded pair must actually form S+A.
    auto target = [&](const rpx::RpxReloc& relocation) -> std::optional<uint32_t> {
        if (relocation.sym_index<m_module.symbols.size())
            return m_module.symbols[relocation.sym_index].address+uint32_t(relocation.addend);
        if (relocation.sym_value) return *relocation.sym_value+uint32_t(relocation.addend);
        return {};
    };
    std::unordered_map<uint32_t,const rpx::RpxReloc*> low;
    for (const auto& relocation : m_module.relocations)
        if (relocation.type==elf::R_PPC_ADDR16_LO && (relocation.offset & 3u)==2)
            low.emplace(relocation.offset,&relocation);

    // A BLR separator rules out fallthrough. Direct non-call branch targets
    // rule out local CFG bodies/cases reached across that separator. Existing
    // inline branch-table stubs are branches themselves and fail the entry
    // check; their case bodies are also direct branch targets.
    std::set<uint32_t> branch_targets;
    for (const auto& sec : m_module.sections) {
        if (!sec.is_executable()) continue;
        for (uint32_t off=0;uint64_t(off)+4<=sec.data.size();off+=4) {
            uint32_t word=(uint32_t(sec.data[off])<<24)|(uint32_t(sec.data[off+1])<<16)|
                          (uint32_t(sec.data[off+2])<<8)|sec.data[off+3];
            if (((word>>26)==18 || (word>>26)==16) && !(word&1u)) {
                auto branch=ppc::decode(word,sec.address+off);
                if (branch) branch_targets.insert(branch->target);
            }
        }
    }
    for (const auto& high : m_module.relocations) {
        if ((high.type!=elf::R_PPC_ADDR16_HA && high.type!=elf::R_PPC_ADDR16_HI) ||
            (high.offset & 3u)!=2 || high.offset>UINT32_MAX-4) continue;
        uint32_t distance=4;
        auto pair=low.find(high.offset+distance);
        if (pair==low.end()) {
            // A scheduled indexed load may separate the two pointer halves.
            // Prove that it neither reads nor overwrites the partial address,
            // and cannot be entered independently. Keep the leaf body proof.
            const uint32_t between_pc=high.offset+2;
            auto upper_word=m_module.read_word(high.offset-2);
            auto between_word=m_module.read_word(between_pc);
            auto upper=upper_word?ppc::decode(*upper_word,high.offset-2):std::nullopt;
            auto between=between_word?ppc::decode(*between_word,between_pc):std::nullopt;
            if (!upper || upper->mnemonic!=Mnemonic::LIS || !between ||
                between->mnemonic!=Mnemonic::LWZX || between->rD==upper->rD ||
                (between->rA!=0 && between->rA==upper->rD) || between->rB==upper->rD ||
                branch_targets.contains(between_pc) || branch_targets.contains(between_pc+4)) continue;
            distance=8;pair=low.find(high.offset+distance);
        }
        auto value=target(high);
        if (pair==low.end() || !value || target(*pair->second)!=value ||
            !is_valid_code_addr(*value) || m_candidates.contains(*value)) continue;
        uint32_t pc=high.offset-2;
        const auto* sec=m_module.section_at_addr(pc);
        if (!sec || !sec->is_executable() || uint64_t(pc-sec->address)+distance+4>sec->data.size()) continue;
        auto upper_word=m_module.read_word(pc), lower_word=m_module.read_word(pc+distance);
        if (!upper_word || !lower_word) continue;
        auto upper=ppc::decode(*upper_word,pc), lower=ppc::decode(*lower_word,pc+distance);
        if (!upper || !lower || upper->mnemonic!=Mnemonic::LIS) continue;
        uint32_t formed=uint32_t(upper->imm)<<16;
        if (lower->mnemonic==Mnemonic::ADDI && upper->rD!=0 &&
            lower->rD==upper->rD && lower->rA==upper->rD) formed+=uint32_t(lower->imm);
        else if (lower->mnemonic==Mnemonic::ORI && lower->rD==upper->rD &&
                 lower->rA==upper->rD) formed|=lower->uimm;
        else continue;
        if (formed!=*value || !is_isolated_leaf(*value,branch_targets)) continue;
        add_candidate(*value,{});
        ++m_stats.from_code_ptr_relocs;
    }
}

void FunctionDiscovery::scan_frameless_callbacks() {
    using ppc::Mnemonic;
    auto instruction=[&](uint32_t pc)->std::optional<ppc::Instruction> {
        const auto* sec=m_module.section_at_addr(pc);
        if (!is_valid_code_addr(pc) || !sec || uint64_t(pc-sec->address)+4>sec->data.size()) return {};
        auto word=m_module.read_word(pc);
        return word ? ppc::decode(*word,pc) : std::nullopt;
    };
    auto relocation_target=[&](const rpx::RpxReloc& r)->std::optional<uint32_t> {
        if (r.sym_index<m_module.symbols.size()) return m_module.symbols[r.sym_index].address+uint32_t(r.addend);
        if (r.sym_value) return *r.sym_value+uint32_t(r.addend);
        return {};
    };
    auto volatile_reg=[](unsigned r){return r==0 || (r>=3 && r<=12);};
    // This small family has no frame, LR writes, calls or protected-register
    // dependencies. Keep the existing adjacent-pointer leaf pass unchanged.
    auto plain_setup=[&](const ppc::Instruction& i) {
        bool move=i.mnemonic==Mnemonic::MR || i.mnemonic==Mnemonic::MR_;
        bool immediate=i.mnemonic==Mnemonic::LI || i.mnemonic==Mnemonic::LIS ||
                       i.mnemonic==Mnemonic::ADDI || i.mnemonic==Mnemonic::ADDIS;
        return (move || immediate) && volatile_reg(move ? i.rA : i.rD) &&
               volatile_reg(move ? i.rD : i.rA);
    };
    std::unordered_map<uint32_t,std::vector<uint32_t>> incoming;
    for (const auto& s:m_module.sections) {
        if (!s.is_executable()) continue;
        for (uint32_t off=0;uint64_t(off)+4<=s.data.size();off+=4) {
            uint32_t pc=s.address+off;
            auto i=instruction(pc);
            if (!i || !i->is_branch() || i->is_indirect_branch()) continue;
            if (!i->is_call()) incoming[i->target].push_back(pc);
        }
    }
    std::unordered_map<uint32_t,const rpx::RpxReloc*> lows;
    for (const auto& r:m_module.relocations)
        if (r.type==elf::R_PPC_ADDR16_LO && (r.offset&3u)==2) lows.emplace(r.offset,&r);
    for (const auto& high:m_module.relocations) {
        if ((high.type!=elf::R_PPC_ADDR16_HA && high.type!=elf::R_PPC_ADDR16_HI) ||
            (high.offset&3u)!=2 || high.offset<2 || high.offset>UINT32_MAX-24) continue;
        auto target=relocation_target(high);auto upper=instruction(high.offset-2);
        if (!target || !is_valid_code_addr(*target) || m_candidates.contains(*target) ||
            !upper || upper->mnemonic!=Mnemonic::LIS || upper->rD==1 || upper->rD==2 || upper->rD==13) continue;
        // The exact HA/LO relocation pair and decoded arithmetic prove the
        // address is taken whichever GPR carries it (callers often keep the
        // pointer in a nonvolatile register across a call).
        bool formed_pointer=false;
        for (uint32_t distance=4;distance<=24;distance+=4) {
            uint32_t pc=high.offset-2+distance;auto low=instruction(pc);
            if (!low || !incoming[pc].empty()) break;
            auto pair=lows.find(pc+2);
            if (pair!=lows.end() && relocation_target(*pair->second)==target) {
                uint32_t formed=uint32_t(upper->imm)<<16;
                // ADDI's RA=0 is a literal zero; ADDIC reads the actual GPR0.
                // This only changes matching relocation arithmetic, not the
                // existing frameless body, separator or ownership proof.
                bool signed_add=(low->mnemonic==Mnemonic::ADDI && upper->rD!=0) ||
                                low->mnemonic==Mnemonic::ADDIC;
                if (signed_add && low->rD==upper->rD &&
                    low->rA==upper->rD && high.type==elf::R_PPC_ADDR16_HA) formed+=uint32_t(low->imm);
                else if (low->mnemonic==Mnemonic::ORI && low->rD==upper->rD && low->rA==upper->rD &&
                         high.type==elf::R_PPC_ADDR16_HI) formed|=low->uimm;
                else break;
                formed_pointer=formed==*target;break;
            }
            // Independent straight-line setup may separate the two halves.
            // No call, branch, unknown effect or read/clobber of the upper GPR.
            // A nonrecord MR (argument copy) writes RA from RS (decoded rD).
            if (low->mnemonic==Mnemonic::MR) {
                if (low->rA==upper->rD || low->rD==upper->rD || !volatile_reg(low->rA)) break;
                continue;
            }
            bool setup=low->mnemonic==Mnemonic::LI || low->mnemonic==Mnemonic::LIS ||
                       low->mnemonic==Mnemonic::ADDI || low->mnemonic==Mnemonic::LWZ;
            if (!setup || low->rD==upper->rD ||
                ((low->mnemonic==Mnemonic::ADDI || low->mnemonic==Mnemonic::LWZ) && low->rA==upper->rD)) break;
        }
        auto first=instruction(*target);
        if (!formed_pointer || *target<8 || !first || first->is_branch()) continue;
        auto previous=instruction(*target-4);
        bool separated=previous && previous->mnemonic==Mnemonic::BLR && !previous->lk;
        // A nonlink unconditional branch is also a no-fallthrough barrier.
        // Its label may be another function's local tail case; it need not
        // be callable. Keep its destination an existing candidate. The new
        // callback still requires its own exact relocation-backed pointer,
        // complete bounded body and no external body/interior entry below.
        if (!separated && previous && !previous->lk &&
            (previous->mnemonic==Mnemonic::B || previous->mnemonic==Mnemonic::BA) &&
            m_candidates.contains(previous->target)) separated=true;
        if (!separated) continue;
        std::set<uint32_t> body;
        std::set<std::pair<uint32_t,bool>> active,done;
        bool tail_ctr=false, ctr_set=false;
        std::function<bool(uint32_t,bool)> visit=[&](uint32_t pc,bool compared) {
            if (pc<*target || uint64_t(pc)-*target>=64 || (pc!=*target && m_candidates.contains(pc))) return false;
            auto key=std::pair(pc,compared);
            if (active.contains(key)) return false;
            if (done.contains(key)) return true;
            auto i=instruction(pc);
            if (!i || i->is_call()) return false;
            active.insert(key);body.insert(pc);
            bool ok=false;
            if (i->mnemonic==Mnemonic::BLR) ok=true;
            // Indirect tail (adjustor/vtable thunk): LR is untouched; CTR must
            // come from an MTCTR inside this body (checked after the walk).
            else if (i->mnemonic==Mnemonic::BCTR && !i->lk && (i->bo&~1u)==20) {ok=true;tail_ctr=true;}
            else if (i->mnemonic==Mnemonic::MTCTR) {
                ok=volatile_reg(i->rD) && visit(pc+4,compared);
                if (ok) ctr_set=true;
            }
            else if (i->mnemonic==Mnemonic::MR_) {
                // Record-form copy writes CR0 from a volatile result.
                ok=volatile_reg(i->rA) && volatile_reg(i->rD) && visit(pc+4,true);
            }
            else if (i->mnemonic==Mnemonic::BCLR) {
                ok=compared && ((i->bo&~1u)==4 || (i->bo&~1u)==12) && i->bi<4 && visit(pc+4,compared);
            } else if (i->mnemonic==Mnemonic::CMPI || i->mnemonic==Mnemonic::CMPLI) {
                ok=i->rD==0 && volatile_reg(i->rA) && visit(pc+4,true);
            } else if (i->mnemonic==Mnemonic::CMP || i->mnemonic==Mnemonic::CMPL) {
                // Register compare into CR0 of two volatile GPRs.
                ok=i->rD==0 && volatile_reg(i->rA) && volatile_reg(i->rB) && visit(pc+4,true);
            } else if (i->mnemonic==Mnemonic::BC || i->mnemonic==Mnemonic::BCA) {
                ok=compared && ((i->bo&~1u)==4 || (i->bo&~1u)==12) && i->bi<4 &&
                   i->target>pc && visit(i->target,compared) && visit(pc+4,compared);
            } else if (i->mnemonic==Mnemonic::B || i->mnemonic==Mnemonic::BA) {
                ok=i->target>pc && visit(i->target,compared);
            } else {
                bool memory=i->mnemonic==Mnemonic::LWZ || i->mnemonic==Mnemonic::LWZU ||
                            i->mnemonic==Mnemonic::LHZ || i->mnemonic==Mnemonic::LHA || i->mnemonic==Mnemonic::LBZ ||
                            i->mnemonic==Mnemonic::STW || i->mnemonic==Mnemonic::STH || i->mnemonic==Mnemonic::STB;
                // Nonrecord, non-OE register arithmetic on volatile GPRs only
                // (e.g. a comparator returning the difference of two keys).
                // Carrying forms only touch XER.CA, which no caller relies on
                // across a call boundary (branch-free compare/select idioms).
                bool arithmetic=(i->mnemonic==Mnemonic::ADD || i->mnemonic==Mnemonic::SUBF ||
                                 i->mnemonic==Mnemonic::ADDC || i->mnemonic==Mnemonic::ADDE ||
                                 i->mnemonic==Mnemonic::SUBFC || i->mnemonic==Mnemonic::SUBFE ||
                                 i->mnemonic==Mnemonic::NEG) && !i->oe &&
                                volatile_reg(i->rD) && volatile_reg(i->rA) && volatile_reg(i->rB);
                ok=((memory && volatile_reg(i->rD) && volatile_reg(i->rA) &&
                     (i->mnemonic!=Mnemonic::LWZU || (i->rA!=0 && i->rA!=i->rD))) || plain_setup(*i) ||
                    arithmetic || i->mnemonic==Mnemonic::NOP) && visit(pc+4,compared);
            }
            active.erase(key);if (ok) done.insert(key);return ok;
        };
        if (!visit(*target,false) || (tail_ctr && !ctr_set)) continue;
        bool shared=false;
        for (uint32_t pc:body)
            for (uint32_t source:incoming[pc]) if (!body.contains(source)) shared=true;
        if (shared) continue;
        add_candidate(*target,{});++m_stats.from_code_ptr_relocs;
    }
}

void FunctionDiscovery::scan_address_taken_backward_leaf_thunks() {
    using ppc::Mnemonic;
    auto instruction=[&](uint32_t pc)->std::optional<ppc::Instruction> {
        auto section=m_module.section_at_addr(pc);
        if (!is_valid_code_addr(pc) || !section || uint64_t(pc-section->address)+4>section->data.size()) return {};
        auto word=m_module.read_word(pc);
        return word?ppc::decode(*word,pc):std::nullopt;
    };
    auto value=[&](const rpx::RpxReloc& r)->std::optional<uint32_t> {
        if (r.sym_index<m_module.symbols.size()) return m_module.symbols[r.sym_index].address+uint32_t(r.addend);
        if (r.sym_value) return *r.sym_value+uint32_t(r.addend);
        return {};
    };
    std::map<uint32_t,std::vector<uint32_t>> incoming;
    std::set<uint32_t> independent=m_candidates;
    std::unordered_map<uint32_t,const rpx::RpxReloc*> lows;
    std::map<uint32_t,unsigned> relocation_count;
    std::map<uint32_t,std::vector<const rpx::RpxReloc*>> evidence;
    for (const auto& r:m_module.relocations) {
        ++relocation_count[r.offset];
        if (r.type==elf::R_PPC_ADDR16_LO && (r.offset&3u)==2) lows.emplace(r.offset,&r);
        if (r.type!=elf::R_PPC_ADDR32 && r.type!=elf::R_PPC_ADDR16_HA &&
            r.type!=elf::R_PPC_ADDR16_HI && r.type!=elf::R_PPC_ADDR16_LO) continue;
        auto target=value(r);
        if (!target || !is_valid_code_addr(*target)) continue;
        independent.insert(*target);
        if (r.type!=elf::R_PPC_ADDR16_LO) evidence[*target].push_back(&r);
    }
    // Include all direct entries and allocated data references, not just the
    // previously accepted callbacks. Computed local case stubs are direct B
    // instructions too, and must not become a second owner of this leaf.
    for (const auto& s:m_module.sections) {
        if (!s.is_allocated()) continue;
        for (uint32_t off=0;uint64_t(off)+4<=s.data.size();off+=4) {
            uint32_t pc=s.address+off;auto word=m_module.read_word(pc);
            if (!word) continue;
            if (!s.is_executable()) {
                if (is_valid_code_addr(*word)) independent.insert(*word);
            } else if (*word>>26==18 || *word>>26==16) {
                auto branch=instruction(pc);
                if (!branch) continue;
                incoming[branch->target].push_back(pc);
                if (branch->is_call()) independent.insert(branch->target);
            }
        }
    }
    auto plain_effect=[](const ppc::Instruction& i,uint32_t& reads,uint32_t& writes) {
        reads=writes=0;
        auto bit=[](unsigned reg){return uint32_t{1}<<reg;};
        switch (i.mnemonic) {
        case Mnemonic::NOP:return true;
        case Mnemonic::LI:case Mnemonic::LIS:writes=bit(i.rD);return true;
        case Mnemonic::ADDI:case Mnemonic::ADDIS:
        case Mnemonic::LWZ:case Mnemonic::LBZ:case Mnemonic::LHZ:case Mnemonic::LHA:
            writes=bit(i.rD);if (i.rA) reads=bit(i.rA);return true;
        case Mnemonic::STW:case Mnemonic::STH:case Mnemonic::STB:
            reads=bit(i.rD);if (i.rA) reads|=bit(i.rA);return true;
        case Mnemonic::MR:case Mnemonic::RLWINM:
            reads=bit(i.rD);writes=bit(i.rA);return true;
        default:return false; // No implicit LR/CTR/XER/CR or update effects.
        }
    };
    constexpr uint32_t protected_registers=(uint32_t{1}<<1)|(uint32_t{1}<<2)|0xFFFFE000u;
    for (const auto& [entry,references]:evidence) {
        auto stub=instruction(entry);
        if (!stub || (stub->mnemonic!=Mnemonic::B && stub->mnemonic!=Mnemonic::BA) || stub->lk || stub->target>=entry) continue;
        uint32_t pointer_pc=references.front()->offset;
        auto report=[&](const char* reason,uint32_t pc,bool accepted=false) {
            if (m_cfg.thunk_audit) m_cfg.thunk_audit({entry,pointer_pc,pc,reason,accepted});
        };
        if (m_candidates.contains(entry)) {report("existing_seed_unchanged",entry);continue;}
        bool formed=false;
        for (auto r:references) {
            if (relocation_count[r->offset]!=1) continue;
            auto section=m_module.section_at_addr(r->offset);
            if (!section || !section->is_allocated()) continue;
            if (r->type==elf::R_PPC_ADDR32) {
                auto word=m_module.read_word(r->offset);
                if (!section->is_executable() && !(r->offset&3u) && word && *word==entry) {
                    formed=true;pointer_pc=r->offset;break;
                }
                continue;
            }
            if ((r->offset&3u)!=2 || r->offset<2 || r->offset>UINT32_MAX-48) continue;
            uint32_t upper_pc=r->offset-2;auto upper=instruction(upper_pc);
            if (!upper || upper->mnemonic!=Mnemonic::LIS || independent.contains(upper_pc) || !incoming[upper_pc].empty()) continue;
            for (uint32_t distance=4;distance<=48;distance+=4) {
                uint32_t pc=upper_pc+distance;auto lower=instruction(pc);
                if (!lower || m_module.section_at_addr(pc)!=section || independent.contains(pc) || !incoming[pc].empty()) break;
                auto low=lows.find(pc+2);
                if (low!=lows.end() && value(*low->second)==std::optional<uint32_t>(entry)) {
                    if (relocation_count[pc+2]!=1) break;
                    uint32_t pointer=uint32_t(upper->imm)<<16;
                    bool add=(lower->mnemonic==Mnemonic::ADDI && upper->rD!=0) || lower->mnemonic==Mnemonic::ADDIC;
                    if (r->type==elf::R_PPC_ADDR16_HA && add && lower->rD==upper->rD && lower->rA==upper->rD)
                        pointer+=uint32_t(lower->imm);
                    else if (r->type==elf::R_PPC_ADDR16_HI && lower->mnemonic==Mnemonic::ORI && lower->rD==upper->rD && lower->rA==upper->rD)
                        pointer|=lower->uimm;
                    else break;
                    formed=pointer==entry;
                    if (formed) pointer_pc=upper_pc;
                    break;
                }
                // Only the smaller explicit setup/store family is permitted
                // between halves. An rA==0 D-form base is a literal zero,
                // whereas ADDIC r0 in the matching low half reads actual r0.
                bool setup=lower->mnemonic==Mnemonic::NOP || lower->mnemonic==Mnemonic::LI ||
                    lower->mnemonic==Mnemonic::LIS || lower->mnemonic==Mnemonic::ADDI ||
                    lower->mnemonic==Mnemonic::LWZ || lower->mnemonic==Mnemonic::STW ||
                    lower->mnemonic==Mnemonic::STH || lower->mnemonic==Mnemonic::STB;
                uint32_t reads,writes;
                if (!setup || !plain_effect(*lower,reads,writes) || ((reads|writes)&(uint32_t{1}<<upper->rD))) break;
            }
            if (formed) break;
        }
        if (!formed) {report("pointer_pair_not_proven",pointer_pc);continue;}
        auto section=m_module.section_at_addr(entry);
        uint32_t body=stub->target;
        auto before=entry>=4?instruction(entry-4):std::nullopt;
        auto separator=body>=4?instruction(body-4):std::nullopt;
        if (entry-body>64 || m_module.section_at_addr(body)!=section || !before || !separator ||
            before->mnemonic!=Mnemonic::BLR || separator->mnemonic!=Mnemonic::BLR) {
            report("leaf_boundary_not_proven",body);continue;
        }
        if (!incoming[entry].empty()) {report("thunk_has_direct_local_entry",entry);continue;}
        bool valid=true;
        for (uint32_t pc=body;pc<entry;pc+=4) {
            if (independent.contains(pc)) {report("leaf_has_independent_entry",pc);valid=false;break;}
            for (uint32_t source:incoming[pc]) {
                if (pc==body && source==entry) continue;
                report("leaf_has_foreign_control_flow",pc);valid=false;break;
            }
            if (!valid) break;
            auto i=instruction(pc);uint32_t reads,writes;
            if (pc==entry-4) {
                if (!i || i->mnemonic!=Mnemonic::BLR || i->lk) {
                    report("leaf_has_no_exact_return",pc);valid=false;
                }
            } else if (!i || !plain_effect(*i,reads,writes) || ((reads|writes)&protected_registers)) {
                report("leaf_has_unproved_state_effect",pc);valid=false;break;
            }
        }
        if (!valid) continue;
        // Only this independently referenced callable thunk is registered.
        // CFGBuilder follows its backward edge; the earlier leaf stays local.
        add_candidate(entry,{});
        ++m_stats.from_backward_leaf_thunks;
        report("isolated_backward_leaf_proven",entry,true);
    }
}

void FunctionDiscovery::scan_address_taken_tail_callbacks() {
    using ppc::Mnemonic;
    auto instruction=[&](uint32_t pc)->std::optional<ppc::Instruction> {
        auto sec=m_module.section_at_addr(pc);
        if (!is_valid_code_addr(pc) || !sec || uint64_t(pc-sec->address)+4>sec->data.size()) return {};
        auto word=m_module.read_word(pc);
        return word?ppc::decode(*word,pc):std::nullopt;
    };
    auto target=[&](const rpx::RpxReloc& r)->std::optional<uint32_t> {
        if (r.sym_index<m_module.symbols.size()) return m_module.symbols[r.sym_index].address+uint32_t(r.addend);
        if (r.sym_value) return *r.sym_value+uint32_t(r.addend);
        return {};
    };
    // A heuristic data pointer/STWU/local label alone cannot prove a tail
    // destination. Require an independently callable seed already discovered.
    std::set<uint32_t> callable;
    for (const auto& s:m_module.symbols)
        if (m_cfg.analyze_symbol_table && s.is_function()) callable.insert(s.address);
    for (const auto& e:m_module.exports)
        if (m_cfg.analyze_exports && !e.is_data) callable.insert(e.address);
    callable.insert(m_cross_function_tails.begin(),m_cross_function_tails.end());
    std::unordered_map<uint32_t,std::vector<uint32_t>> incoming;
    for (const auto& s:m_module.sections) {
        if (!s.is_executable()) continue;
        for (uint32_t off=0;uint64_t(off)+4<=s.data.size();off+=4) {
            auto i=instruction(s.address+off);
            if (!i || !i->is_branch() || i->is_indirect_branch()) continue;
            if (i->is_call()) callable.insert(i->target);
            else incoming[i->target].push_back(s.address+off);
        }
    }
    std::erase_if(callable,[&](uint32_t pc){return !m_candidates.contains(pc) || !is_valid_code_addr(pc);});
    std::set<uint32_t> import_stubs;
    for (const auto& imp:m_module.imports)
        if (!imp.is_data) import_stubs.insert(imp.stub_address);
    // A discovered entry with a frame prologue (mflr r0 / stwu r1,-N(r1))
    // right after a terminal separator is an independent function even when
    // only tails reach it (Gambit 0209CC88 after a bctr).
    for (uint32_t pc:m_candidates) {
        if (pc<4 || callable.contains(pc)) continue;
        auto previous=instruction(pc-4), head=instruction(pc);
        if (!previous || !head || previous->lk) continue;
        const bool separated=previous->mnemonic==Mnemonic::BLR || previous->mnemonic==Mnemonic::B ||
            previous->mnemonic==Mnemonic::BA || (previous->mnemonic==Mnemonic::BCTR && (previous->bo&~1u)==20);
        const bool prologue=(head->mnemonic==Mnemonic::MFLR && head->rD==0) ||
            (head->mnemonic==Mnemonic::STWU && head->rD==1 && head->rA==1 && head->imm<0);
        if (separated && prologue) callable.insert(pc);
    }
    std::unordered_map<uint32_t,const rpx::RpxReloc*> lows;
    for (const auto& r:m_module.relocations)
        if (r.type==elf::R_PPC_ADDR16_LO && (r.offset&3u)==2) lows.emplace(r.offset,&r);
    std::set<uint32_t> pointers, volatile_setup_pointers;
    for (const auto& high:m_module.relocations) {
        if ((high.type!=elf::R_PPC_ADDR16_HA && high.type!=elf::R_PPC_ADDR16_HI) ||
            (high.offset&3u)!=2 || high.offset<2 || high.offset>UINT32_MAX-64) continue;
        auto value=target(high);
        auto upper=instruction(high.offset-2);
        if (!value || !is_valid_code_addr(*value) || !upper || upper->mnemonic!=Mnemonic::LIS) continue;
        bool volatile_setup=upper->rD!=1 && upper->rD!=2 && upper->rD!=13;
        // A matching low half may follow independent setup/stores within 64
        // bytes (Gambit 027AEC30..027AEC4C is 28). No
        // branch into the window, call, unknown operation or upper-register
        // use/clobber is allowed. Do not broaden the existing leaf validator.
        for (uint32_t distance=4;distance<=64;distance+=4) {
            uint32_t pc=high.offset-2+distance;
            auto low=instruction(pc);
            if (!low || !incoming[pc].empty()) break;
            auto pair=lows.find(pc+2);
            if (pair!=lows.end() && target(*pair->second)==value) {
                uint32_t formed=uint32_t(upper->imm)<<16;
                bool add=(low->mnemonic==Mnemonic::ADDI && upper->rD!=0) || low->mnemonic==Mnemonic::ADDIC;
                if (add && low->rD==upper->rD && low->rA==upper->rD && high.type==elf::R_PPC_ADDR16_HA)
                    formed+=uint32_t(low->imm);
                else if (low->mnemonic==Mnemonic::ORI && low->rD==upper->rD && low->rA==upper->rD &&
                         high.type==elf::R_PPC_ADDR16_HI) formed|=low->uimm;
                else break;
                if (formed==*value) {
                    pointers.insert(*value);
                    if (volatile_setup) volatile_setup_pointers.insert(*value);
                }
                break;
            }
            // MR is OR RA,RS,RS: RA is the destination, unlike D-form
            // setup instructions. Only an independent nonrecord alias can
            // leave the upper pointer value and its control state intact.
            if (low->mnemonic==Mnemonic::MR && !low->rc && low->rD==low->rB &&
                low->rD!=upper->rD && low->rA!=upper->rD) {
                volatile_setup&=low->rA==0 || (low->rA>=3 && low->rA<=12);
                continue;
            }
            // Non-updating stores change memory only. They may separate the
            // halves when neither their base nor (integer) source is the upper GPR.
            bool int_store=low->mnemonic==Mnemonic::STW || low->mnemonic==Mnemonic::STH ||
                           low->mnemonic==Mnemonic::STB;
            bool fp_store=low->mnemonic==Mnemonic::STFS || low->mnemonic==Mnemonic::STFD;
            if ((int_store || fp_store) && low->rA!=upper->rD && (fp_store || low->rD!=upper->rD)) continue;
            // Non-updating indexed integer stores (Gambit stbx at 0209C3CC):
            // memory only, when neither base, index nor source is the upper GPR.
            bool indexed_store=low->mnemonic==Mnemonic::STWX || low->mnemonic==Mnemonic::STHX ||
                               low->mnemonic==Mnemonic::STBX;
            if (indexed_store && low->rA!=upper->rD && low->rB!=upper->rD && low->rD!=upper->rD) continue;
            // Frame save (Gambit stmw r24,0x28(r1) at 027B268C): memory only,
            // when the upper GPR is neither the base nor a stored register.
            if (low->mnemonic==Mnemonic::STMW && low->rA!=upper->rD && upper->rD<low->rD) continue;
            // addic/addic. (Gambit 027B2800) write rD, XER.CA and CR0 only.
            if ((low->mnemonic==Mnemonic::ADDIC || low->mnemonic==Mnemonic::ADDIC_) &&
                low->rD!=upper->rD && low->rA!=upper->rD) continue;
            // Non-record X-form integer arithmetic (Gambit subfe r3,r12,r31 at
            // 02A90468) writes rD and at most XER.CA, reads rA/rB: independent
            // when none of the three is the upper GPR.
            const bool xform=low->mnemonic==Mnemonic::SUBF || low->mnemonic==Mnemonic::SUBFC ||
                low->mnemonic==Mnemonic::SUBFE || low->mnemonic==Mnemonic::ADD || low->mnemonic==Mnemonic::ADDE;
            if (xform && low->rD!=upper->rD && low->rA!=upper->rD && low->rB!=upper->rD) continue;
            bool setup=low->mnemonic==Mnemonic::LIS || low->mnemonic==Mnemonic::LI ||
                       low->mnemonic==Mnemonic::ADDI || low->mnemonic==Mnemonic::LWZ;
            if (!setup || low->rD==upper->rD ||
                ((low->mnemonic==Mnemonic::ADDI || low->mnemonic==Mnemonic::LWZ) && low->rA==upper->rD)) break;
        }
    }
    auto terminal=[&](uint32_t pc) {
        auto i=instruction(pc);
        return i && (i->mnemonic==Mnemonic::BLR ||
            (i->mnemonic==Mnemonic::BCTR && !i->lk && (i->bo&~1u)==20) ||
            ((i->mnemonic==Mnemonic::B || i->mnemonic==Mnemonic::BA) &&
             (callable.contains(i->target) || pointers.contains(i->target))));
    };
    auto volatile_reg=[](uint32_t r){return r==0 || (r>=3 && r<=12);};
    for (uint32_t addr:pointers) {
        if (m_candidates.contains(addr) || addr<4 || !terminal(addr-4)) continue;
        auto first=instruction(addr);
        if (!first) continue;
        if (first->mnemonic==Mnemonic::BLR) {
            // Address-taken empty callback (Gambit 026F8704): a lone blr behind
            // a terminal separator with no branch into it.
            if (incoming[addr].empty()) {
                add_candidate(addr,{});
                ++m_stats.from_tail_callbacks;
            }
            continue;
        }
        if (first->is_branch()) {
            // Address-taken one-instruction thunk (Gambit 02A07600 -> 02A075E8):
            // a relocation-backed pointer, behind a terminal separator, nothing
            // branches into it, and a non-linking B to an already discovered
            // entry. The tail keeps the incoming LR like any tail callback.
            if ((first->mnemonic==Mnemonic::B || first->mnemonic==Mnemonic::BA) &&
                incoming[addr].empty() && m_candidates.contains(first->target) &&
                first->target!=addr) {
                add_candidate(addr,{});
                ++m_stats.from_tail_callbacks;
            }
            continue;
        }
        // Body window; Gambit 026FDC48 places its last tail at +0x40. A tail
        // must still leave the original 64-byte callback span (tail_exit).
        constexpr uint32_t max_bytes=128, tail_exit=64;
        std::set<uint32_t> body, tails;
        std::set<std::pair<uint32_t,bool>> active, done;
        bool conditional=false, guarded_return=false, plain_return=false, indirect_tail=false;
        bool update_load=false, unproved_update_arithmetic=false;
        std::function<bool(uint32_t,bool)> visit=[&](uint32_t pc,bool compared) {
            if (pc<addr || uint64_t(pc)-addr>=max_bytes || (pc!=addr && m_candidates.contains(pc))) return false;
            auto key=std::pair(pc,compared);
            if (active.contains(key)) return false;
            if (done.contains(key)) return true;
            auto i=instruction(pc);
            if (!i || i->is_call()) return false;
            active.insert(key); body.insert(pc);
            bool ok=false;
            if (i->mnemonic==Mnemonic::B || i->mnemonic==Mnemonic::BA) {
                // Terminal tail only; local unconditional jumps are outside
                // this deliberately small family.
                // A function import stub (.fimport, not data) is callable
                // too (Gambit 02A7B590 tails into OSSignalEvent).
                ok=(callable.contains(i->target) || import_stubs.contains(i->target)) &&
                   (i->target<addr || uint64_t(i->target)-addr>=tail_exit);
                if (ok) tails.insert(i->target);
            } else if (i->mnemonic==Mnemonic::BCLR) {
                // Frameless return-or-tail callbacks preserve the incoming LR.
                // Prove the CR0 producer and exclude CTR/link dependencies.
                ok=compared && ((i->bo&~1u)==12 || (i->bo&~1u)==4) && i->bi<4 &&
                   visit(pc+4,compared);
                conditional=true;
                if (ok) guarded_return=true;
            } else if (i->mnemonic==Mnemonic::BC || i->mnemonic==Mnemonic::BCA) {
                ok=compared && ((i->bo&~1u)==12 || (i->bo&~1u)==4) && i->bi<4 &&
                   i->target>pc && visit(i->target,compared) && visit(pc+4,compared);
                conditional=true;
            } else if (i->mnemonic==Mnemonic::BLR) {
                // Plain return (Gambit 026FDC48): frameless, LR never written.
                ok=true; plain_return=true;
            } else if (i->mnemonic==Mnemonic::MTCTR) {
                ok=volatile_reg(i->rD) && visit(pc+4,compared);
            } else if (i->mnemonic==Mnemonic::BCTR && !i->lk && (i->bo&~1u)==20) {
                // Object-method tail: every entry to this branch must execute
                // its adjacent CTR producer. Preserve the incoming LR.
                auto producer=instruction(pc-4);
                ok=producer && producer->mnemonic==Mnemonic::MTCTR &&
                   volatile_reg(producer->rD) && incoming[pc].empty();
                indirect_tail|=ok;
            } else if ((i->mnemonic==Mnemonic::LFS || i->mnemonic==Mnemonic::LFD) &&
                       i->rD<14 && i->rA!=1 && volatile_reg(i->rA)) {
                ok=visit(pc+4,compared);
            } else if ((i->mnemonic==Mnemonic::STFS || i->mnemonic==Mnemonic::STFD) &&
                       i->rA!=1 && volatile_reg(i->rA)) {
                // Non-updating publication reads the FPR and writes no GPR.
                ok=visit(pc+4,compared);
            } else if (i->mnemonic==Mnemonic::MULLI && volatile_reg(i->rD) && volatile_reg(i->rA)) {
                ok=visit(pc+4,compared);
            } else if (i->mnemonic==Mnemonic::LWZU && volatile_reg(i->rD) &&
                       volatile_reg(i->rA) && i->rA!=0 && i->rA!=i->rD) {
                // Update both volatile RT/RA; preserve SP, SDA and CR state.
                update_load=true;
                ok=visit(pc+4,compared);
            } else if (i->mnemonic==Mnemonic::RLWINM && !i->rc &&
                       volatile_reg(i->rA) && volatile_reg(i->rD)) {
                ok=visit(pc+4,compared);
            } else if ((i->mnemonic==Mnemonic::STW || i->mnemonic==Mnemonic::STH ||
                        i->mnemonic==Mnemonic::STB) && i->rA!=1 && volatile_reg(i->rA)) {
                // Non-updating object store through a volatile, non-stack base;
                // memory effect only (any source register is merely read).
                ok=visit(pc+4,compared);
            } else if (i->mnemonic==Mnemonic::CMPI || i->mnemonic==Mnemonic::CMPLI) {
                ok=i->rD==0 && volatile_reg(i->rA) && visit(pc+4,true);
            } else if (i->mnemonic==Mnemonic::CMP || i->mnemonic==Mnemonic::CMPL) {
                // Register thresholds produce CR0 exactly like immediate
                // compares; neither operand nor LR/SP is written.
                ok=i->rD==0 && volatile_reg(i->rA) && volatile_reg(i->rB) && visit(pc+4,true);
            } else if (i->mnemonic==Mnemonic::ADDIC || i->mnemonic==Mnemonic::ADDIC_) {
                // A countdown writes volatile RT and XER.CA; addic. also
                // produces CR0. Neither form updates its base register.
                const bool record=i->mnemonic==Mnemonic::ADDIC_;
                unproved_update_arithmetic=true;
                ok=volatile_reg(i->rD) && volatile_reg(i->rA) && visit(pc+4,compared || record);
            } else if (i->mnemonic==Mnemonic::NEG && !i->oe && !i->rc) {
                ok=volatile_reg(i->rD) && volatile_reg(i->rA) && visit(pc+4,compared);
            } else if (i->mnemonic==Mnemonic::ANDC && !i->rc) {
                // Logical X-form: RA is written, RS (decoded rD) and RB read.
                ok=volatile_reg(i->rA) && volatile_reg(i->rD) && volatile_reg(i->rB) && visit(pc+4,compared);
            } else if ((i->mnemonic==Mnemonic::EXTSB || i->mnemonic==Mnemonic::EXTSH) && !i->rc) {
                ok=volatile_reg(i->rA) && volatile_reg(i->rD) && visit(pc+4,compared);
            } else if (i->mnemonic==Mnemonic::MFCR) {
                // Read a locally produced comparison without modifying any
                // CR field, including nonvolatile CR2-CR4.
                ok=compared && volatile_reg(i->rD) && visit(pc+4,compared);
            } else if (i->mnemonic==Mnemonic::RLWINM_) {
                // Record-form rotate writes RA and CR0, never LR/CTR.
                ok=volatile_reg(i->rA) && volatile_reg(i->rD) && visit(pc+4,true);
            } else if (i->mnemonic==Mnemonic::LFS || i->mnemonic==Mnemonic::LFD) {
                // Non-updating FP argument load: volatile FPR, volatile base.
                ok=i->rD<14 && volatile_reg(i->rA) && visit(pc+4,compared);
            } else if (i->mnemonic==Mnemonic::MR && !i->rc && i->rD==i->rB) {
                // Argument move (Gambit 027B3214: mr r5,r4). MR is OR RA,RS,RS:
                // RA is written, RS only read.
                ok=volatile_reg(i->rA) && visit(pc+4,compared);
            } else if ((i->mnemonic==Mnemonic::FCMPU || i->mnemonic==Mnemonic::FCMPO) && i->rD==0) {
                // FP compare into CR0 (Gambit 02C4F15C: fcmpu cr0,f1,f3).
                ok=visit(pc+4,true);
            } else if (!i->rc && i->rD<14 &&
                       (i->mnemonic==Mnemonic::FADD || i->mnemonic==Mnemonic::FADDS ||
                        i->mnemonic==Mnemonic::FSUB || i->mnemonic==Mnemonic::FSUBS ||
                        i->mnemonic==Mnemonic::FMUL || i->mnemonic==Mnemonic::FMULS ||
                        i->mnemonic==Mnemonic::FSEL || i->mnemonic==Mnemonic::FMR ||
                        i->mnemonic==Mnemonic::FNEG || i->mnemonic==Mnemonic::FABS)) {
                // Non-record FP arithmetic writes only a volatile FPR.
                ok=visit(pc+4,compared);
            } else if (i->mnemonic==Mnemonic::SUBF || i->mnemonic==Mnemonic::SUBFC ||
                       i->mnemonic==Mnemonic::ADD || i->mnemonic==Mnemonic::SUBF_ ||
                       i->mnemonic==Mnemonic::ADD_) {
                // Comparator arithmetic (Gambit 02D01518: subf. r0,r11,r0 /
                // subf r0,r12,r10): volatile GPRs only; the record forms
                // produce CR0 for a following guarded branch.
                const bool record=i->mnemonic==Mnemonic::SUBF_ || i->mnemonic==Mnemonic::ADD_;
                unproved_update_arithmetic|=i->oe || (record && compared);
                ok=volatile_reg(i->rD) && volatile_reg(i->rA) && volatile_reg(i->rB) &&
                   visit(pc+4,compared || record);
            } else {
                bool setup=i->mnemonic==Mnemonic::LIS || i->mnemonic==Mnemonic::LI ||
                           i->mnemonic==Mnemonic::ADDI || i->mnemonic==Mnemonic::ORI || i->mnemonic==Mnemonic::LWZ ||
                           i->mnemonic==Mnemonic::LBZ || i->mnemonic==Mnemonic::LHZ || i->mnemonic==Mnemonic::LHA;
                ok=setup && volatile_reg(i->rD) && volatile_reg(i->rA) && visit(pc+4,compared);
            }
            active.erase(key);
            if (ok) done.insert(key);
            return ok;
        };
        // Unconditional argument-setup thunk: straight-line volatile setup and
        // one terminal tail to an independently callable function. The tail
        // keeps the incoming LR; the pointer, separator and ownership proofs
        // below are the same as for the conditional family.
        // FP conversion leaves may keep LR untouched while using a small
        // scratch frame. Validate separately so the frameless family retains
        // its original instruction/window/forward-branch restrictions.
        auto frame_head=instruction(addr+4);
        const bool fp_frame=first->mnemonic==Mnemonic::FCMPU && first->rD==0 &&
            frame_head && frame_head->mnemonic==Mnemonic::STWU &&
            frame_head->rD==1 && frame_head->rA==1 && frame_head->imm<=-8 &&
            frame_head->imm>=-256 && (frame_head->imm&7)==0;
        if (fp_frame) {
            const int frame_size=-frame_head->imm;
            // -1 means no proven frame-relative pointer. Copy state at forks;
            // merging by PC alone could hide an unbalanced incoming path.
            using Aliases=std::array<int,32>;
            using State=std::tuple<uint32_t,int,Aliases>;
            std::set<State> finished;
            std::set<uint32_t> visiting;
            std::function<bool(uint32_t,int,Aliases)> framed;
            framed=[&](uint32_t pc,int frame,Aliases aliases) {
                if (pc<addr || uint64_t(pc)-addr>=256 ||
                    (pc!=addr && pc!=addr+4 && m_candidates.contains(pc)) ||
                    visiting.contains(pc)) return false;
                State state{pc,frame,aliases};
                if (finished.contains(state)) return true;
                auto op=instruction(pc);
                if (!op || op->is_call()) return false;
                const auto& i=*op;
                visiting.insert(pc); body.insert(pc);
                auto next=[&]{return framed(pc+4,frame,aliases);};
                auto scratch=[&](int offset,int width) {
                    return frame>0 && offset>=4 && offset<=frame_size-width;
                };
                bool ok=false;
                if (pc==addr) {
                    ok=next(); // the entry FCMPU establishes CR0 on every path
                } else if (pc==addr+4) {
                    ok=frame==0 && framed(pc+4,frame_size,aliases);
                } else if (i.mnemonic==Mnemonic::ADDI && i.rD==1 && i.rA==1) {
                    // Exactly one restoration; invalidate all scratch aliases.
                    aliases.fill(-1);
                    ok=frame>0 && i.imm==frame_size && framed(pc+4,-1,aliases);
                } else if (i.mnemonic==Mnemonic::BLR) {
                    ok=frame==-1; plain_return|=ok;
                } else if (i.mnemonic==Mnemonic::BC || i.mnemonic==Mnemonic::BCA) {
                    conditional=true;
                    ok=((i.bo&~1u)==12 || (i.bo&~1u)==4) && i.bi<4 &&
                        framed(i.target,frame,aliases) && next();
                } else if (i.mnemonic==Mnemonic::FCMPU || i.mnemonic==Mnemonic::FCMPO) {
                    ok=i.rD==0 && next();
                } else if (i.mnemonic==Mnemonic::ADDI && volatile_reg(i.rD) && i.rA==1) {
                    aliases[i.rD]=i.imm;
                    ok=scratch(i.imm,4) && next();
                } else if (i.mnemonic==Mnemonic::STFIWX) {
                    // Indexed conversion store must use a proven SP-derived
                    // address (RA=0 has zero-base semantics, RB=0 reads r0).
                    ok=i.rA==0 && volatile_reg(i.rB) &&
                        scratch(aliases[i.rB],4) && next();
                } else if (i.mnemonic==Mnemonic::STW && i.rA==1) {
                    ok=scratch(i.imm,4) && next();
                } else if (i.mnemonic==Mnemonic::LWZ && i.rA==1 && volatile_reg(i.rD)) {
                    aliases[i.rD]=-1;
                    ok=scratch(i.imm,4) && next();
                } else if (i.mnemonic==Mnemonic::LFS || i.mnemonic==Mnemonic::LFD) {
                    const int width=i.mnemonic==Mnemonic::LFS?4:8;
                    const bool base=i.rA==1?scratch(i.imm,width):
                        volatile_reg(i.rA) && (i.rA==0 || aliases[i.rA]<0);
                    ok=i.rD<14 && base && next();
                } else if (!i.rc && i.rD<14 &&
                    (i.mnemonic==Mnemonic::FADD || i.mnemonic==Mnemonic::FADDS ||
                     i.mnemonic==Mnemonic::FSUB || i.mnemonic==Mnemonic::FSUBS ||
                     i.mnemonic==Mnemonic::FMUL || i.mnemonic==Mnemonic::FMULS ||
                     i.mnemonic==Mnemonic::FDIV || i.mnemonic==Mnemonic::FDIVS ||
                     i.mnemonic==Mnemonic::FMADD || i.mnemonic==Mnemonic::FMADDS ||
                     i.mnemonic==Mnemonic::FMSUB || i.mnemonic==Mnemonic::FMSUBS ||
                     i.mnemonic==Mnemonic::FNMADD || i.mnemonic==Mnemonic::FNMADDS ||
                     i.mnemonic==Mnemonic::FNMSUB || i.mnemonic==Mnemonic::FNMSUBS ||
                     i.mnemonic==Mnemonic::FMR || i.mnemonic==Mnemonic::FNEG ||
                     i.mnemonic==Mnemonic::FABS || i.mnemonic==Mnemonic::FRSP ||
                     i.mnemonic==Mnemonic::FCTIWZ)) {
                    ok=next();
                } else if (i.mnemonic==Mnemonic::LIS || i.mnemonic==Mnemonic::LI) {
                    aliases[i.rD]=-1;
                    ok=volatile_reg(i.rD) && next();
                } else if (i.mnemonic==Mnemonic::XORIS) {
                    // Logical immediates write RA, read RS (decoded rD).
                    aliases[i.rA]=-1;
                    ok=volatile_reg(i.rA) && volatile_reg(i.rD) && next();
                }
                visiting.erase(pc);
                if (ok) finished.insert(state);
                return ok;
            };
            Aliases aliases; aliases.fill(-1);
            if (!framed(addr,0,aliases)) continue;
        } else if (!visit(addr,false)) continue;
        // The added updating-load family retains the original separated-leaf
        // restrictions on pointer setup and implicit guard/overflow changes.
        // Existing non-updating tail families keep their established proofs.
        if (update_load && (!volatile_setup_pointers.contains(addr) || unproved_update_arithmetic)) continue;
        const auto tail_count=tails.size()+(indirect_tail?1u:0u);
        const bool argument_thunk=!conditional && tail_count==1;
        // Straight-line frameless getter/setter ending in a plain blr
        // (Gambit 027AF110: li r0,1; stb r0,0x1F9(r3); blr).
        // A guarded frameless leaf whose every path ends in a plain blr
        // (Gambit comparator 02D01518) is the same shape with forward
        // conditional branches inside the body.
        const bool plain_leaf=tail_count==0 && plain_return;
        if (!argument_thunk && !plain_leaf && (!conditional ||
            (tail_count<2 && !(guarded_return && tail_count==1) && !plain_return))) continue;
        bool owned=false;
        for (uint32_t pc:body) {
            for (uint32_t source:incoming[pc]) {
                if (body.contains(source)) continue;
                // Exception only for a separately relocation-backed, isolated
                // one-instruction callable thunk targeting the entry itself.
                auto stub=instruction(source);
                bool independent=pc==addr && pointers.contains(source) && source>=4 &&
                    !body.contains(source) && incoming[source].empty() && terminal(source-4) && stub &&
                    (stub->mnemonic==Mnemonic::B || stub->mnemonic==Mnemonic::BA) && stub->target==addr;
                if (!independent) owned=true;
            }
        }
        if (owned) continue;
        add_candidate(addr,{});
        ++m_stats.from_tail_callbacks;
    }
}

void FunctionDiscovery::scan_corroborated_entries() {
    using ppc::Mnemonic;
    if (m_cfg.analyzer_entries.empty()) return;
    // Independent evidence 1: something references the address (relocation
    // target or code-address word in allocated data).
    std::set<uint32_t> referenced;
    for (const auto& r : m_module.relocations) {
        if (r.type!=elf::R_PPC_ADDR32 && r.type!=elf::R_PPC_ADDR16_HA &&
            r.type!=elf::R_PPC_ADDR16_HI && r.type!=elf::R_PPC_ADDR16_LO) continue;
        std::optional<uint32_t> value;
        if (r.sym_index<m_module.symbols.size()) value=m_module.symbols[r.sym_index].address+uint32_t(r.addend);
        else if (r.sym_value) value=*r.sym_value+uint32_t(r.addend);
        if (value && is_valid_code_addr(*value)) referenced.insert(*value);
    }
    // Non-linking branch sources per target; true = unconditional relative B.
    std::unordered_map<uint32_t,std::vector<std::pair<uint32_t,bool>>> incoming;
    for (const auto& s : m_module.sections) {
        if (!s.is_allocated()) continue;
        for (uint32_t off=0;uint64_t(off)+4<=s.data.size();off+=4) {
            const uint32_t pc=s.address+off;
            auto word=m_module.read_word(pc);
            if (!word) continue;
            if (!s.is_executable()) {
                if (is_valid_code_addr(*word)) referenced.insert(*word);
            } else if ((*word>>26==18 || *word>>26==16) && !(*word&1u)) {
                if (auto branch=ppc::decode(*word,pc))
                    incoming[branch->target].push_back({pc,branch->mnemonic==Mnemonic::B});
            }
        }
    }
    // A branch crosses functions when a discovered entry behind a separator
    // (no fallthrough into it) lies between its source and the target; the
    // source's own entry counts when the source follows the target. Interior
    // STWU aliases follow MFLR and do not count. Such a B is a tail transfer,
    // never an interior label of the source's body.
    auto boundary=[&](uint32_t entry) {
        auto word=entry>=4 ? m_module.read_word(entry-4) : std::nullopt;
        auto previous=word ? ppc::decode(*word,entry-4) : std::nullopt;
        return previous && !previous->lk && (previous->mnemonic==Mnemonic::BLR ||
            previous->mnemonic==Mnemonic::B || previous->mnemonic==Mnemonic::BA ||
            (previous->mnemonic==Mnemonic::BCTR && (previous->bo&~1u)==20));
    };
    auto cross_function=[&](uint32_t source,uint32_t addr) {
        const uint32_t low=std::min(source,addr), high=std::max(source,addr);
        for (auto entry=m_candidates.upper_bound(low);entry!=m_candidates.end() && *entry<=high;++entry)
            if (*entry!=addr && (source>addr || *entry<addr) && boundary(*entry)) return true;
        return false;
    };
    for (uint32_t addr : m_cfg.analyzer_entries) {
        if (m_candidates.contains(addr) || addr<4 || !is_valid_code_addr(addr)) continue;
        // Independent evidence 1: a reference, or only cross-function tail
        // branches (any local or conditional branch makes it an interior label).
        const auto sources=incoming.find(addr);
        bool tail_only=false;
        if (sources!=incoming.end()) {
            tail_only=std::all_of(sources->second.begin(),sources->second.end(),[&](const auto& source) {
                return source.second && cross_function(source.first,addr);
            });
            if (!tail_only) continue;
        } else if (!referenced.contains(addr)) continue;
        // Independent evidence 2: no fallthrough from a previous body.
        auto word=m_module.read_word(addr-4);
        auto previous=word ? ppc::decode(*word,addr-4) : std::nullopt;
        if (!previous || previous->lk) continue;
        const bool separated=previous->mnemonic==Mnemonic::BLR || previous->mnemonic==Mnemonic::B ||
            previous->mnemonic==Mnemonic::BA || (previous->mnemonic==Mnemonic::BCTR && (previous->bo&~1u)==20);
        if (!separated) continue;
        add_candidate(addr,{});
        if (tail_only) m_cross_function_tails.insert(addr);
        ++m_stats.from_corroborated_entries;
    }
}

}
