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
    struct Config {
      bool follow_calls;
      bool follow_branch_targets;
      bool analyze_symbol_table;
      bool analyze_exports;
      bool analyze_imports;
      uint32_t max_function_size;
      std::vector<FunctionHint> hints;
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
      uint32_t from_data_ptrs;
      uint32_t total;
    };
    Stats stats() const { return m_stats; }

  private:
    void scan_calls(uint32_t start, uint32_t end);
    void scan_prologues();
    void scan_data_pointers();
    void add_candidate(uint32_t addr, std::string name, bool force=false);
    bool is_valid_code_addr(uint32_t addr) const;

    const rpx::RpxModule& m_module;
    const linker::Linker& m_linker;
    Config m_cfg;
    std::set<uint32_t> m_candidates;
    std::unordered_map<uint32_t, std::string> m_names;
    Stats m_stats{};
  };
}
