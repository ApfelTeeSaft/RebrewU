#include "function.h"
#include "disasm.h"
#include "wiiu_ppc.h"
#include <vector>
#include <bit>
#include <algorithm>
#include <cassert>

size_t Function::SearchBlock(size_t address) const {
    if (address < base) {
        return static_cast<size_t>(-1);
    }

    for (size_t i = 0; i < blocks.size(); i++) {
        const auto& block = blocks[i];
        const auto begin = base + block.base;
        const auto end = begin + block.size;

        if (begin != end) {
            if (address >= begin && address < end) {
                return i;
            }
        } else { // fresh block
            if (address == begin) {
                return i;
            }
        }
    }

    return static_cast<size_t>(-1);
}

Function Function::Analyze(const void* code, size_t size, size_t base) {
    Function fn{ base, 0 };

    // Quick check for simple tail call pattern
    const uint32_t* data = static_cast<const uint32_t*>(code);
    if (size >= 8 && ByteSwap(data[1]) == 0x04000048) { // shifted ptr tail call
        fn.size = 0x8;
        return fn;
    }

    auto& blocks = fn.blocks;
    blocks.reserve(8);
    blocks.emplace_back();

    const auto* dataStart = data;
    const auto* dataEnd = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(code) + size);
    std::vector<size_t> blockStack{};
    blockStack.reserve(32);
    blockStack.emplace_back(0);

    auto restoreData = [&]() {
        if (!blockStack.empty()) {
            data = dataStart + ((blocks[blockStack.back()].base + blocks[blockStack.back()].size) / sizeof(*data)) - 1;
        }
    };

    // Analyze control flow
    for (; data <= dataEnd; ++data) {
        const size_t addr = base + ((data - dataStart) * sizeof(*data));
        if (blockStack.empty()) {
            break; // Analysis complete
        }

        auto& curBlock = blocks[blockStack.back()];
        DEBUG(const auto blockBase = curBlock.base);
        const uint32_t instruction = ByteSwap(*data);

        const uint32_t op = PPC_OP(instruction);
        const uint32_t isLink = PPC_BL(instruction); // call

        ppc_insn insn;
        ppc::Disassemble(data, addr, insn);

        assert(addr == base + curBlock.base + curBlock.size);
        if (curBlock.projectedSize != static_cast<size_t>(-1) && 
            curBlock.size >= curBlock.projectedSize) { // fallthrough
            blockStack.pop_back();
            restoreData();
            continue;
        }

        curBlock.size += 4;
        
        if (op == PPC_OP_BC) { // conditional branches
            if (isLink) { // just a conditional call, nothing to see here
                continue;
            }

            // TODO: carry projections over to false case
            curBlock.projectedSize = static_cast<size_t>(-1);
            blockStack.pop_back();

            // TODO: Handle absolute branches?
            assert(!PPC_BA(instruction));
            const size_t branchDest = addr + PPC_BD(instruction);

            // true/false paths
            // left block: false case
            // right block: true case
            const size_t lBase = (addr - base) + 4;
            const size_t rBase = (addr + PPC_BD(instruction)) - base;

            // these will be -1 if it's our first time seeing these blocks
            auto lBlock = fn.SearchBlock(base + lBase);

            if (lBlock == static_cast<size_t>(-1)) {
                blocks.emplace_back(lBase, 0).projectedSize = rBase - lBase;
                lBlock = blocks.size() - 1;

                // push this first, this gets overridden by the true case as it'd be further away
                DEBUG(blocks[lBlock].parent = blockBase);
                blockStack.emplace_back(lBlock);
            }

            size_t rBlock = fn.SearchBlock(base + rBase);
            if (rBlock == static_cast<size_t>(-1)) {
                blocks.emplace_back(branchDest - base, 0);
                rBlock = blocks.size() - 1;

                DEBUG(blocks[rBlock].parent = blockBase);
                blockStack.emplace_back(rBlock);
            }

            restoreData();
        }
        else if (op == PPC_OP_B || instruction == 0 || 
                (op == PPC_OP_CTR && (PPC_XOP(instruction) == 16 || PPC_XOP(instruction) == 528))) { 
            // b, blr, end padding
            if (!isLink) {
                blockStack.pop_back();

                if (op == PPC_OP_B) {
                    assert(!PPC_BA(instruction));
                    const size_t branchDest = addr + PPC_BI(instruction);
                    const size_t branchBase = branchDest - base;

                    if (branchDest < base) {
                        // Branches before base are just tail calls, no need to chase after those
                        restoreData();
                        continue;
                    }

                    const size_t branchBlock = fn.SearchBlock(branchDest);

                    // carry over our projection if blocks are next to each other
                    const bool isContinuous = branchBase == curBlock.base + curBlock.size;
                    size_t sizeProjection = static_cast<size_t>(-1);

                    if (curBlock.projectedSize != static_cast<size_t>(-1) && isContinuous) {
                        sizeProjection = curBlock.projectedSize - curBlock.size;
                    }

                    if (branchBlock == static_cast<size_t>(-1)) {
                        blocks.emplace_back(branchBase, 0, sizeProjection);
                        blockStack.emplace_back(blocks.size() - 1);
                        
                        DEBUG(blocks.back().parent = blockBase);
                        restoreData();
                        continue;
                    }
                }
                else if (op == PPC_OP_CTR) {
                    // 5th bit of BO tells cpu to ignore the counter
                    const bool conditional = !(PPC_BO(instruction) & 0x10);
                    if (conditional) {
                        // right block's just going to return
                        const size_t lBase = (addr - base) + 4;
                        size_t lBlock = fn.SearchBlock(lBase);
                        if (lBlock == static_cast<size_t>(-1)) {
                            blocks.emplace_back(lBase, 0);
                            lBlock = blocks.size() - 1;

                            DEBUG(blocks[lBlock].parent = blockBase);
                            blockStack.emplace_back(lBlock);
                            restoreData();
                            continue;
                        }
                    }
                }

                restoreData();
            }
        }
        else if (insn.opcode == nullptr) {
            blockStack.pop_back();
            restoreData();
        }
    }

    // Sort and invalidate discontinuous blocks
    if (blocks.size() > 1) {
        std::sort(blocks.begin(), blocks.end(), [](const Block& a, const Block& b) {
            return a.base < b.base;
        });

        size_t discontinuity = static_cast<size_t>(-1);
        for (size_t i = 0; i < blocks.size() - 1; i++) {
            if (blocks[i].base + blocks[i].size >= blocks[i + 1].base) {
                continue;
            }

            discontinuity = i + 1;
            break;
        }

        if (discontinuity != static_cast<size_t>(-1)) {
            blocks.erase(blocks.begin() + discontinuity, blocks.end());
        }
    }

    fn.size = 0;
    for (const auto& block : blocks) {
        // pick the block furthest away
        fn.size = std::max(fn.size, block.base + block.size);
    }
    
    return fn;
}

