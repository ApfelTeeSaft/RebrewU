#pragma once

#include "ir_function.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>

// ============================================================================
// RebrewU — Wii U static recompilation framework
// ir_module.hpp — IRModule: a collection of IRFunctions for one RPX/RPL
// ============================================================================

namespace rebrewu::ir {

/// A data section captured from the original binary.
struct IRDataSection {
    std::string  name{};         // e.g. ".rodata", ".data"
    uint32_t     guest_addr{0};  // base virtual address
    std::vector<uint8_t> data{}; // raw bytes (host order, already decompressed)
    bool         read_only{false};
    bool         executable{false};
};

/// The complete IR representation of one RPX or RPL module.
struct IRModule {
    std::string  name{};     // module name (without extension)
    bool         is_rpx{false}; // true if executable (RPX), false if library (RPL)

    std::vector<IRFunction>    functions{};
    std::vector<IRDataSection> data_sections{};

    std::unordered_map<uint32_t, std::string> addr_to_name{};   // symbol table
    std::unordered_map<std::string, uint32_t> name_to_addr{};

    // Imports: module name → set of symbol names
    std::unordered_map<std::string, std::vector<std::string>> imports{};
    // Exports: symbol name → guest address
    std::unordered_map<std::string, uint32_t> exports{};

    // -----------------------------------------------------------------
    // Function management
    // -----------------------------------------------------------------

    IRFunction& add_function(std::string func_name, uint32_t entry) {
        IRFunction f;
        f.name       = std::move(func_name);
        f.entry_addr = entry;
        functions.push_back(std::move(f));
        return functions.back();
    }

    IRFunction* function_at(uint32_t addr) {
        for (auto& f : functions)
            if (f.entry_addr == addr) return &f;
        return nullptr;
    }

    const IRFunction* function_at(uint32_t addr) const {
        for (const auto& f : functions)
            if (f.entry_addr == addr) return &f;
        return nullptr;
    }

    IRFunction* function_named(const std::string& nm) {
        for (auto& f : functions)
            if (f.name == nm) return &f;
        return nullptr;
    }

    // -----------------------------------------------------------------
    // Symbol helpers
    // -----------------------------------------------------------------

    std::optional<std::string> symbol_at(uint32_t addr) const {
        auto it = addr_to_name.find(addr);
        if (it != addr_to_name.end()) return it->second;
        return {};
    }

    void add_symbol(uint32_t addr, std::string sym_name) {
        addr_to_name[addr]       = sym_name;
        name_to_addr[sym_name]   = addr;
    }
};

}