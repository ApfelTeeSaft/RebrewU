#include "pch.h"
#include "recompiler_config.h"

void RecompilerConfig::Load(const std::string_view& configFilePath) {
    directoryPath = configFilePath.substr(0, configFilePath.find_last_of("\\/") + 1);
    
    try {
        toml::table toml = toml::parse_file(configFilePath);
        
        if (auto mainPtr = toml["main"].as_table()) {
            const auto& main = *mainPtr;
            
            // File paths
            filePath = main["file_path"].value_or<std::string>("");
            outDirectoryPath = main["out_directory_path"].value_or<std::string>("");
            switchTableFilePath = main["switch_table_file_path"].value_or<std::string>("");
            
            // Optimization settings
            skipLr = main["skip_lr"].value_or(false);
            skipMsr = main["skip_msr"].value_or(false);
            ctrAsLocalVariable = main["ctr_as_local"].value_or(false);
            xerAsLocalVariable = main["xer_as_local"].value_or(false);
            reservedRegisterAsLocalVariable = main["reserved_as_local"].value_or(false);
            crRegistersAsLocalVariables = main["cr_as_local"].value_or(false);
            nonArgumentRegistersAsLocalVariables = main["non_argument_as_local"].value_or(false);
            nonVolatileRegistersAsLocalVariables = main["non_volatile_as_local"].value_or(false);
            
            // WiiU system function addresses
            restGpr14Address = main["restgprlr_14_address"].value_or(0u);
            saveGpr14Address = main["savegprlr_14_address"].value_or(0u);
            restFpr14Address = main["restfpr_14_address"].value_or(0u);
            saveFpr14Address = main["savefpr_14_address"].value_or(0u);
            restVmx14Address = main["restvmx_14_address"].value_or(0u);
            saveVmx14Address = main["savevmx_14_address"].value_or(0u);
            restVmx64Address = main["restvmx_64_address"].value_or(0u);
            saveVmx64Address = main["savevmx_64_address"].value_or(0u);
            longJmpAddress = main["longjmp_address"].value_or(0u);
            setJmpAddress = main["setjmp_address"].value_or(0u);
            
            // WiiU-specific GQR function addresses
            for (int i = 0; i < 8; i++) {
                auto gqr_load_key = fmt::format("gqr_{}_load_address", i);
                auto gqr_store_key = fmt::format("gqr_{}_store_address", i);
                gqr_load_functions[i] = main[gqr_load_key].value_or(0u);
                gqr_store_functions[i] = main[gqr_store_key].value_or(0u);
            }
            
            // WiiU memory layout
            mem1_base = main["mem1_base"].value_or(0x00800000u);
            mem1_size = main["mem1_size"].value_or(0x01800000u);
            mem2_base = main["mem2_base"].value_or(0x10000000u);
            mem2_size = main["mem2_size"].value_or(0x20000000u);
            
            // Code generation settings
            generatePairedSingleSupport = main["generate_paired_single_support"].value_or(true);
            generateGQRSupport = main["generate_gqr_support"].value_or(true);
            optimizeForWiiUHardware = main["optimize_for_wiiu_hardware"].value_or(true);
            enableCacheOptimizations = main["enable_cache_optimizations"].value_or(false);
            
            // Advanced settings
            treatUnknownInstructionsAsNop = main["treat_unknown_instructions_as_nop"].value_or(false);
            generateDebugInfo = main["generate_debug_info"].value_or(false);
            maxFunctionSize = main["max_function_size"].value_or(0x10000u);
            
            // Validate critical addresses
            if (restGpr14Address == 0) fmt::println("WARNING: __restgprlr_14 address is unspecified");
            if (saveGpr14Address == 0) fmt::println("WARNING: __savegprlr_14 address is unspecified");
            if (restFpr14Address == 0) fmt::println("WARNING: __restfpr_14 address is unspecified");
            if (saveFpr14Address == 0) fmt::println("WARNING: __savefpr_14 address is unspecified");
            
            // Load manual function definitions
            if (auto functionsArray = main["functions"].as_array()) {
                for (auto& func : *functionsArray) {
                    auto& funcTable = *func.as_table();
                    uint32_t address = *funcTable["address"].value<uint32_t>();
                    uint32_t size = *funcTable["size"].value<uint32_t>();
                    functions.emplace(address, size);
                }
            }
            
            // Load invalid instruction patterns
            if (auto invalidArray = main["invalid_instructions"].as_array()) {
                for (auto& instr : *invalidArray) {
                    auto& instrTable = *instr.as_table();
                    uint32_t data = *instrTable["data"].value<uint32_t>();
                    uint32_t size = *instrTable["size"].value<uint32_t>();
                    invalidInstructions.emplace(data, size);
                }
            }
            
            // Load switch table definitions
            if (!switchTableFilePath.empty()) {
                try {
                    toml::table switchToml = toml::parse_file(directoryPath + switchTableFilePath);
                    if (auto switchArray = switchToml["switch"].as_array()) {
                        for (auto& entry : *switchArray) {
                            auto& table = *entry.as_table();
                            RecompilerSwitchTable switchTable;
                            switchTable.r = *table["r"].value<uint32_t>();
                            
                            if (auto labelsArray = table["labels"].as_array()) {
                                for (auto& label : *labelsArray) {
                                    switchTable.labels.push_back(*label.value<uint32_t>());
                                }
                            }
                            
                            switchTables.emplace(*table["base"].value<uint32_t>(), std::move(switchTable));
                        }
                    }
                } catch (const std::exception& e) {
                    fmt::println("WARNING: Could not load switch table file '{}': {}", switchTableFilePath, e.what());
                }
            }
        }
        
        // Load mid-assembly hooks
        if (auto midAsmHookArray = toml["midasm_hook"].as_array()) {
            for (auto& entry : *midAsmHookArray) {
                auto& table = *entry.as_table();
                
                RecompilerMidAsmHook midAsmHook;
                midAsmHook.name = *table["name"].value<std::string>();
                
                if (auto registerArray = table["registers"].as_array()) {
                    for (auto& reg : *registerArray) {
                        midAsmHook.registers.push_back(*reg.value<std::string>());
                    }
                }
                
                midAsmHook.ret = table["return"].value_or(false);
                midAsmHook.returnOnTrue = table["return_on_true"].value_or(false);
                midAsmHook.returnOnFalse = table["return_on_false"].value_or(false);
                
                midAsmHook.jumpAddress = table["jump_address"].value_or(0u);
                midAsmHook.jumpAddressOnTrue = table["jump_address_on_true"].value_or(0u);
                midAsmHook.jumpAddressOnFalse = table["jump_address_on_false"].value_or(0u);
                
                midAsmHook.afterInstruction = table["after_instruction"].value_or(false);
                
                // Validate hook configuration
                if ((midAsmHook.ret && midAsmHook.jumpAddress != 0) ||
                    (midAsmHook.returnOnTrue && midAsmHook.jumpAddressOnTrue != 0) ||
                    (midAsmHook.returnOnFalse && midAsmHook.jumpAddressOnFalse != 0)) {
                    fmt::println("WARNING: {}: can't return and jump at the same time", midAsmHook.name);
                }
                
                if ((midAsmHook.ret || midAsmHook.jumpAddress != 0) &&
                    (midAsmHook.returnOnFalse || midAsmHook.returnOnTrue ||
                     midAsmHook.jumpAddressOnFalse != 0 || midAsmHook.jumpAddressOnTrue != 0)) {
                    fmt::println("WARNING: {}: can't mix direct and conditional return/jump", midAsmHook.name);
                }
                
                midAsmHooks.emplace(*table["address"].value<uint32_t>(), std::move(midAsmHook));
            }
        }
        
    } catch (const std::exception& e) {
        fmt::println("ERROR: Failed to parse configuration file '{}': {}", configFilePath, e.what());
        throw;
    }
}

