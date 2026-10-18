#include "audit.hpp"
#include "ppc/semantics/ppc_semantics.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <map>
#include <set>
using namespace rebrewu;
using nlohmann::json;
bool AuditInventory::write(const rpx::RpxModule& rpx, const ir::IRModule& module,
                          const analysis::CFGBuilder& cfg, const std::filesystem::path& output) const {
    json j; j["schema"]=1; j["module"]=rpx.name; j["entry"]=rpx.entry_point;
    j["scope"]="Static candidates, not runtime reachability. Linear text sweep may contain data. No behavior or registry changes.";
    j["functions"]=json::array(); j["frames"]=json::array(); j["indirect"]=json::array();
    j["references"]=json::array(); j["imports"]=json::array(); j["semantics"]=json::array();
    j["backward_leaf_thunks"]=json::array();
    std::set<uint32_t> entries, reachable;
    std::map<unsigned,std::pair<unsigned,unsigned>> histogram;
    for(const auto& f:module.functions) {
        entries.insert(f.entry_addr);json ranges=json::array();
        for(const auto& b:f.blocks) {
            ranges.push_back({b.guest_start,b.guest_end});
            for(uint32_t pc=b.guest_start;pc<b.guest_end;pc+=4) reachable.insert(pc);
        }
        j["functions"].push_back({{"entry",f.entry_addr},{"ranges",ranges}});
    }
    for(const auto& f:frames) {
        auto word=rpx.read_word(f.pc);auto insn=word?ppc::decode(*word,f.pc):std::nullopt;
        j["frames"].push_back({{"entry",f.entry},{"stack",f.stack},{"pc",f.pc},
            {"reason",f.reason},{"accepted",f.accepted},{"mnemonic",insn?unsigned(insn->mnemonic):0}});
    }
    for(const auto& f:thunks)
        j["backward_leaf_thunks"].push_back({{"entry",f.entry},{"pointer_pc",f.pointer_pc},
            {"pc",f.pc},{"reason",f.reason},{"accepted",f.accepted}});
    for(const auto& s:rpx.sections) {
        if(!s.is_allocated())continue;
        for(uint32_t off=0;uint64_t(off)+4<=s.data.size();off+=4) {
            auto pc=s.address+off;auto word=rpx.read_word(pc);if(!word)continue;
            if(!s.is_executable()) {
                if((*word&3)==0 && rpx.is_code_addr(*word))
                    j["references"].push_back({{"source",pc},{"target",*word},{"kind","data_word"},{"discovered",entries.contains(*word)}});
                continue;
            }
            auto insn=ppc::decode(*word,pc);unsigned mnemonic=insn?unsigned(insn->mnemonic):0;
            auto& h=histogram[mnemonic];++h.first;if(reachable.contains(pc))++h.second;
            if(!insn) { if(reachable.contains(pc))j["semantics"].push_back({{"pc",pc},{"mnemonic",0},{"reason","decode_unknown_reachable"}});continue; }
            if(insn->mnemonic==ppc::Mnemonic::BCTR || insn->mnemonic==ppc::Mnemonic::BCTRL) {
                auto proof=cfg.audit_inline_branch(pc);
                json site={{"pc",pc},{"link",insn->lk},{"reachable",reachable.contains(pc)},
                    {"reason",insn->lk?"dynamic_call_requires_runtime_target":proof.reason},{"local_targets",proof.targets}};
                // Only an exact adjacent LIS/ADDI/MTCTR straight-line producer.
                // Dynamic object/array fields are intentionally unresolved.
                auto at=[&](uint32_t a){auto w=rpx.read_word(a);return w?ppc::decode(*w,a):std::nullopt;};
                if(pc>=12 && proof.targets.empty()) {
                    auto high=at(pc-12),low=at(pc-8),mt=at(pc-4);
                    if(high && low && mt && high->mnemonic==ppc::Mnemonic::LIS && high->rD!=0 &&
                        low->mnemonic==ppc::Mnemonic::ADDI && low->rD==high->rD && low->rA==high->rD &&
                        mt->mnemonic==ppc::Mnemonic::MTCTR && mt->rD==high->rD)
                        site["constant_target"]=(uint32_t(high->imm)<<16)+uint32_t(low->imm);
                }
                j["indirect"].push_back(std::move(site));
            }
            if(!reachable.contains(pc))continue;
            ir::IRFunction probe;auto& b=probe.add_block(pc);ir::IRBuilder builder(probe);builder.set_insert_point(b);
            bool ok=ppc::lower_to_ir(*insn,builder,probe);
            bool only_nop=!b.instrs.empty(), undefined=false;
            for(const auto& op:b.instrs){only_nop &= op.opcode==ir::Opcode::Nop;undefined |= op.opcode==ir::Opcode::Undefined;}
            if(!ok || undefined || (only_nop && insn->mnemonic!=ppc::Mnemonic::NOP))
                j["semantics"].push_back({{"pc",pc},{"mnemonic",mnemonic},{"reason",!ok?"lowering_returns_false":undefined?"undefined_ir":"nop_lowering_review"}});
        }
    }
    for(const auto& r:rpx.relocations) {
        if(r.type!=elf::R_PPC_ADDR32 && r.type!=elf::R_PPC_ADDR16_LO && r.type!=elf::R_PPC_ADDR16_HA && r.type!=elf::R_PPC_ADDR16_HI)continue;
        auto value=r.sym_index<rpx.symbols.size()?std::optional<uint32_t>(rpx.symbols[r.sym_index].address):r.sym_value;
        if(!value)continue;uint32_t target=*value+uint32_t(r.addend);
        if((target&3)==0 && rpx.is_code_addr(target))j["references"].push_back({{"source",r.offset},{"target",target},{"kind","relocation"},{"type",r.type},{"discovered",entries.contains(target)}});
    }
    j["histogram"]=json::array();for(const auto& [id,count]:histogram)j["histogram"].push_back({{"mnemonic",id},{"linear",count.first},{"cfg_reachable",count.second}});
    for(const auto& imp:rpx.imports)j["imports"].push_back({{"name",imp.name},{"module",imp.from_module},{"stub",imp.stub_address},{"data",imp.is_data}});
    std::filesystem::create_directories(output);std::ofstream stream(output/"inventory.json");stream<<j.dump(2)<<'\n';return bool(stream);
}
