#pragma once

#include "rpx_image.h"
#include "function.h"
#include <vector>
#include <string>

// RPX-specific analysis tools for WiiU executables
class RPXAnalyzer {
public:
    explicit RPXAnalyzer(RPXImage& image);
    
    // Analyze the RPX file and extract functions
    bool AnalyzeFunctions();
    
    // Detect and analyze jump tables
    bool DetectJumpTables();
    
    // Find system function addresses (runtime functions)
    bool FindSystemFunctions();
    
    // Analyze imports and exports
    bool AnalyzeImportsExports();
    
    // Generate analysis report
    std::string GenerateReport() const;
    
    // Get analyzed functions
    const std::vector<Function>& GetFunctions() const { return functions; }
    
    // Get detected jump tables
    const std::vector<JumpTable>& GetJumpTables() const { return jump_tables; }
    
    // Get system function addresses
    const SystemFunctions& GetSystemFunctions() const { return system_functions; }

private:
    struct JumpTable {
        uint32_t base_address;
        uint32_t table_address;
        uint32_t register_used;
        uint32_t default_target;
        std::vector<uint32_t> targets;
        enum Type { Absolute, Computed, ByteOffset, ShortOffset } type;
    };
    
    struct SystemFunctions {
        uint32_t restgprlr_14 = 0;
        uint32_t savegprlr_14 = 0;
        uint32_t restfpr_14 = 0;
        uint32_t savefpr_14 = 0;
        uint32_t restvmx_14 = 0;
        uint32_t savevmx_14 = 0;
        uint32_t longjmp = 0;
        uint32_t setjmp = 0;
    };
    
    RPXImage& image;
    std::vector<Function> functions;
    std::vector<JumpTable> jump_tables;
    SystemFunctions system_functions;
    
    // Analysis helper methods
    bool AnalyzeCodeSection(const Section& section);
    bool FindFunctionBoundaries(const Section& section);
    Function AnalyzeSingleFunction(uint32_t address, const uint8_t* data, size_t max_size);
    
    // Jump table detection
    bool ScanForJumpTablePatterns(const Section& section);
    bool AnalyzeJumpTableAt(uint32_t address, JumpTable& table);
    bool ValidateJumpTable(const JumpTable& table);
    
    // System function detection
    bool FindRestoreFunction(const char* name, uint32_t pattern[], size_t pattern_size, uint32_t& address);
    bool FindSaveFunction(const char* name, uint32_t pattern[], size_t pattern_size, uint32_t& address);
    
    // Utility methods
    const uint8_t* GetCodePointer(uint32_t address) const;
    bool IsValidCodeAddress(uint32_t address) const;
    bool IsInCodeSection(uint32_t address) const;
    std::string FormatAddress(uint32_t address) const;
};