#include "../../runtime/rbrew_diagnostics.h"
// gx2_perf.h - opt-in frame timing. RBREW_PERF=<file> appends, every 120
// swaps, the frame rate and the wall time per frame spent in each renderer
// stage (scopes nest: "draw" includes shader/vertex/uniform/texture), byte
// counters, and the costliest native GX2 commands (address = RVA in the
// linker map; "dl" = replayed from a display list, "call" = the caller's
// whole native call including any wait for the render thread).
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

enum Gx2PerfSlot {
    kPerfDraw, kPerfShader, kPerfVertex, kPerfUniform, kPerfTexture,
    kPerfDisplayList, kPerfPresent, kPerfVsync, kPerfTexUpload, kPerfTexReadback, kPerfTexScan, kPerfTexNew, kPerfTexInvalidated,
    kPerfTexHash, kPerfShaderKey, kPerfIndex, kPerfInvalidate, kPerfSubmitWait, kPerfRenderBusy, kPerfFrameWait, kPerfGlQuery,
    kPerfAudioRender, kPerfAudioMix, kPerfAudioCallbacks, kPerfAudioVoiceWait,
    kPerfSlots
};
inline const char* const kGx2PerfNames[kPerfSlots] = {
    "draw", "shader", "vertex", "uniform", "texture", "dlist", "present", "vsync",
    "tex_upload", "tex_readback", "tex_scan", "tex_new", "tex_invalidated",
    "tex_hash", "shader_key", "index", "invalidate", "submit_wait", "render_busy", "frame_wait", "gl_query",
    "audio_render", "audio_mix", "audio_callbacks", "audio_voice_wait"};
enum Gx2PerfBytes { kBytesTexHash, kBytesShaderKey, kBytesVertex, kBytesIndex, kBytesUniformBlock, kBytesSlots };
inline const char* const kGx2PerfByteNames[kBytesSlots] = {"tex_hash", "shader_key", "vertex", "index", "ubo"};
inline std::atomic<uint64_t> g_gx2_perf_ns[kPerfSlots];
inline std::atomic<uint64_t> g_gx2_perf_calls[kPerfSlots];
inline std::atomic<uint64_t> g_gx2_perf_bytes[kBytesSlots];

#ifdef RBREW_DISABLE_LOGGING
// The shipped game has no profiler: every scope and counter folds away.
inline FILE* gx2_perf_file() { return nullptr; }
constexpr bool gx2_perf_enabled() { return false; }
#else
inline FILE* gx2_perf_file() {
    static FILE* file = [] {
        const char* path = rbrew_diagnostic_env("RBREW_PERF");
        return path && *path ? std::fopen(path, "a") : nullptr;
    }();
    return file;
}
inline bool gx2_perf_enabled() { static const bool on = gx2_perf_file() != nullptr; return on; }
#endif
inline void gx2_perf_bytes(Gx2PerfBytes slot, uint64_t bytes) {
    if (gx2_perf_enabled()) g_gx2_perf_bytes[slot].fetch_add(bytes, std::memory_order_relaxed);
}
inline void gx2_perf_count(Gx2PerfSlot slot) {
    if (gx2_perf_enabled()) g_gx2_perf_calls[slot].fetch_add(1, std::memory_order_relaxed);
}

// Per native function: a small lock-free table keyed by function address.
struct Gx2PerfFunction { std::atomic<uintptr_t> key{0}; std::atomic<uint64_t> ns{0}, calls{0}; };
inline Gx2PerfFunction g_gx2_perf_functions[2][256];  // [0] replayed, [1] caller-side
inline void gx2_perf_function(unsigned table, const void* fn, uint64_t ns) {
    const auto key = fn ? reinterpret_cast<uintptr_t>(fn) : uintptr_t(1);  // 1: uniform snapshot
    for (unsigned probe = 0, i = unsigned(key >> 4) % 256; probe < 256; ++probe, i = (i + 1) % 256) {
        auto& e = g_gx2_perf_functions[table][i];
        uintptr_t seen = e.key.load(std::memory_order_acquire);
        if (!seen && e.key.compare_exchange_strong(seen, key)) seen = key;
        if (seen != key) continue;
        e.ns.fetch_add(ns, std::memory_order_relaxed);
        e.calls.fetch_add(1, std::memory_order_relaxed);
        return;
    }
}
#if defined(_MSC_VER)
extern "C" char __ImageBase;
inline uintptr_t gx2_perf_module_base() { return reinterpret_cast<uintptr_t>(&__ImageBase); }
#else
inline uintptr_t gx2_perf_module_base() { return 0; }
#endif
// Optional extra report lines from the renderer (see gx2_render.cpp).
inline void (*g_gx2_perf_detail)(FILE* f, unsigned frames) = nullptr;

