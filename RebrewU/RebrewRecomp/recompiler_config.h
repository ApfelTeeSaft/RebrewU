#pragma once

#include "pch.h"

// WiiU-specific switch table for jump table handling
struct RecompilerSwitchTable {
    uint32_t r;
    std::vector<uint32_t> labels;
};

// Mid-assembly hook for custom implementations
struct RecompilerMidAsmHook {
    std::string name;
    std::vector<std::string> registers;

    bool ret = false;
    bool returnOnTrue = false;
    bool returnOnFalse = false;

    uint32_t jumpAddress = 0;
    uint32_t jumpAddressOnTrue = 0;
    uint32_t jumpAddressOnFalse = 0;

    bool afterInstruction = false;
};

// Main configuration structure for WiiU recompilation
struct RecompilerConfig {
    // File paths
    std::string directoryPath;
    std::string filePath;
    std::string outDirectoryPath;
    std::string switchTableFilePath;
    
    // WiiU-specific settings
    std::unordered_map<uint32_t, RecompilerSwitchTable> switchTables;
    
    // Optimization flags
    bool skipLr = false;
    bool ctrAsLocalVariable = false;
    bool xerAsLocalVariable = false;
    bool reservedRegisterAsLocalVariable = false;
    bool skipMsr = false;
    bool crRegistersAsLocalVariables = false;
    bool nonArgumentRegistersAsLocalVariables = false;
    bool nonVolatileRegistersAsLocalVariables = false;
    
    // WiiU system function addresses (these are game-specific and must be found through analysis)
    uint32_t restGpr14Address = 0;
    uint32_t saveGpr14Address = 0;
    uint32_t restFpr14Address = 0;
    uint32_t saveFpr14Address = 0;
    uint32_t restVmx14Address = 0;
    uint32_t saveVmx14Address = 0;
    uint32_t restVmx64Address = 0;
    uint32_t saveVmx64Address = 0;
    uint32_t longJmpAddress = 0;
    uint32_t setJmpAddress = 0;
    
    // WiiU-specific graphics quantization register functions
    uint32_t gqr_load_functions[8] = {0};
    uint32_t gqr_store_functions[8] = {0};
    
    // Manual function definitions
    std::unordered_map<uint32_t, uint32_t> functions;
    
    // Invalid instruction patterns to skip
    std::unordered_map<uint32_t, uint32_t> invalidInstructions;
    
    // Mid-assembly hooks for custom implementations
    std::unordered_map<uint32_t, RecompilerMidAsmHook> midAsmHooks;
    
    // WiiU memory layout settings
    uint32_t mem1_base = 0x00800000;
    uint32_t mem1_size = 0x01800000;
    uint32_t mem2_base = 0x10000000;
    uint32_t mem2_size = 0x20000000;
    
    // Code generation settings
    bool generatePairedSingleSupport = true;
    bool generateGQRSupport = true;
    bool optimizeForWiiUHardware = true;
    bool enableCacheOptimizations = false;
    
    // Advanced settings
    bool treatUnknownInstructionsAsNop = false;
    bool generateDebugInfo = false;
    uint32_t maxFunctionSize = 0x10000; // 64KB max function size
    
    // Load configuration from TOML file
    void Load(const std::string_view& configFilePath);
    
    // Validate configuration
    bool Validate() const;
    
    // Get effective memory base address
    uint32_t GetMemoryBase() const { return std::min(mem1_base, mem2_base); }
    
    // Check if address is in valid memory range
    bool IsValidAddress(uint32_t address) const {
        return (address >= mem1_base && address < mem1_base + mem1_size) ||
               (address >= mem2_base && address < mem2_base + mem2_size);
    }
    
    // Get section for address
    std::string GetAddressSection(uint32_t address) const {
        if (address >= mem1_base && address < mem1_base + mem1_size) {
            return "MEM1";
        } else if (address >= mem2_base && address < mem2_base + mem2_size) {
            return "MEM2";
        }
        return "UNKNOWN";
    }
};