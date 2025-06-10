#include "pch.h"
#include "recompiler.h"

bool Recompiler::LoadConfig(const std::string_view& configFilePath) {
    try {
        config.Load(configFilePath);
        
        if (!config.Validate()) {
            fmt::println("ERROR: Configuration validation failed");
            return false;
        }
        
        std::string rpx_path = config.directoryPath + config.filePath;
        const auto file = LoadFile(rpx_path);
        if (file.empty()) {
            fmt::println("ERROR: Could not load RPX file: {}", rpx_path);
            return false;
        }
        
        image = RPXImage::ParseImage(file.data(), file.size());
        if (image.data.empty()) {
            fmt::println("ERROR: Could not parse RPX file");
            return false;
        }
        
        fmt::println("Successfully loaded RPX file: {} (size: 0x{:X})", rpx_path, image.size);
        return true;
        
    } catch (const std::exception& e) {
        fmt::println("ERROR: Failed to load configuration: {}", e.what());
        return false;
    }
}

void Recompiler::Analyse() {
    functions.clear();
    
    // Generate system function entries first
    for (size_t i = 14; i < 32; i++) {
        if (config.restGpr14Address != 0) {
            auto& restgpr = functions.emplace_back();
            restgpr.base = config.restGpr14Address + (i - 14) * 4;
            restgpr.size = (32 - i) * 4 + 12;
            image.symbols.emplace(fmt::format("__restgprlr_{}", i), restgpr.base, restgpr.size, Symbol_Function);
        }
        
        if (config.saveGpr14Address != 0) {
            auto& savegpr = functions.emplace_back();
            savegpr.base = config.saveGpr14Address + (i - 14) * 4;
            savegpr.size = (32 - i) * 4 + 8;
            image.symbols.emplace(fmt::format("__savegprlr_{}", i), savegpr.base, savegpr.size, Symbol_Function);
        }
        
        if (config.restFpr14Address != 0) {
            auto& restfpr = functions.emplace_back();
            restfpr.base = config.restFpr14Address + (i - 14) * 4;
            restfpr.size = (32 - i) * 4 + 4;
            image.symbols.emplace(fmt::format("__restfpr_{}", i), restfpr.base, restfpr.size, Symbol_Function);
        }
        
        if (config.saveFpr14Address != 0) {
            auto& savefpr = functions.emplace_back();
            savefpr.base = config.saveFpr14Address + (i - 14) * 4;
            savefpr.size = (32 - i) * 4 + 4;
            image.symbols.emplace(fmt::format("__savefpr_{}", i), savefpr.base, savefpr.size, Symbol_Function);
        }
    }
    
    // Add manually defined functions
    for (auto& [address, size] : config.functions) {
        functions.emplace_back(address, size);
        image.symbols.emplace(fmt::format("sub_{:X}", address), address, size, Symbol_Function);
    }
    
    // Analyze functions from symbol table and RPX structure
    for (const auto& symbol : image.symbols) {
        if (symbol.type == Symbol_Function && symbol.size > 0) {
            auto existing = std::find_if(functions.begin(), functions.end(),
                                       [&symbol](const Function& f) {
                                           return f.base == symbol.address;
                                       });
            
            if (existing == functions.end()) {
                functions.emplace_back(symbol.address, symbol.size);
            }
        }
    }
    
    // Scan code sections for additional functions
    for (const auto& section : image.sections) {
        if (!(section.flags & SectionFlags_Code) || !section.data) {
            continue;
        }
        
        size_t base = section.base;
        uint8_t* data = section.data;
        uint8_t* dataEnd = section.data + section.size;
        
        while (data < dataEnd) {
            // Skip invalid instruction patterns
            auto invalidInstr = config.invalidInstructions.find(ByteSwap(*(uint32_t*)data));
            if (invalidInstr != config.invalidInstructions.end()) {
                base += invalidInstr->second;
                data += invalidInstr->second;
                continue;
            }
            
            // Check if we already have a function at this address
            auto existing_symbol = image.symbols.find(base);
            if (existing_symbol != image.symbols.end() && 
                existing_symbol->address == base && 
                existing_symbol->type == Symbol_Function) {
                
                base += existing_symbol->size;
                data += existing_symbol->size;
            } else {
                // Analyze new function
                auto fn = Function::Analyze(data, dataEnd - data, base);
                if (fn.IsValid() && fn.size >= 4) {
                    image.symbols.emplace(fmt::format("sub_{:X}", fn.base), fn.base, fn.size, Symbol_Function);
                    functions.push_back(fn);
                    
                    base += fn.size;
                    data += fn.size;
                } else {
                    // Skip this instruction
                    base += 4;
                    data += 4;
                }
            }
        }
    }
    
    // Sort functions by address
    std::sort(functions.begin(), functions.end(), 
              [](const Function& lhs, const Function& rhs) { 
                  return lhs.base < rhs.base; 
              });
    
    fmt::println("Analysis complete. Found {} functions.", functions.size());
}