inline uint64_t gx2_perf_now() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
// Time the calling thread spent waiting for the render thread.
inline thread_local uint64_t t_gx2_perf_wait_ns = 0;

struct Gx2PerfScope {
    Gx2PerfSlot slot;
    bool on;
    std::chrono::steady_clock::time_point start;
    explicit Gx2PerfScope(Gx2PerfSlot s) : slot(s), on(gx2_perf_enabled()) {
        if (on) start = std::chrono::steady_clock::now();
    }
    ~Gx2PerfScope() { stop(); }
    void stop() {
        if (!on) return;
        on = false;
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count();
        g_gx2_perf_ns[slot] += uint64_t(ns);
        ++g_gx2_perf_calls[slot];
    }
    Gx2PerfScope(const Gx2PerfScope&) = delete;
    Gx2PerfScope& operator=(const Gx2PerfScope&) = delete;
};

// Called once per GX2SwapScanBuffers, on the thread that presents.
inline void gx2_perf_frame() {
#ifndef RBREW_DISABLE_LOGGING
    if (!gx2_perf_enabled()) return;
    using clock = std::chrono::steady_clock;
    static const clock::time_point first = clock::now();
    static clock::time_point last = first;
    static unsigned frames = 0;
    static uint64_t frame_wait_start = t_gx2_perf_wait_ns;
    if (++frames < 120) return;
    const auto now = clock::now();
    const double wall_ms = std::chrono::duration<double, std::milli>(now - last).count();
    g_gx2_perf_ns[kPerfFrameWait] += t_gx2_perf_wait_ns - frame_wait_start;
    g_gx2_perf_calls[kPerfFrameWait] += frames;
    frame_wait_start = t_gx2_perf_wait_ns;
    FILE* f = gx2_perf_file();
    std::fprintf(f, "[perf] t=%.0fs %.1f fps, %.2f ms/frame |",
                 std::chrono::duration<double>(now - first).count(), frames * 1000.0 / wall_ms, wall_ms / frames);
    for (unsigned i = 0; i < kPerfSlots; ++i) {
        const uint64_t ns = g_gx2_perf_ns[i].exchange(0), calls = g_gx2_perf_calls[i].exchange(0);
        std::fprintf(f, " %s %.2fms/%llu", kGx2PerfNames[i], ns / 1e6 / frames,
                     static_cast<unsigned long long>(calls / frames));
    }
    std::fprintf(f, " | KiB/frame");
    for (unsigned i = 0; i < kBytesSlots; ++i)
        std::fprintf(f, " %s %.0f", kGx2PerfByteNames[i], g_gx2_perf_bytes[i].exchange(0) / 1024.0 / frames);
    std::fprintf(f, "\n");
    for (unsigned table = 0; table < 2; ++table) {
        struct Row { uintptr_t key; uint64_t ns, calls; };
        std::vector<Row> rows;
        for (auto& e : g_gx2_perf_functions[table])
            if (const uintptr_t key = e.key.load())
                rows.push_back({key, e.ns.exchange(0), e.calls.exchange(0)});
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.ns > b.ns; });
        std::fprintf(f, "[perf-%s]", table ? "call" : "dl");
        for (size_t i = 0; i < rows.size() && i < 14; ++i)
            std::fprintf(f, " %llX:%.2fms/%llu",
                         static_cast<unsigned long long>(rows[i].key == 1 ? 0 : rows[i].key - gx2_perf_module_base()),
                         rows[i].ns / 1e6 / frames, static_cast<unsigned long long>(rows[i].calls / frames));
        std::fprintf(f, "\n");
    }
    if (g_gx2_perf_detail) g_gx2_perf_detail(f, frames);
    std::fflush(f);
    frames = 0; last = now;
#endif
}
