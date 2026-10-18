#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

struct RbrewNativeRegions {
    uint32_t console_region;
    uint32_t title_regions;
};

// Cafe MCP uses masks, not the application's region ordinals. A multi-region
// title cannot identify a console region without explicit host configuration.
uint32_t rbrew_parse_title_regions(std::string_view metadata);
RbrewNativeRegions rbrew_select_native_regions(uint32_t title_regions,
    std::optional<uint32_t> console_region = std::nullopt);
uint32_t rbrew_parse_console_region(std::string_view numeric_region);
uint32_t rbrew_parse_native_uc_scalar(std::string_view value,uint32_t maximum);
RbrewNativeRegions rbrew_read_native_regions(const std::filesystem::path& metadata,
    std::optional<uint32_t> console_region = std::nullopt);
