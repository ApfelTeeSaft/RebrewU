#include "rpx_analyzer.h"
#include "disasm.h"
#include <algorithm>
#include <fmt/core.h>

RPXAnalyzer::RPXAnalyzer(RPXImage& image) : image(image) {
}

bool RPXAnalyzer::AnalyzeFunctions() {
    functions.clear();
    
    for (const auto& symbol : image.symbols) {
        if (symbol.type == Symbol_Function && symbol.size > 0) {
            Function func(symbol.address, symbol.size);
            functions.push_back(func);
        }
    }
    
    for (const auto& section : image.sections) {
        if (section.flags & SectionFlags_Code) {
            AnalyzeCodeSection(section);
        }
    }
    
    std::sort(functions.begin(), functions.end(), 
              [](const Function& a, const Function& b) {
                  return a.base < b.base;
              });
    
    auto it = std::unique(functions.begin(), functions.end(),
                         [](const Function& a, const Function& b) {
                             return a.base == b.base;
                         });
    functions.erase(it, functions.end());
    
    return !functions.empty();
}

bool RPXAnalyzer::AnalyzeCodeSection(const Section& section) {
    if (!section.data || section.size == 0) {
        return false;
    }
    
    return FindFunctionBoundaries(section);
}

bool RPXAnalyzer::FindFunctionBoundaries(const Section& section) {
    const uint32_t* data = reinterpret_cast<const uint32_t*>(section.data);
    const size_t word_count = section.size / 4;
    
    // Scan for function calls (bl instructions)
    for (size_t i = 0; i < word_count; i++) {
        uint32_t instruction = ByteSwap(data[i]);
        uint32_t op = PPC_OP(instruction);
        
        if (op == PPC_OP_B && PPC_BL(instruction)) {
            // This is a function call
            uint32_t current_addr = section.base + (i * 4);
            uint32_t target_addr = current_addr + PPC_BI(instruction);
            
            // Check if target is within this section
            if (target_addr >= section.base && target_addr < section.base + section.size) {
                // Check if we already have a function at this address
                auto existing = std::find_if(functions.begin(), functions.end(),
                                           [target_addr](const Function& f) {
                                               return f.base == target_addr;
                                           });
                
                if (existing == functions.end()) {
                    // Analyze this function
                    size_t offset = target_addr - section.base;
                    size_t max_size = section.size - offset;
                    
                    Function func = AnalyzeSingleFunction(target_addr, section.data + offset, max_size);
                    if (func.IsValid()) {
                        functions.push_back(func);
                    }
                }
            }
        }
    }
    
    return true;
}

Function RPXAnalyzer::AnalyzeSingleFunction(uint32_t address, const uint8_t* data, size_t max_size) {
    return Function::Analyze(data, max_size, address);
}

bool RPXAnalyzer::DetectJumpTables() {
    jump_tables.clear();
    
    // Scan each code section for jump table patterns
    for (const auto& section : image.sections) {
        if (section.flags & SectionFlags_Code) {
            ScanForJumpTablePatterns(section);
        }
    }
    
    return !jump_tables.empty();
}

bool RPXAnalyzer::ScanForJumpTablePatterns(const Section& section) {
    if (!section.data || section.size < 24) { // Need at least 6 instructions
        return false;
    }
    
    const uint32_t* data = reinterpret_cast<const uint32_t*>(section.data);
    const size_t word_count = section.size / 4;
    
    // Pattern 1: Absolute jump table
    // lis r11, table@ha
    // addi r11, r11, table@l  
    // slwi r0, r0, 2
    // lwzx r0, r11, r0
    // mtctr r0
    // bctr
    for (size_t i = 0; i < word_count - 5; i++) {
        ppc_insn insns[6];
        bool valid_pattern = true;
        
        for (int j = 0; j < 6; j++) {
            if (!ppc::Disassemble(&data[i + j], section.base + (i + j) * 4, insns[j])) {
                valid_pattern = false;
                break;
            }
        }
        
        if (valid_pattern &&
            insns[0].opcode->id == PPC_INST_LIS &&
            insns[1].opcode->id == PPC_INST_ADDI &&
            insns[2].opcode->id == PPC_INST_RLWINM &&
            insns[3].opcode->id == PPC_INST_LWZX &&
            insns[4].opcode->id == PPC_INST_MTCTR &&
            insns[5].opcode->id == PPC_INST_BCTR) {
            
            JumpTable table;
            table.base_address = section.base + i * 4;
            table.type = JumpTable::Absolute;
            
            if (AnalyzeJumpTableAt(table.base_address, table)) {
                jump_tables.push_back(table);
            }
        }
    }
    
    return true;
}

bool RPXAnalyzer::AnalyzeJumpTableAt(uint32_t address, JumpTable& table) {
    // TODO:
    // 1. Trace back to find the comparison instruction
    // 2. Extract the register used and number of cases
    // 3. Calculate the table address
    // 4. Read the table entries
    
    // For now, just mark it as detected
    table.register_used = 3; // r3 is commonly used
    table.table_address = 0; // Would be calculated
    table.default_target = 0; // Would be found
    
    return true;
}

bool RPXAnalyzer::ValidateJumpTable(const JumpTable& table) {
    // Validate that all targets are reasonable
    for (uint32_t target : table.targets) {
        if (!IsValidCodeAddress(target)) {
            return false;
        }
    }
    
    return table.targets.size() > 0 && table.targets.size() < 1000; // Reasonable limits
}