bool RecompilerConfig::Validate() const {
    // Check required file paths
    if (filePath.empty()) {
        fmt::println("ERROR: file_path is required");
        return false;
    }
    
    if (outDirectoryPath.empty()) {
        fmt::println("ERROR: out_directory_path is required");
        return false;
    }
    
    // Check if input file exists
    if (!std::filesystem::exists(directoryPath + filePath)) {
        fmt::println("ERROR: Input file '{}' does not exist", directoryPath + filePath);
        return false;
    }
    
    // Check if output directory exists
    if (!std::filesystem::exists(directoryPath + outDirectoryPath)) {
        fmt::println("ERROR: Output directory '{}' does not exist", directoryPath + outDirectoryPath);
        return false;
    }
    
    // Validate memory layout
    if (mem1_size == 0 || mem2_size == 0) {
        fmt::println("ERROR: Invalid memory layout configuration");
        return false;
    }
    
    if (mem1_base >= mem1_base + mem1_size || mem2_base >= mem2_base + mem2_size) {
        fmt::println("ERROR: Memory region overflow in configuration");
        return false;
    }
    
    // Validate function addresses are in valid memory ranges
    auto validateAddress = [this](uint32_t addr, const char* name) -> bool {
        if (addr != 0 && !IsValidAddress(addr)) {
            fmt::println("WARNING: {} address 0x{:X} is outside valid memory ranges", name, addr);
            return false;
        }
        return true;
    };
    
    validateAddress(restGpr14Address, "__restgprlr_14");
    validateAddress(saveGpr14Address, "__savegprlr_14");
    validateAddress(restFpr14Address, "__restfpr_14");
    validateAddress(saveFpr14Address, "__savefpr_14");
    validateAddress(restVmx14Address, "__restvmx_14");
    validateAddress(saveVmx14Address, "__savevmx_14");
    validateAddress(longJmpAddress, "longjmp");
    validateAddress(setJmpAddress, "setjmp");
    
    // Validate GQR function addresses
    for (int i = 0; i < 8; i++) {
        validateAddress(gqr_load_functions[i], fmt::format("gqr_{}_load", i).c_str());
        validateAddress(gqr_store_functions[i], fmt::format("gqr_{}_store", i).c_str());
    }
    
    // Validate manual function definitions
    for (const auto& [address, size] : functions) {
        if (!IsValidAddress(address)) {
            fmt::println("WARNING: Manual function at 0x{:X} is outside valid memory ranges", address);
        }
        if (size == 0 || size > maxFunctionSize) {
            fmt::println("WARNING: Manual function at 0x{:X} has invalid size 0x{:X}", address, size);
        }
    }
    
    return true;
}