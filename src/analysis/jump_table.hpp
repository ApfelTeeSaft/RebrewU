#pragma once
#include "../ir/ir_function.hpp"
#include "../core/rpx/rpx_types.hpp"
#include <optional>
#include <vector>

namespace rebrewu::analysis {
  struct JumpTableEntry {
    uint32_t offset_from_base;  // index * 4 from table base
    uint32_t target_addr;
  };

  struct JumpTable {
    uint32_t dispatch_addr;      // address of the bctr instruction
    uint32_t table_addr;         // address of the jump table data
    uint32_t num_entries;
    std::vector<JumpTableEntry> entries;
    // Pattern that set up the bctr:
    uint32_t index_reg;          // register holding the index
    uint32_t base_reg;           // register holding table base
  };

  class JumpTableAnalyzer {
  public:
    explicit JumpTableAnalyzer(const rpx::RpxModule& module);

    // Analyze a function to find all jump tables
    std::vector<JumpTable> analyze(const ir::IRFunction& func);

    // Try to resolve a specific indirect branch as a jump table
    std::optional<JumpTable> try_resolve(
      uint32_t indirect_branch_addr,
      const ir::IRFunction& func
    );

  private:
    bool scan_pattern(uint32_t indirect_branch_addr, JumpTable& out);
    std::optional<uint32_t> read_word(uint32_t addr) const;
    bool is_rodata_addr(uint32_t addr) const;

    const rpx::RpxModule& m_module;
  };
}
