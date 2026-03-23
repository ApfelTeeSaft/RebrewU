#pragma once

#include "ir_types.hpp"
#include <string>
#include <vector>
#include <deque>
#include <unordered_map>
#include <optional>

// ============================================================================
// RebrewU — Wii U static recompilation framework
// ir_function.hpp — IRFunction: a control-flow graph of BasicBlocks
// ============================================================================

namespace rebrewu::ir {

/// A single recompiled function represented as a CFG of BasicBlocks.
struct IRFunction {
    std::string  name{};
    uint32_t     entry_addr{0};   // guest address of function entry

    std::deque<BasicBlock>          blocks{};  // deque preserves refs/ptrs on push_back
    std::unordered_map<uint32_t,uint32_t> addr_to_block{}; // guest addr -> block id

    uint32_t next_temp_id{0};
    uint32_t next_block_id{0};

    // -----------------------------------------------------------------
    // Block management
    // -----------------------------------------------------------------

    BasicBlock& add_block(uint32_t guest_start = 0) {
        BasicBlock blk;
        blk.id          = next_block_id++;
        blk.guest_start = guest_start;
        blocks.push_back(std::move(blk));
        if (guest_start)
            addr_to_block[guest_start] = blocks.back().id;
        return blocks.back();
    }

    BasicBlock* block_by_id(uint32_t id) {
        for (auto& b : blocks)
            if (b.id == id) return &b;
        return nullptr;
    }

    const BasicBlock* block_by_id(uint32_t id) const {
        for (const auto& b : blocks)
            if (b.id == id) return &b;
        return nullptr;
    }

    BasicBlock* block_at_addr(uint32_t addr) {
        auto it = addr_to_block.find(addr);
        return it != addr_to_block.end() ? block_by_id(it->second) : nullptr;
    }

    // -----------------------------------------------------------------
    // VReg allocation
    // -----------------------------------------------------------------

    VReg alloc_temp() { return VReg::temp(next_temp_id++); }

    // -----------------------------------------------------------------
    // Queries
    // -----------------------------------------------------------------

    bool empty() const noexcept { return blocks.empty(); }

    const BasicBlock* entry_block() const {
        for (const auto& b : blocks)
            if (b.is_entry) return &b;
        return blocks.empty() ? nullptr : &blocks.front();
    }
};

}