std::vector<size_t> Function::GetBranchTargets() const {
    std::vector<size_t> targets;
    
    for (const auto& block : blocks) {
        size_t addr = base + block.base;
        const size_t endAddr = addr + block.size;
        
        // This would require access to the actual code data
        // For now, return empty vector
        // TODO: Implement proper branch target extraction
    }
    
    return targets;
}

void Function::MergeOverlappingBlocks() {
    if (blocks.size() <= 1) {
        return;
    }
    
    std::sort(blocks.begin(), blocks.end(), [](const Block& a, const Block& b) {
        return a.base < b.base;
    });
    
    std::vector<Block> merged;
    merged.reserve(blocks.size());
    merged.push_back(blocks[0]);
    
    for (size_t i = 1; i < blocks.size(); i++) {
        Block& last = merged.back();
        const Block& current = blocks[i];
        
        if (current.base <= last.base + last.size) {
            // Overlapping or adjacent blocks - merge them
            last.size = std::max(last.base + last.size, current.base + current.size) - last.base;
        } else {
            // Non-overlapping block
            merged.push_back(current);
        }
    }
    
    blocks = std::move(merged);
}

bool Function::Validate() const {
    if (base == 0 || size == 0 || blocks.empty()) {
        return false;
    }
    
    // Check that all blocks are within the function bounds
    for (const auto& block : blocks) {
        if (block.base >= size || block.base + block.size > size) {
            return false;
        }
    }
    
    // Check that blocks are properly ordered and don't overlap
    for (size_t i = 1; i < blocks.size(); i++) {
        if (blocks[i].base < blocks[i-1].base + blocks[i-1].size) {
            return false; // Overlapping blocks
        }
    }
    
    return true;
}