void Recompiler::Recompile(const std::filesystem::path& headerFilePath) {
    out.reserve(10 * 1024 * 1024); // Pre-allocate 10MB
    
    fmt::println("Starting recompilation of {} functions...", functions.size());
    
    // Generate configuration header
    GenerateConfigFiles(headerFilePath);
    
    // Generate function headers
    GenerateHeaderFiles();
    
    // Generate function mappings
    GenerateFunctionMappings();
    
    // Recompile all functions
    size_t success_count = 0;
    for (size_t i = 0; i < functions.size(); i++) {
        if ((i % 256) == 0) {
            SaveCurrentOutData();
            println("#include \"ppc_recomp_shared.h\"");
            println("#include <cmath>");
            println("#include <immintrin.h>");
            println("");
        }
        
        if ((i % 100) == 0 || (i == functions.size() - 1)) {
            fmt::println("Recompiling functions... {:.1f}%", 
                        static_cast<float>(i + 1) / functions.size() * 100.0f);
        }
        
        if (RecompileFunction(functions[i])) {
            success_count++;
        } else {
            fmt::println("WARNING: Failed to recompile function at 0x{:X}", functions[i].base);
        }
    }
    
    SaveCurrentOutData();
    
    fmt::println("Recompilation complete! Successfully recompiled {}/{} functions.", 
                success_count, functions.size());
}

