#pragma once

#include <cstddef>
#include <vector>

#ifdef _DEBUG
#define DEBUG(X) X
#else
#define DEBUG(X)
#endif

// Function analysis for WiiU PowerPC code
struct Function {
    struct Block {
        size_t base{};
        size_t size{};
        size_t projectedSize{ static_cast<size_t>(-1) }; // Used during analysis
        DEBUG(size_t parent{});

        Block() = default;
        
        Block(size_t base, size_t size)
            : base(base), size(size) {}
        
        Block(size_t base, size_t size, size_t projectedSize) 
            : base(base), size(size), projectedSize(projectedSize) {}
    };

    size_t base{};
    size_t size{};
    std::vector<Block> blocks{};

    Function() = default;

    Function(size_t base, size_t size)
        : base(base), size(size) {}
    
    // Find which block contains the given address
    size_t SearchBlock(size_t address) const;
    
    // Analyze a function from raw code data
    static Function Analyze(const void* code, size_t size, size_t base);
    
    // Check if this function is valid
    bool IsValid() const { return size > 0 && !blocks.empty(); }
    
    // Check if this function contains the given address
    bool Contains(size_t address) const {
        return address >= base && address < base + size;
    }
    
    // Get the end address of this function
    size_t GetEndAddress() const { return base + size; }
    
    // Get all branch targets within this function
    std::vector<size_t> GetBranchTargets() const;
    
    // Check if this function has multiple entry points
    bool HasMultipleEntryPoints() const { return blocks.size() > 1; }
    
    // Merge overlapping blocks (used during analysis)
    void MergeOverlappingBlocks();
    
    // Validate function integrity
    bool Validate() const;
};