#pragma once
#include <string>
#include <vector>
#include <optional>
#include <filesystem>

namespace rebrewu::cli {
  enum class Command {
    Recompile,     // main recompilation
    Inspect,       // inspect binary structure
    DumpRelocs,    // dump relocation tables
    ListSymbols,   // list symbols
    ListSections,  // list sections
    Disassemble,   // disassemble a range
    ListExports,   // list RPL exports
    ListImports,   // list RPL imports
    Analyze,       // run analysis only (no codegen)
    Help,
    Version
  };

  struct CliOptions {
    Command command{Command::Recompile};
    std::filesystem::path rpx_path;
    std::vector<std::filesystem::path> rpl_paths;
    std::filesystem::path output_dir{"output"};
    std::optional<std::filesystem::path> config_path;

    // Inspect options
    bool show_headers{false};
    bool show_sections{false};
    bool show_segments{false};

    // Disassemble options
    std::optional<uint32_t> disasm_start;
    std::optional<uint32_t> disasm_end;
    std::optional<std::string> disasm_function;

    // Symbol options
    bool show_imports{true};
    bool show_exports{true};
    bool show_synthetic{false};

    // General
    bool verbose{false};
    bool color{true};
    bool no_codegen{false};
  };

  std::optional<CliOptions> parse(int argc, char** argv);

  void print_usage(const char* prog);
  void print_version();
}