bool Recompiler::RecompileFunction(const Function& fn) {
    auto base = fn.base;
    auto end = base + fn.size;
    auto* data = (uint32_t*)image.Find(base);
    
    if (!data) {
        fmt::println("ERROR: Could not find data for function at 0x{:X}", base);
        return false;
    }
    
    // Collect labels for jumps within the function
    std::unordered_set<size_t> labels;
    
    for (size_t addr = base; addr < end; addr += 4) {
        const uint32_t instruction = ByteSwap(*(uint32_t*)((char*)data + addr - base));
        
        if (!PPC_BL(instruction)) {
            const size_t op = PPC_OP(instruction);
            if (op == PPC_OP_B) {
                labels.emplace(addr + PPC_BI(instruction));
            } else if (op == PPC_OP_BC) {
                labels.emplace(addr + PPC_BD(instruction));
            }
        }
        
        // Check for switch table at this address
        auto switchTable = config.switchTables.find(addr);
        if (switchTable != config.switchTables.end()) {
            for (auto label : switchTable->second.labels) {
                labels.emplace(label);
            }
        }
        
        // Check for mid-asm hooks
        auto midAsmHook = config.midAsmHooks.find(addr);
        if (midAsmHook != config.midAsmHooks.end()) {
            if (midAsmHook->second.jumpAddress != 0) {
                labels.emplace(midAsmHook->second.jumpAddress);
            }
            if (midAsmHook->second.jumpAddressOnTrue != 0) {
                labels.emplace(midAsmHook->second.jumpAddressOnTrue);
            }
            if (midAsmHook->second.jumpAddressOnFalse != 0) {
                labels.emplace(midAsmHook->second.jumpAddressOnFalse);
            }
        }
    }
    
    // Get function symbol
    auto symbol = image.symbols.find(fn.base);
    std::string name = symbol != image.symbols.end() ? symbol->name : fmt::format("sub_{:X}", fn.base);
    
#ifdef REBREW_RECOMP_USE_ALIAS
    println("__attribute__((alias(\"__imp__{}\"))) PPC_WEAK_FUNC({});", name, name);
#endif
    
    println("PPC_FUNC_IMPL(__imp__{}) {{", name);
    println("\tPPC_FUNC_PROLOGUE();");
    
    // Generate local variables
    RecompilerLocalVariables localVariables;
    std::string tempOutput;
    std::swap(out, tempOutput);
    
    auto switchTable = config.switchTables.end();
    FPState fpState = FPState::Unknown;
    bool allRecompiled = true;
    
    // Recompile each instruction
    ppc_insn insn;
    while (base < end) {
        // Generate labels
        if (labels.find(base) != labels.end()) {
            println("loc_{:X}:", base);
            fpState = FPState::Unknown; // Reset FP state at labels
        }
        
        // Find switch table for this address
        if (switchTable == config.switchTables.end()) {
            switchTable = config.switchTables.find(base);
        }
        
        ppc::Disassemble(data, base, insn);
        
        if (insn.opcode == nullptr) {
            println("\t// INVALID INSTRUCTION: 0x{:X}", ByteSwap(*data));
            if (!config.treatUnknownInstructionsAsNop) {
                allRecompiled = false;
            }
        } else {
            if (!RecompileInstruction(fn, base, insn, data, switchTable, localVariables, fpState)) {
                fmt::println("ERROR: Unimplemented instruction at 0x{:X}: {}", base, insn.opcode->name);
                allRecompiled = false;
            }
        }
        
        base += 4;
        ++data;
    }
    
    println("}}");
    println("");
    
#ifndef REBREW_RECOMP_USE_ALIAS
    println("PPC_WEAK_FUNC({}) {{", name);
    println("\t__imp__{}(ctx, base);", name);
    println("}}");
    println("");
#endif
    
    // Add local variable declarations at the beginning
    std::swap(out, tempOutput);
    
    // Generate local variable declarations
    if (localVariables.ctr) println("\tPPCRegister ctr{{}};");
    if (localVariables.xer) println("\tPPCXERRegister xer{{}};");
    if (localVariables.reserved) println("\tPPCRegister reserved{{}};");
    
    for (size_t i = 0; i < 8; i++) {
        if (localVariables.cr[i]) println("\tPPCCRRegister cr{}{{}};", i);
        if (localVariables.gqr[i]) println("\tPPCRegister gqr{}{{}};", i);
    }
    
    for (size_t i = 0; i < 32; i++) {
        if (localVariables.r[i]) println("\tPPCRegister r{}{{}};", i);
        if (localVariables.f[i]) println("\tPPCFPRegister f{}{{}};", i);
    }
    
    if (localVariables.env) println("\tPPCContext env{{}};");
    if (localVariables.temp) println("\tPPCRegister temp{{}};");
    if (localVariables.vTemp) println("\tPPCRegister vTemp{{}};");
    if (localVariables.ea) println("\tuint32_t ea{{}};");
    if (localVariables.ps_temp) println("\tPPCFPRegister ps_temp{{}};");
    
    out += tempOutput;
    
    return allRecompiled;
}

// Helper function implementations
bool Recompiler::IsInMEM1(uint32_t address) const {
    return address >= config.mem1_base && address < config.mem1_base + config.mem1_size;
}

bool Recompiler::IsInMEM2(uint32_t address) const {
    return address >= config.mem2_base && address < config.mem2_base + config.mem2_size;
}

bool Recompiler::IsValidWiiUAddress(uint32_t address) const {
    return IsInMEM1(address) || IsInMEM2(address);
}

std::string Recompiler::GetMemoryRegionName(uint32_t address) const {
    if (IsInMEM1(address)) return "MEM1";
    if (IsInMEM2(address)) return "MEM2";
    return "UNKNOWN";
}

