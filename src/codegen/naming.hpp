#pragma once
#include "../ir/ir_function.hpp"
#include "../core/rpx/rpx_types.hpp"
#include <string>
#include <unordered_map>
#include <set>

namespace rebrewu::codegen {
  class NamingContext {
  public:
    explicit NamingContext(std::string_view module_name);

    // Get C-safe name for a function
    std::string function_name(uint32_t addr, std::string_view hint = "") const;

    // Get C-safe name for a block within a function
    std::string block_label(uint32_t func_addr, uint32_t block_addr) const;

    // Get name for a data symbol
    std::string data_name(uint32_t addr, std::string_view hint = "") const;

    // Register an explicit name override
    void set_name(uint32_t addr, std::string name);

    // Sanitize a name to be C-identifier safe
    static std::string sanitize(std::string_view name);

    // Check if name is taken
    bool is_name_used(std::string_view name) const;

  private:
    std::string m_module_prefix;
    std::unordered_map<uint32_t, std::string> m_overrides;
    mutable std::unordered_map<uint32_t, std::string> m_cache;
    mutable std::set<std::string> m_used_names;
  };
}
