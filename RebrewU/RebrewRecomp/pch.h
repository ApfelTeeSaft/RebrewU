#pragma once

// Standard library includes
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <memory>
#include <utility>

#ifdef _WIN32
#include <Windows.h>
#include <immintrin.h>
#else
#include <x86intrin.h>
#endif

#include <fmt/core.h>
#include <fmt/format.h>
#include <toml++/toml.hpp>

#include "file.h"
#include "disasm.h"
#include "rpx_image.h"
#include "wiiu_ppc.h"
#include "ppc_context.h"
#include "function.h"

// xxHash for file hashing
#ifdef XXHASH_INCLUDE_DIR
#include <xxhash.h>
#else
extern "C" {
    typedef struct { uint64_t v[4]; } XXH3_state_t;
    typedef struct { uint64_t low64, high64; } XXH128_hash_t;
    
    XXH128_hash_t XXH3_128bits(const void* data, size_t len);
    int XXH128_isEqual(XXH128_hash_t h1, XXH128_hash_t h2);
}
#endif

#if defined(_MSC_VER)
#define FORCE_INLINE __forceinline
#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#elif defined(__GNUC__) || defined(__clang__)
#define FORCE_INLINE __attribute__((always_inline)) inline
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define FORCE_INLINE inline
#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#endif

#ifdef _DEBUG
#define DEBUG_PRINT(...) fmt::print(__VA_ARGS__)
#define DEBUG_ASSERT(x) assert(x)
#else
#define DEBUG_PRINT(...)
#define DEBUG_ASSERT(x)
#endif

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s8 = int8_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;
using f32 = float;
using f64 = double;

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#define CONCAT(a, b) a##b
#define CONCAT3(a, b, c) a##b##c

#define ALIGN_UP(x, align) (((x) + (align) - 1) & ~((align) - 1))
#define ALIGN_DOWN(x, align) ((x) & ~((align) - 1))
#define IS_ALIGNED(x, align) (((x) & ((align) - 1)) == 0)

#define BIT(n) (1U << (n))
#define BITS(start, count) (((1U << (count)) - 1) << (start))
#define GET_BITS(value, start, count) (((value) >> (start)) & ((1U << (count)) - 1))
#define SET_BITS(value, start, count, new_bits) \
    ((value) = ((value) & ~BITS(start, count)) | (((new_bits) & ((1U << (count)) - 1)) << (start)))

#define CHECK(condition) \
    do { \
        if (UNLIKELY(!(condition))) { \
            fmt::print(stderr, "CHECK failed at {}:{}: {}\n", __FILE__, __LINE__, #condition); \
            std::abort(); \
        } \
    } while (0)

#define CHECK_MSG(condition, msg, ...) \
    do { \
        if (UNLIKELY(!(condition))) { \
            fmt::print(stderr, "CHECK failed at {}:{}: " msg "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
            std::abort(); \
        } \
    } while (0)

template<typename Container, typename Predicate>
auto find_if(Container&& container, Predicate&& pred) {
    return std::find_if(std::begin(container), std::end(container), std::forward<Predicate>(pred));
}

template<typename Container, typename Value>
auto find(Container&& container, const Value& value) {
    return std::find(std::begin(container), std::end(container), value);
}

template<typename Container, typename Predicate>
bool any_of(Container&& container, Predicate&& pred) {
    return std::any_of(std::begin(container), std::end(container), std::forward<Predicate>(pred));
}

template<typename Container, typename Predicate>
bool all_of(Container&& container, Predicate&& pred) {
    return std::all_of(std::begin(container), std::end(container), std::forward<Predicate>(pred));
}

template<typename... Args>
std::string format_string(fmt::format_string<Args...> fmt, Args&&... args) {
    return fmt::format(fmt, std::forward<Args>(args)...);
}

inline std::string to_hex_string(uint32_t value, bool prefix = true) {
    return prefix ? fmt::format("0x{:X}", value) : fmt::format("{:X}", value);
}

inline std::string to_hex_string(uint64_t value, bool prefix = true) {
    return prefix ? fmt::format("0x{:X}", value) : fmt::format("{:X}", value);
}