void Recompiler::GenerateConfigFiles(const std::filesystem::path& headerFilePath) {
    // Generate ppc_config.h
    println("#pragma once");
    println("#ifndef PPC_CONFIG_H_INCLUDED");
    println("#define PPC_CONFIG_H_INCLUDED");
    println("");
    
    // Configuration defines
    if (config.skipLr) println("#define PPC_CONFIG_SKIP_LR");
    if (config.ctrAsLocalVariable) println("#define PPC_CONFIG_CTR_AS_LOCAL");
    if (config.xerAsLocalVariable) println("#define PPC_CONFIG_XER_AS_LOCAL");
    if (config.reservedRegisterAsLocalVariable) println("#define PPC_CONFIG_RESERVED_AS_LOCAL");
    if (config.skipMsr) println("#define PPC_CONFIG_SKIP_MSR");
    if (config.crRegistersAsLocalVariables) println("#define PPC_CONFIG_CR_AS_LOCAL");
    if (config.nonArgumentRegistersAsLocalVariables) println("#define PPC_CONFIG_NON_ARGUMENT_AS_LOCAL");
    if (config.nonVolatileRegistersAsLocalVariables) println("#define PPC_CONFIG_NON_VOLATILE_AS_LOCAL");
    
    println("");
    
    // Memory layout
    println("#define PPC_IMAGE_BASE 0x{:X}ull", image.base);
    println("#define PPC_IMAGE_SIZE 0x{:X}ull", image.size);
    println("#define PPC_MEM1_BASE 0x{:X}ull", config.mem1_base);
    println("#define PPC_MEM1_SIZE 0x{:X}ull", config.mem1_size);
    println("#define PPC_MEM2_BASE 0x{:X}ull", config.mem2_base);
    println("#define PPC_MEM2_SIZE 0x{:X}ull", config.mem2_size);
    
    println("");
    println("#ifdef PPC_INCLUDE_DETAIL");
    println("#include \"ppc_detail.h\"");
    println("#endif");
    println("");
    println("#endif");
    
    SaveCurrentOutData("ppc_config.h");
}

void Recompiler::GenerateHeaderFiles() {
    // Generate ppc_context.h copy
    std::ifstream contextFile("RebrewUtils/ppc_context.h");
    if (contextFile.is_open()) {
        println("#pragma once");
        println("#include \"ppc_config.h\"");
        println("");
        
        std::string line;
        while (std::getline(contextFile, line)) {
            println("{}", line);
        }
        contextFile.close();
    }
    
    SaveCurrentOutData("ppc_context.h");
    
    // Generate shared header
    println("#pragma once");
    println("#include \"ppc_config.h\"");
    println("#include \"ppc_context.h\"");
    println("");
    
    for (auto& symbol : image.symbols) {
        if (symbol.type == Symbol_Function) {
            println("PPC_EXTERN_FUNC({});", symbol.name);
        }
    }
    
    SaveCurrentOutData("ppc_recomp_shared.h");
}

void Recompiler::GenerateFunctionMappings() {
    println("#include \"ppc_recomp_shared.h\"");
    println("");
    println("PPCFuncMapping PPCFuncMappings[] = {{");
    
    for (auto& symbol : image.symbols) {
        if (symbol.type == Symbol_Function) {
            println("\t{{ 0x{:X}, {} }},", symbol.address, symbol.name);
        }
    }
    
    println("\t{{ 0, nullptr }}");
    println("}};");
    
    SaveCurrentOutData("ppc_func_mapping.cpp");
}

void Recompiler::SaveCurrentOutData(const std::string_view& name) {
    if (!out.empty()) {
        std::string fileName;
        
        if (name.empty()) {
            fileName = fmt::format("ppc_recomp.{}.cpp", cppFileIndex);
            ++cppFileIndex;
        } else {
            fileName = std::string(name);
        }
        
        std::string fullPath = fmt::format("{}/{}/{}", config.directoryPath, config.outDirectoryPath, fileName);

        bool shouldWrite = true;
        if (std::filesystem::exists(fullPath)) {
            auto existingContent = LoadFile(fullPath);
            if (existingContent.size() == out.size()) {
                if (std::memcmp(existingContent.data(), out.data(), out.size()) == 0) {
                    shouldWrite = false;
                }
            }
        }
        
        if (shouldWrite) {
            if (!SaveFile(fullPath, out.data(), out.size())) {
                fmt::println("ERROR: Could not write file: {}", fullPath);
            }
        }
        
        out.clear();
    }
}