bool RPXAnalyzer::FindSystemFunctions() {
    // Look for standard PowerPC function prologue/epilogue patterns
    
    // Pattern for __restgprlr_14: ld r14, -0x98(r1), ld r15, -0x90(r1), etc.
    uint32_t restgpr_pattern[] = {
        0xE9C1FF68, // ld r14, -0x98(r1)
        0xE9E1FF70, // ld r15, -0x90(r1)
    };
    
    uint32_t savegpr_pattern[] = {
        0xF9C1FF68, // std r14, -0x98(r1)
        0xF9E1FF70, // std r15, -0x90(r1)
    };
    
    // These patterns would be different for WiiU's 32-bit PowerPC
    // The actual patterns would need to be found through analysis
    
    return FindRestoreFunction("__restgprlr_14", restgpr_pattern, 2, system_functions.restgprlr_14) &&
           FindSaveFunction("__savegprlr_14", savegpr_pattern, 2, system_functions.savegprlr_14);
}

bool RPXAnalyzer::FindRestoreFunction(const char* name, uint32_t pattern[], size_t pattern_size, uint32_t& address) {
    for (const auto& section : image.sections) {
        if (!(section.flags & SectionFlags_Code) || !section.data) {
            continue;
        }
        
        const uint32_t* data = reinterpret_cast<const uint32_t*>(section.data);
        const size_t word_count = section.size / 4;
        
        for (size_t i = 0; i < word_count - pattern_size; i++) {
            bool match = true;
            for (size_t j = 0; j < pattern_size; j++) {
                if (ByteSwap(data[i + j]) != pattern[j]) {
                    match = false;
                    break;
                }
            }
            
            if (match) {
                address = section.base + i * 4;
                return true;
            }
        }
    }
    
    return false;
}

bool RPXAnalyzer::FindSaveFunction(const char* name, uint32_t pattern[], size_t pattern_size, uint32_t& address) {
    return FindRestoreFunction(name, pattern, pattern_size, address);
}

bool RPXAnalyzer::AnalyzeImportsExports() {
    // TODO
    // WiiU RPX files have specific import/export sections
    // This would involve parsing the .rpl_imports and .rpl_exports sections
    return true;
}

std::string RPXAnalyzer::GenerateReport() const {
    std::string report;
    
    report += "=== RebrewU RPX Analysis Report ===\n\n";
    
    // Basic file info
    report += fmt::format("RPX File Analysis\n");
    report += fmt::format("Base Address: 0x{:X}\n", image.base);
    report += fmt::format("Entry Point: 0x{:X}\n", image.entry_point);
    report += fmt::format("File Size: 0x{:X} bytes\n", image.size);
    report += "\n";
    
    // Sections
    report += fmt::format("Sections ({} total):\n", image.sections.size());
    for (const auto& section : image.sections) {
        report += fmt::format("  {:<15} 0x{:08X} - 0x{:08X} (size: 0x{:X})\n", 
                             section.name, section.base, section.base + section.size, section.size);
    }
    report += "\n";
    
    // Functions
    report += fmt::format("Functions ({} total):\n", functions.size());
    size_t total_code_size = 0;
    for (const auto& func : functions) {
        report += fmt::format("  0x{:08X} - 0x{:08X} (size: 0x{:X})\n", 
                             func.base, func.GetEndAddress(), func.size);
        total_code_size += func.size;
    }
    report += fmt::format("Total code size: 0x{:X} bytes\n\n", total_code_size);
    
    // Jump tables
    if (!jump_tables.empty()) {
        report += fmt::format("Jump Tables ({} total):\n", jump_tables.size());
        for (const auto& table : jump_tables) {
            const char* type_names[] = {"Absolute", "Computed", "ByteOffset", "ShortOffset"};
            report += fmt::format("  0x{:08X} ({}, {} targets)\n", 
                                 table.base_address, type_names[table.type], table.targets.size());
        }
        report += "\n";
    }
    
    // System functions
    report += "System Functions:\n";
    if (system_functions.restgprlr_14) {
        report += fmt::format("  __restgprlr_14: 0x{:08X}\n", system_functions.restgprlr_14);
    }
    if (system_functions.savegprlr_14) {
        report += fmt::format("  __savegprlr_14: 0x{:08X}\n", system_functions.savegprlr_14);
    }
    if (system_functions.longjmp) {
        report += fmt::format("  longjmp: 0x{:08X}\n", system_functions.longjmp);
    }
    if (system_functions.setjmp) {
        report += fmt::format("  setjmp: 0x{:08X}\n", system_functions.setjmp);
    }
    
    report += "\n=== End of Report ===\n";
    
    return report;
}

const uint8_t* RPXAnalyzer::GetCodePointer(uint32_t address) const {
    return image.Find(address);
}

bool RPXAnalyzer::IsValidCodeAddress(uint32_t address) const {
    for (const auto& section : image.sections) {
        if ((section.flags & SectionFlags_Code) && 
            address >= section.base && address < section.base + section.size) {
            return true;
        }
    }
    return false;
}

bool RPXAnalyzer::IsInCodeSection(uint32_t address) const {
    return IsValidCodeAddress(address);
}

std::string RPXAnalyzer::FormatAddress(uint32_t address) const {
    return fmt::format("0x{:08X}", address);
}