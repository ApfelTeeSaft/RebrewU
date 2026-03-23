#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <cstdint>
#include <filesystem>

namespace rebrewu::config {
  struct SymbolOverride {
    std::string name;
    uint32_t address;
    std::optional<uint32_t> size;
    bool force_function{false};
    bool ignore{false};
  };

  struct FunctionBoundaryOverride {
    uint32_t start;
    uint32_t end;  // 0 = auto
    std::string name;
  };

  struct JumpTableHint {
    uint32_t dispatch_addr;
    uint32_t table_addr;
    uint32_t num_entries;
  };

  struct IgnoredRegion {
    uint32_t start;
    uint32_t end;
    std::string reason;
  };

  struct RelocOverride {
    uint32_t address;
    uint8_t type;
    std::string symbol;
    int32_t addend;
  };

  struct RebrewConfig {
    // General
    std::string output_dir{"output"};
    bool verbose{false};
    bool emit_comments{true};

    // Symbol overrides
    std::vector<SymbolOverride> symbols;

    // Function boundaries
    std::vector<FunctionBoundaryOverride> functions;

    // Jump table hints
    std::vector<JumpTableHint> jump_tables;

    // Ignored regions (not decompiled)
    std::vector<IgnoredRegion> ignored_regions;

    // Relocation overrides
    std::vector<RelocOverride> reloc_overrides;

    // Naming
    std::string function_prefix{"func_"};
    std::string data_prefix{"data_"};

    // Build
    std::vector<std::string> rpl_paths;
  };

  class ConfigLoader {
  public:
    // Load from JSON file
    static std::optional<RebrewConfig> load_json(const std::filesystem::path& path, std::string& error);
    // Load from TOML file
    static std::optional<RebrewConfig> load_toml(const std::filesystem::path& path, std::string& error);
    // Auto-detect format from extension
    static std::optional<RebrewConfig> load(const std::filesystem::path& path, std::string& error);
    // Write default config
    static bool write_default(const std::filesystem::path& path, std::string& error);
  };
}
