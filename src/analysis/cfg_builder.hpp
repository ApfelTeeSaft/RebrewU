#pragma once
#include "../ir/ir_function.hpp"
#include "../ir/ir_module.hpp"
#include "../core/rpx/rpx_types.hpp"
#include "../ppc/decoder/ppc_decode.hpp"
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>

namespace rebrewu::analysis {
  class CFGBuilder {
  public:
    struct Config {
      uint32_t max_instructions_per_function;
      bool strict_mode;
      Config() noexcept : max_instructions_per_function(50000), strict_mode(false) {}
    };

    explicit CFGBuilder(const rpx::RpxModule& module, Config cfg = {});

    // Build IR function from a known function address
    std::optional<ir::IRFunction> build(uint32_t entry_addr, std::string_view name = "");

    // Get last error
    std::string_view last_error() const { return m_last_error; }

  private:
    // linear scan to find all block leaders
    std::set<uint32_t> find_leaders(uint32_t entry, uint32_t text_end);
    // decode and split into blocks
    void build_blocks(ir::IRFunction& func, const std::set<uint32_t>& leaders, uint32_t text_end);
    // lower PPC instrs to IR
    void lower_instruction(ir::BasicBlock& block, const ppc::Instruction& instr, ir::IRFunction& func);
    // resolve edges
    void resolve_edges(ir::IRFunction& func);

    const rpx::RpxModule& m_module;
    Config m_cfg;
    std::string m_last_error;

    // Helper: get bytes at guest addr
    std::optional<uint32_t> read_word(uint32_t addr) const;
    bool is_code_addr(uint32_t addr) const;
  };
}
