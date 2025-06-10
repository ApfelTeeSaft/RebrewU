#pragma once

#include "pch.h"
#include "recompiler_config.h"

// Local variable tracking for optimization
struct RecompilerLocalVariables {
    bool ctr{};
    bool xer{};
    bool reserved{};
    bool cr[8]{};
    bool r[32]{};
    bool f[32]{};
    bool gqr[8]{}; // WiiU-specific Graphics Quantization Registers
    bool env{};
    bool temp{};
    bool vTemp{};
    bool ea{};
    bool ps_temp{}; // Paired single temporary
};

// Floating point state for proper denormal handling
enum class FPState {
    Unknown,
    FPU,      // Normal FPU operations
    PairedSingle // Paired single operations (WiiU specific)
};

// Main WiiU recompiler class
class Recompiler {
public:
    // Core data
    RPXImage image;
    std::vector<Function> functions;
    std::string out;
    size_t cppFileIndex = 0;
    RecompilerConfig config;

    bool LoadConfig(const std::string_view& configFilePath);

    void Analyse();

    void Recompile(const std::filesystem::path& headerFilePath);

    template<class... Args>
    void print(fmt::format_string<Args...> fmt, Args&&... args) {
        fmt::vformat_to(std::back_inserter(out), fmt.get(), fmt::make_format_args(args...));
    }

    template<class... Args>
    void println(fmt::format_string<Args...> fmt, Args&&... args) {
        fmt::vformat_to(std::back_inserter(out), fmt.get(), fmt::make_format_args(args...));
        out += '\n';
    }

private:
    bool RecompileFunction(const Function& fn);

    bool RecompileInstruction(
        const Function& fn,
        uint32_t base,
        const ppc_insn& insn,
        const uint32_t* data,
        std::unordered_map<uint32_t, RecompilerSwitchTable>::iterator& switchTable,
        RecompilerLocalVariables& localVariables,
        FPState& fpState);

    // WiiU-specific instruction handlers
    bool RecompilePairedSingleInstruction(const ppc_insn& insn, RecompilerLocalVariables& locals);
    bool RecompileGQRInstruction(const ppc_insn& insn, RecompilerLocalVariables& locals);
    bool RecompileQuantizedLoadStore(const ppc_insn& insn, RecompilerLocalVariables& locals);

    // Register name generators with optimization support
    std::string GetRegisterName(size_t index, char type, RecompilerLocalVariables& locals);
    std::string GetGQRName(size_t index, RecompilerLocalVariables& locals);
    std::string GetCRName(size_t index, RecompilerLocalVariables& locals);
    std::string GetTempName(const std::string& baseName, RecompilerLocalVariables& locals);

    // Floating point state management
    void SetFloatingPointState(FPState newState, FPState& currentState);
    void PrintSetFlushMode(bool enable, FPState& currentState);

    // Control flow helpers
    void PrintConditionalBranch(const Function& fn, const ppc_insn& insn, bool negate, const std::string_view& condition);
    void PrintFunctionCall(uint32_t address);
    void PrintMidAsmHook(const RecompilerMidAsmHook& hook, RecompilerLocalVariables& locals);

    // Memory access helpers
    bool IsMemoryMappedIO(const uint32_t* data) const;
    std::string GetMemoryAccessMacro(bool isMMIO, const std::string& operation) const;

    // Utility functions
    void SaveCurrentOutData(const std::string_view& name = std::string_view());
    void GenerateConfigFiles(const std::filesystem::path& headerFilePath);
    void GenerateFunctionMappings();
    void GenerateHeaderFiles();

    // Constants for WiiU
    static constexpr uint32_t c_eieio = 0xAC06007C; // Enforce In-order Execution of I/O
    static constexpr uint32_t c_isync = 0x4C00012C; // Instruction synchronize
    static constexpr uint32_t c_sync = 0x7C0004AC;  // Synchronize

    // WiiU memory layout helpers
    bool IsInMEM1(uint32_t address) const;
    bool IsInMEM2(uint32_t address) const;
    bool IsValidWiiUAddress(uint32_t address) const;
    std::string GetMemoryRegionName(uint32_t address) const;
};