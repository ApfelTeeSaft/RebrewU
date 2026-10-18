#pragma once
#include "../ir/ir_module.hpp"
#include "../core/rpx/rpx_types.hpp"
#include "../core/linker/linker.hpp"
#include <functional>
#include <set>

namespace rebrewu::analysis {
  struct FunctionHint {
    uint32_t address;
    std::string name;
    bool force;  // override automatic analysis
  };

  struct FunctionBoundary {
    uint32_t start;
    uint32_t end;  // exclusive, 0 = unknown
    std::string name;
    bool is_import;
    std::string import_module;
  };

  class FunctionDiscovery {
  public:
    struct FrameAudit { uint32_t entry, stack, pc; std::string reason; bool accepted; };
    struct ThunkAudit { uint32_t entry, pointer_pc, pc; std::string reason; bool accepted; };
    struct Config {
      bool follow_calls;
      bool follow_branch_targets;
      bool analyze_symbol_table;
      bool analyze_exports;
      bool analyze_imports;
      uint32_t max_function_size;
      std::vector<FunctionHint> hints;
      // Function starts reported by an external analyzer (e.g. Ghidra). Used
      // only as corroboration: an entry is added when it is also referenced
      // and follows a no-fallthrough separator.
      std::set<uint32_t> analyzer_entries;
      // Optional observation only; never changes candidate decisions.
      std::function<void(const FrameAudit&)> frame_audit;
      std::function<void(const ThunkAudit&)> thunk_audit;
      Config() noexcept
          : follow_calls(true), follow_branch_targets(true)
          , analyze_symbol_table(true), analyze_exports(true)
          , analyze_imports(true), max_function_size(1024*1024) {}
    };


    explicit FunctionDiscovery(
      const rpx::RpxModule& module,
      const linker::Linker& linker,
      Config cfg = {}
    );

    // Run discovery, returns list of discovered function boundaries
    std::vector<FunctionBoundary> discover();

    // Add a hint manually
    void add_hint(FunctionHint hint);

    // Statistics
    struct Stats {
      uint32_t from_exports;
      uint32_t from_symbols;
      uint32_t from_call_targets;
      uint32_t from_hints;
      uint32_t from_prologues;
      uint32_t pre_stack_prologues;
      uint32_t post_stack_prologues;
      uint32_t from_data_ptrs;
      uint32_t from_code_ptr_relocs;
      uint32_t from_tail_callbacks;
      uint32_t from_backward_leaf_thunks;
      uint32_t from_corroborated_entries;
      uint32_t total;
    };
    Stats stats() const { return m_stats; }

  private:
    void scan_calls(uint32_t start, uint32_t end);
    void scan_prologues();
    void scan_data_pointers();
    void scan_code_pointer_relocations();
    void scan_frameless_callbacks();
    void scan_address_taken_tail_callbacks();
    void scan_address_taken_backward_leaf_thunks();
    void scan_corroborated_entries();
    bool is_isolated_leaf(uint32_t addr, const std::set<uint32_t>& branch_targets) const;
    void add_candidate(uint32_t addr, std::string name, bool force=false);
    bool is_valid_code_addr(uint32_t addr) const;

    const rpx::RpxModule& m_module;
    const linker::Linker& m_linker;
    Config m_cfg;
    std::set<uint32_t> m_candidates;
    // Analyzer entries reached only by tail branches from other functions;
    // independently callable for the address-taken tail-callback proof.
    std::set<uint32_t> m_cross_function_tails;
    std::unordered_map<uint32_t, std::string> m_names;
    Stats m_stats{};
  };
}
