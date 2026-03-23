#pragma once
#include <cstdint>
#include <string_view>

namespace rebrewu {
  constexpr uint32_t VERSION_MAJOR = 0;
  constexpr uint32_t VERSION_MINOR = 1;
  constexpr uint32_t VERSION_PATCH = 0;
  constexpr std::string_view VERSION_STRING = "0.1.0";
  constexpr std::string_view PROJECT_NAME = "RebrewU";
  constexpr std::string_view PROJECT_DESCRIPTION =
    "Wii U static recompilation framework";
}
