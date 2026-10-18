#include "../../runtime/rbrew_diagnostics.h"
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. See LICENSE.txt in this directory.
#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <bitset>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <memory>
#include <span>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <vector>
using uint8 = uint8_t; using uint16 = uint16_t; using uint32 = uint32_t; using uint64 = uint64_t;
using sint8 = int8_t; using sint16 = int16_t; using sint32 = int32_t; using sint64 = int64_t;
using MPTR = uint32; constexpr MPTR MPTR_NULL = 0;
class RendererShader;
constexpr uint32 LATTE_GLSL_DYNAMIC_UNIFORM_BLOCK_SIZE = 4096;
namespace fmt { using std::format_to_n; using std::format; }
template<class T> auto reverse_itr(T& value) { return std::views::reverse(value); }
namespace ActiveSettings {
inline bool ForceSamplerRoundToPrecision() { return false; }
inline bool ShaderPreventInfiniteLoopsEnabled() { return false; }
inline bool DumpShadersEnabled() { return false; }
}
// Assertions become translation failures caught at the native API boundary.
#define cemu_assert(x) do { if (!(x)) throw std::runtime_error("Cemu shader assertion: " #x); } while (0)
#define cemu_assert_debug(x) cemu_assert(x)
#define cemu_assert_suspicious() cemu_assert_unimplemented()
#define cemu_assert_unimplemented() throw std::runtime_error("Unsupported Latte shader instruction")
#define debugBreakpoint() cemu_assert_unimplemented()
#define debug_printf(...) RBREW_LOGF(stderr, __VA_ARGS__)
#define DEFINE_ENUM_FLAG_OPERATORS(T) \
inline T operator|(T a, T b) { return T(uint32(a)|uint32(b)); } \
inline T operator&(T a, T b) { return T(uint32(a)&uint32(b)); }
enum class LogType { Force, APIErrors };
template<class T> auto shader_format_value(const T& v) {
    if constexpr (std::is_enum_v<T>) return std::underlying_type_t<T>(v);
    else return v;
}
template<class... Args> std::string shader_format(std::string_view fmt, const Args&... args) {
    auto values = std::tuple{shader_format_value(args)...};
    return std::apply([&](auto&... v) { return std::vformat(fmt, std::make_format_args(v...)); }, values);
}
template<class... Args> void cemuLog_log(LogType, std::string_view fmt, const Args&... args) {
    RBREW_LOGF(stderr, "[shader] %s\n", shader_format(fmt, args...).c_str());
}
#define cemuLog_logDebug cemuLog_log
struct ShaderHostState { uint32 vendor{}; };
extern thread_local ShaderHostState LatteGPUState;
template<class T> constexpr T _swapEndianU32(T v) {
    uint32 x = uint32(v); return T((x<<24)|((x<<8)&0xFF0000)|((x>>8)&0xFF00)|(x>>24));
}