// TODO: Add all WiiU Power PC Instructions
bool Recompiler::RecompileInstruction(
    const Function& fn,
    uint32_t base,
    const ppc_insn& insn,
    const uint32_t* data,
    std::unordered_map<uint32_t, RecompilerSwitchTable>::iterator& switchTable,
    RecompilerLocalVariables& localVariables,
    FPState& fpState) {
    
    println("\t// {} {}", insn.opcode->name, insn.op_str);
    
    auto midAsmHook = config.midAsmHooks.find(base);
    if (midAsmHook != config.midAsmHooks.end() && !midAsmHook->second.afterInstruction) {
        PrintMidAsmHook(midAsmHook->second, localVariables);
    }
    
    // TODO: Add all PowerPC and WiiU PowerPC instructions here:
    
    switch (insn.opcode->id) {
        case PPC_INST_ADD:
            println("\t{}.u64 = {}.u64 + {}.u64;", 
                   GetRegisterName(insn.operands[0], 'r', localVariables),
                   GetRegisterName(insn.operands[1], 'r', localVariables),
                   GetRegisterName(insn.operands[2], 'r', localVariables));
            break;
            
        case PPC_INST_ADDI:
            println("\t{}.s64 = {}.s64 + {};", 
                   GetRegisterName(insn.operands[0], 'r', localVariables),
                   GetRegisterName(insn.operands[1], 'r', localVariables),
                   static_cast<int32_t>(insn.operands[2]));
            break;
            
        case PPC_INST_B:
            if (insn.operands[0] < fn.base || insn.operands[0] >= fn.base + fn.size) {
                PrintFunctionCall(insn.operands[0]);
                println("\treturn;");
            } else {
                println("\tgoto loc_{:X};", insn.operands[0]);
            }
            break;
            
        case PPC_INST_BLR:
            println("\treturn;");
            break;
            
        // WiiU-specific paired single instructions
        case PPC_INST_PS_ADD:
            if (config.generatePairedSingleSupport) {
                return RecompilePairedSingleInstruction(insn, localVariables);
            }
            break;
            
        default:
            println("\t__builtin_debugtrap(); // Unimplemented: {}", insn.opcode->name);
            return false;
    }
    
    // Handle mid-asm hooks after instruction
    if (midAsmHook != config.midAsmHooks.end() && midAsmHook->second.afterInstruction) {
        PrintMidAsmHook(midAsmHook->second, localVariables);
    }
    
    return true;
}

// Placeholder implementations for helper methods
std::string Recompiler::GetRegisterName(size_t index, char type, RecompilerLocalVariables& locals) {
    if (type == 'r' && index < 32) {
        if ((config.nonArgumentRegistersAsLocalVariables && (index == 0 || index == 2 || index == 11 || index == 12)) ||
            (config.nonVolatileRegistersAsLocalVariables && index >= 14)) {
            locals.r[index] = true;
            return fmt::format("r{}", index);
        }
        return fmt::format("ctx.r{}", index);
    }
    return fmt::format("ctx.{}{}", type, index);
}

bool Recompiler::RecompilePairedSingleInstruction(const ppc_insn& insn, RecompilerLocalVariables& locals) {
    // Placeholder for paired single instruction implementation
    println("\t// Paired single instruction: {}", insn.opcode->name);
    return true;
}

void Recompiler::PrintFunctionCall(uint32_t address) {
    auto targetSymbol = image.symbols.find(address);
    if (targetSymbol != image.symbols.end() && targetSymbol->type == Symbol_Function) {
        println("\t{}(ctx, base);", targetSymbol->name);
    } else {
        println("\t// CALL to unknown function 0x{:X}", address);
    }
}

void Recompiler::PrintMidAsmHook(const RecompilerMidAsmHook& hook, RecompilerLocalVariables& locals) {
    println("\t{}(); // Mid-asm hook", hook.name);
}

// we need more stuff here but i cba writing that rn