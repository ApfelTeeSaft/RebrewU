#include "../../runtime/rbrew_diagnostics.h"
#include "../../runtime/texture_diagnostics.h"
// GX2 context and initialization
// GX2Init, GX2Shutdown, GX2SetContextState, GX2DrawDone, swap chain
//
// Creates the selected backend's SDL2 window. Vulkan uses SDL_WINDOW_VULKAN
// and a native Vulkan device/swapchain; TV/GamePad toggle retains touch input.

#include "../os_common.h"
#include "../../runtime/screen_mode.h"
#include "gx2_render.h"
#include "gx2_pm4_state.h"
#include "gx2_perf.h"
#include "gx2_vblank.h"
#include "../../runtime/host_sleep.h"
#include <cstring>
#include <mutex>
#include "gx2_native_context.h"
#include "gx2_surface.h"
#include "gx2_display_list.h"
#include <cstdlib>
#include <string>
#include <stdio.h>
#include <cstdio>
#include <atomic>
#include <thread>
#include <array>
#include <vector>

// Definitions for shared screen-mode state (declared in screen_mode.h)
ScreenMode g_screen_mode  = ScreenMode::TV;
TouchState g_touch_state  = { false, 0, 0 };

void gx2_draw_set_default_state(CPUState* cpu);

// SDL windowing is optional for headless tools and tests.
#if defined(HAVE_SDL2)
#include <SDL2/SDL.h>
#if defined(_WIN32)
#include <SDL2/SDL_syswm.h>
#endif
#include "vk/gx2_vk_shader.h"
// gx2_render_vk.cpp owns the Vulkan device; this branch needs no GPU types.

static SDL_Window*   s_window    = nullptr;
static bool          s_tv_enabled = true;

// GamePad screen resolution (hardware fixed)
static constexpr int DRC_W = 854;
static constexpr int DRC_H = 480;

// A visible rate measurement without console/file diagnostics or GPU readback.
static uint64_t s_fps_sample_tick = 0;
static unsigned s_fps_frames = 0;
static double s_window_fps = 0;
static void update_window_title() {
    if (!s_window) return;
    constexpr const char* backend = "Vulkan";
    char title[192];
    std::snprintf(title, sizeof(title), "Splatoon | %s | %s | %.1f FPS  (Tab = switch | F11 = fullscreen)",
                  backend, g_screen_mode == ScreenMode::TV ? "TV" : "Gamepad", s_window_fps);
    SDL_SetWindowTitle(s_window, title);
}

// Switch between TV and Gamepad view, adjusting mouse capture accordingly
static void toggle_screen_mode() {
    if (g_screen_mode == ScreenMode::TV) {
        g_screen_mode = ScreenMode::Gamepad;
        SDL_SetRelativeMouseMode(SDL_FALSE); // free the cursor for touch sim
        g_touch_state = { false, 0, 0 };
    } else {
        g_screen_mode = ScreenMode::TV;
        SDL_SetRelativeMouseMode(SDL_TRUE);  // capture cursor for camera
        g_touch_state = { false, 0, 0 };
    }
    update_window_title();
}

// Map a window-space mouse position to GamePad touch coordinates [0,853] x [0,479]
static void map_mouse_to_touch(int mx, int my) {
    int ww, wh;
    SDL_GetWindowSize(s_window, &ww, &wh);
    if (ww <= 0 || wh <= 0) return;

    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    if (mx >= ww) mx = ww - 1;
    if (my >= wh) my = wh - 1;

    g_touch_state.x = (uint16_t)((mx * (DRC_W - 1)) / (ww - 1));
    g_touch_state.y = (uint16_t)((my * (DRC_H - 1)) / (wh - 1));
}

#if defined(_WIN32)
extern "C" __declspec(dllimport) void __stdcall DisableProcessWindowsGhosting(void);
extern "C" __declspec(dllimport) void* __stdcall GetCurrentProcess(void);
extern "C" __declspec(dllimport) int __stdcall TerminateProcess(void* process, unsigned int code);
#endif
static SDL_threadID s_window_thread = 0;

void gx2_render_wait_cache_writes(uint32_t timeout_ms);
// Window close. exit() ran static destructors while the render, recording and
// compiler threads were still running; a joinable std::thread among them
// aborted every close with a crash report (owner WER dumps 2026-10-17).
// Guest file writes use C streams: flush them, let queued shader-cache files
// finish (bounded), then end the process without destructors.
[[noreturn]] static void gx2_close_requested() {
    RBREW_LOGF(stderr, "[gx2] Window close requested\n");
    gx2_render_wait_cache_writes(2000);
    std::fflush(nullptr);
    TXDIAG(txdiag::finish());
#if defined(_WIN32)
    TerminateProcess(GetCurrentProcess(), 0);
#endif
    std::_Exit(0);
}

// Keep the window responsive before the game presents its first frame:
// process OS messages on the owning thread (at most every 50 ms). SDL events
// stay queued for GX2SwapScanBuffers; only a close request is acted on here.
static void gx2_pump_window_messages() {
    if (!s_window || SDL_ThreadID() != s_window_thread) return;
    static Uint32 s_last = 0;
    const Uint32 now = SDL_GetTicks();
    if (now - s_last < 50) return;
    s_last = now;
    SDL_PumpEvents();
    if (SDL_PeepEvents(nullptr, 0, SDL_PEEKEVENT, SDL_QUIT, SDL_QUIT) > 0) gx2_close_requested();
}

static bool gx2_platform_open_window() {
#if defined(RBREW_DISABLE_LOGGING)
    // SDL's internal diagnostics use their own callback, outside RBREW_LOGF.
    SDL_LogSetOutputFunction([](void*, int, SDL_LogPriority, const char*) {}, nullptr);
#endif
#if defined(_WIN32)
    // A long load before the first presented frame must not turn the window
    // into Windows' "(Not Responding)" ghost with a close prompt.
    DisableProcessWindowsGhosting();
#endif
    // VPADRead maps controller buttons by position. SDL otherwise labels
    // Nintendo-layout pads (Switch Pro, PowerA Spectra and other HIDAPI
    // Switch controllers) by their printed letters, swapping A/B and X/Y.
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    // Keep reading the controller while another window has focus.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        RBREW_LOGF(stderr, "[gx2] SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    // Vulkan presentation: no GPU attributes or context. The device and
    // swapchain are created by gx2_render_init for this window; the present
    // mode avoids a second refresh wait (see gx2_vk.cpp create_swapchain).
    s_window = SDL_CreateWindow("Splatoon | Vulkan | TV",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1920, 1080, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!s_window) {
        RBREW_LOGF(stderr, "[gx2] SDL_CreateWindow(SDL_WINDOW_VULKAN) failed: %s\n", SDL_GetError());
        return false;
    }
#if defined(_WIN32)
    SDL_SysWMinfo window_info{};
    SDL_VERSION(&window_info.version);
    if (SDL_GetWindowWMInfo(s_window, &window_info)) {
        const HICON icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101));
        if (icon) {
            SendMessageW(window_info.info.win.window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
            SendMessageW(window_info.info.win.window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
        }
    }
#endif
    gx2_native_set_context(s_window, nullptr);
    s_window_thread = SDL_ThreadID();
    if (!std::getenv("RBREW_VK_CACHE_DIR")) {
        // Stable SPIR-V / pipeline cache next to the executable.
        if (char* base = SDL_GetBasePath()) { gx2vk::shader_set_cache_dir(std::string(base) + "vulkan_cache"); SDL_free(base); }
    }

    // Start in TV view with mouse captured for camera control.
    // Raise the window first so it has focus, then flush any spurious events
    // (e.g. SDL_QUIT) that SDL may have queued during window creation or
    // SDL_SetRelativeMouseMode before the window was visible.
    SDL_RaiseWindow(s_window);
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    SDL_SetRelativeMouseMode(SDL_TRUE);
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    g_screen_mode = ScreenMode::TV;
    s_fps_sample_tick = 0; s_fps_frames = 0; s_window_fps = 0;
    update_window_title();

    // Initialize the selected native renderer for the window.
    gx2_render_init();
    gx2_native_start_render_thread();
    gx2_render_startup();   // splash + shader warm-up ahead of the first frame
    return true;
}
#endif // HAVE_SDL2

// ---------------------------------------------------------------------------
// GX2Init(attribs) → TRUE
// ---------------------------------------------------------------------------
static void GX2Init(CPUState* cpu) {
#if defined(HAVE_SDL2)
    if (!s_window) {
        gx2_platform_open_window();
    }
#else
    RBREW_LOGF(stderr, "[gx2] GX2Init called (SDL2 not available, stub)\n");
#endif
    RET = 1;
}

// GX2Shutdown()
static void GX2Shutdown(CPUState* cpu) {
#if defined(HAVE_SDL2)
    gx2_native_stop_render_thread();
    gx2_render_shutdown();
    if (s_window) { SDL_DestroyWindow(s_window);   s_window = nullptr; }
    SDL_Quit();
#endif
}

// GX2SetContextState(state*)
namespace {
struct ContextRange {
    uint32_t memory_offset;
    uint32_t register_base;
    uint32_t first;
    uint32_t count;
};

constexpr ContextRange kContextRanges[] = {
    {0x0000, 0x2000, 0x300, 6}, {0x0000, 0x2000, 0x900, 0x48},
    {0x0000, 0x2000, 0x980, 0x48}, {0x0000, 0x2000, 0xA00, 0x48},
    {0x0000, 0x2000, 0x310, 0xC}, {0x0000, 0x2000, 0x542, 1},
    {0x0000, 0x2000, 0x235, 1}, {0x0000, 0x2000, 0x232, 2},
    {0x0000, 0x2000, 0x23A, 1}, {0x0000, 0x2000, 0x256, 1},
    {0x0000, 0x2000, 0x60C, 1}, {0x0000, 0x2000, 0x5C5, 1},
    {0x0000, 0x2000, 0x2C8, 1}, {0x0000, 0x2000, 0x363, 1},
    {0x0000, 0x2000, 0x404, 2},
    {0x2C00, 0xA000, 0x000, 2}, {0x2C00, 0xA000, 0x003, 3},
    {0x2C00, 0xA000, 0x00A, 4}, {0x2C00, 0xA000, 0x010, 0x38},
    {0x2C00, 0xA000, 0x050, 0x34}, {0x2C00, 0xA000, 0x08E, 4},
    {0x2C00, 0xA000, 0x094, 0x40}, {0x2C00, 0xA000, 0x100, 9},
    {0x2C00, 0xA000, 0x10C, 3}, {0x2C00, 0xA000, 0x10F, 0x60},
    {0x2C00, 0xA000, 0x185, 0xA}, {0x2C00, 0xA000, 0x191, 0x27},
    {0x2C00, 0xA000, 0x1E0, 9}, {0x2C00, 0xA000, 0x200, 1},
    {0x2C00, 0xA000, 0x202, 7}, {0x2C00, 0xA000, 0x0E0, 0x20},
    {0x2C00, 0xA000, 0x210, 0x29}, {0x2C00, 0xA000, 0x250, 0x34},
    {0x2C00, 0xA000, 0x290, 1}, {0x2C00, 0xA000, 0x292, 2},
    {0x2C00, 0xA000, 0x2A1, 1}, {0x2C00, 0xA000, 0x2A5, 1},
    {0x2C00, 0xA000, 0x2A8, 2}, {0x2C00, 0xA000, 0x2AC, 3},
    {0x2C00, 0xA000, 0x2CA, 1}, {0x2C00, 0xA000, 0x2CC, 1},
    {0x2C00, 0xA000, 0x2CE, 1}, {0x2C00, 0xA000, 0x300, 9},
    {0x2C00, 0xA000, 0x30C, 1}, {0x2C00, 0xA000, 0x312, 1},
    {0x2C00, 0xA000, 0x316, 2}, {0x2C00, 0xA000, 0x343, 2},
    {0x2C00, 0xA000, 0x349, 3}, {0x2C00, 0xA000, 0x34C, 2},
    {0x2C00, 0xA000, 0x351, 1}, {0x2C00, 0xA000, 0x37E, 6},
    {0x2C00, 0xA000, 0x2B4, 3}, {0x2C00, 0xA000, 0x2B8, 3},
    {0x2C00, 0xA000, 0x2BC, 3}, {0x2C00, 0xA000, 0x2C0, 3},
    {0x2C00, 0xA000, 0x2C8, 1}, {0x2C00, 0xA000, 0x29B, 1},
    {0x2C00, 0xA000, 0x08C, 1}, {0x2C00, 0xA000, 0x0D5, 1},
    {0x2C00, 0xA000, 0x284, 0xC},
    {0x3C00, 0xC000, 0, 0x800}, {0x5C00, 0xF880, 0, 0x60},
    {0x5E00, 0xE000, 0, 0x70}, {0x5E00, 0xE000, 0x380, 0x70},
    {0x5E00, 0xE000, 0x460, 0x70}, {0x5E00, 0xE000, 0x7E0, 0x70},
    {0x5E00, 0xE000, 0x8B9, 7}, {0x5E00, 0xE000, 0x8C0, 0x70},
    {0x5E00, 0xE000, 0x930, 0x70}, {0x5E00, 0xE000, 0xCB0, 0x70},
    {0x5E00, 0xE000, 0xD89, 7},
    {0x9500, 0xF000, 0, 0x36}, {0x9500, 0xF000, 0x36, 0x36},
    {0x9500, 0xF000, 0x6C, 0x36},
};

std::array<uint32_t,3> s_active_context_state{};

template<class Visitor>
void visit_context_words(uint32_t state, Visitor&& visitor) {
    for (const auto& range : kContextRanges) {
        for (uint32_t i = 0; i < range.count; ++i) {
            const uint32_t memory_index = range.first + i;
            visitor(range.register_base + memory_index,
                    state + range.memory_offset + memory_index * 4);
        }
    }
}

constexpr size_t context_register_words() {
    size_t words = 0;
    for (const auto& range : kContextRanges) words += range.count;
    return words;
}

template<class ReadWord>
void restore_context_words(ReadWord&& read_word) {
    std::vector<Gx2RegisterWrite> writes;
    writes.reserve(context_register_words());
    for (const auto& range : kContextRanges) {
        for (uint32_t i = 0; i < range.count; ++i) {
            const uint32_t memory_index = range.first + i;
            writes.push_back({range.register_base + memory_index,
                              read_word(range.memory_offset + memory_index * 4)});
        }
    }
    gx2_render_apply_registers(writes.data(), writes.size());

    std::array<uint32_t, 0x400> uniforms{};
    for (uint32_t i = 0; i < uniforms.size(); ++i)
        uniforms[i] = read_word(0x3C00 + i * 4);
    gx2_render_set_uniform_words(true, 0, uniforms.data(), uint32_t(uniforms.size()));
    for (uint32_t i = 0; i < uniforms.size(); ++i)
        uniforms[i] = read_word(0x3C00 + (0x400 + i) * 4);
    gx2_render_set_uniform_words(false, 0, uniforms.data(), uint32_t(uniforms.size()));
}

void restore_context_state(CPUState* cpu, uint32_t state) {
    restore_context_words([cpu, state](uint32_t offset) {
        return rbrew_read32(MEM, state + offset);
    });
}

// Each range (and each uniform bank) is one contiguous guest run: store it with
// one write instead of one per word. The same bytes land at the same
// addresses in the same range order; a run that is not entirely accessible
// falls back to per-word stores, which skip the inaccessible words as before.
void write_context_run(uint8_t* mem, uint32_t address, const uint32_t* words, uint32_t count) {
    if (rbrew_memory_accessible(mem, address, count * 4)) {
        std::array<uint32_t, 0x800> be;
        for (uint32_t i = 0; i < count; ++i) be[i] = rbrew_bswap32(words[i]);
        rbrew_write_memory(mem, address, be.data(), count * 4);
        return;
    }
    for (uint32_t i = 0; i < count; ++i) rbrew_write32(mem, address + i * 4, words[i]);
}

void write_context_state(uint8_t* mem, uint32_t state) {
    std::array<uint32_t, 0x800> words;
    for (const auto& range : kContextRanges) {
        for (uint32_t i = 0; i < range.count; ++i) words[i] = gx2_render_register(range.register_base + range.first + i);
        write_context_run(mem, state + range.memory_offset + range.first * 4, words.data(), range.count);
    }
    for (const bool pixel : {true, false}) {
        for (uint32_t i = 0; i < 0x400; ++i) words[i] = gx2_render_uniform_word(pixel, i);
        write_context_run(mem, state + 0x3C00 + (pixel ? 0 : 0x400) * 4, words.data(), 0x400);
    }
}

uint32_t read_context_snapshot_word(const uint8_t* snapshot, uint32_t offset) {
    return (uint32_t(snapshot[offset]) << 24) | (uint32_t(snapshot[offset + 1]) << 16) |
           (uint32_t(snapshot[offset + 2]) << 8) | uint32_t(snapshot[offset + 3]);
}

void restore_context_snapshot(CPUState*, const uint8_t* snapshot) {
    restore_context_words([snapshot](uint32_t offset) {
        return read_context_snapshot_word(snapshot, offset);
    });
}

void snapshot_context_state(CPUState* cpu, uint32_t state) {
    std::memset(MEM + state, 0, 0xA100);
    rbrew_write32(MEM, state + 0x9800, ARG1 & 1);
    rbrew_write32(MEM, state + 0x9808, 0);
    write_context_state(MEM, state);
    (void)cpu;
}

void save_active_context(CPUState* cpu, uint32_t state) {
    write_context_state(MEM, state);
    gx2_render_save_context_state(state);

    const uint32_t list_size = rbrew_read32(MEM, state + 0x9804);
    if (!list_size) return;

    const uint32_t updated_size = gx2_dl_create_snapshot(
        cpu, state + 0x9E00, 0x300, restore_context_snapshot,
        std::span<const uint8_t>(MEM + state, 0x9800));
    if (updated_size)
        rbrew_write32(MEM, state + 0x9804, updated_size);
    else
        RBREW_LOGF(stderr, "[gx2] Failed to refresh active context shadow list at %08X\n", state);
}
}

static void GX2SetContextState(CPUState* cpu) {
    if (!ARG0) return; // Native register writes are always shadowed in renderer-owned state.
    const uint32_t core=rbrew_current_core();
    if(core>=s_active_context_state.size()) {
        RBREW_LOGF(stderr,"[gx2] GX2SetContextState received an invalid core id %u\n",core);
        return;
    }
    auto& active_context_state=s_active_context_state[core];
    const uint32_t state = ARG0;
    if (!rbrew_memory_accessible(MEM, state, 0xA100)) {
        RBREW_LOGF(stderr, "[gx2] GX2SetContextState received an invalid state address %08X\n", state);
        return;
    }
    // The GPU shadows register writes into the active context's memory, so
    // reselecting the active context reloads its current state, not the
    // values captured when it was last switched away from.
    if (active_context_state &&
        rbrew_memory_accessible(MEM, active_context_state, 0xA100))
        save_active_context(cpu, active_context_state);
    const uint32_t list_size = rbrew_read32(MEM, state + 0x9804);
    extern bool g_gx2_frame_trace_active;
    if (g_gx2_frame_trace_active)
        RBREW_LOGF(stderr, "[ftrace] SetContextState core=%u active=%08X state=%08X list=%u\n", core, active_context_state, state, list_size);
    if (list_size)
        gx2_dl_call(cpu, state + 0x9E00, list_size);
    else
        restore_context_state(cpu, state);
    gx2_render_restore_context_state(state, MEM);
    active_context_state = state;
}

// GX2GetContextStateDisplayList(state, outPtr, outSize)
static void GX2GetContextStateDisplayList(CPUState* cpu) {
    if (!ARG0 || !rbrew_memory_accessible(MEM, ARG0, 0xA100)) return;
    if (ARG1) rbrew_write32(MEM, ARG1, ARG0 + 0x9E00);
    if (ARG2) rbrew_write32(MEM, ARG2, rbrew_read32(MEM, ARG0 + 0x9804));
}

// GX2DrawDone()
static void GX2DrawDone(CPUState* cpu) {
#if defined(HAVE_SDL2)
    gx2_pump_window_messages();
    RET = gx2_native_gpu_finish() ? 1 : 0; // original PPC returns GX2WaitTimeStamp's BOOL in r3
#else
    static std::atomic_flag missing_gpu_reported = ATOMIC_FLAG_INIT;
    if (!missing_gpu_reported.test_and_set())
        RBREW_LOGF(stderr, "[gx2] GX2DrawDone cannot wait without native GL\n");
    RET = 0;
#endif
}

// GX2Flush()
static void GX2Flush(CPUState* cpu) {
    (void)cpu;
#if defined(HAVE_SDL2)
    gx2_pump_window_messages();
    gx2_native_gpu_flush();
#endif
}

// Swap bookkeeping (gx2.rpl: swap counter DAT_10015620, flip counter
// DAT_10016140, last flip / last vsync OSTime pairs). On the console the
// GPU bumps the flip counter when a queued frame reaches the scan-out; here
// GX2SwapScanBuffers presents synchronously, so the frame is flipped by the
// time it returns.
static std::atomic<uint32_t> s_swap_count{0};
static std::atomic<uint32_t> s_flip_count{0};
static std::atomic<uint64_t> s_last_flip{0};
static std::atomic<uint64_t> s_last_vsync{0};

// GX2WaitForVsync() — blocks until the next vblank (gx2.rpl waits on the
// vsync event; GX2WaitForFlip/GX2SetSwapInterval poll in loops around it).
static void GX2WaitForVsync(CPUState* cpu) {
    Gx2PerfScope perf_scope(kPerfVsync);
    (void)cpu;
#if defined(HAVE_SDL2)
    gx2_pump_window_messages();
#endif
    const uint64_t now = rbrew_system_ticks();
    const uint64_t target = gx2_next_vblank_tick(now,RBREW_TIMER_CLOCK);
    if (target > now)
        rbrew_host_sleep_until(rbrew_boot_time+std::chrono::nanoseconds(
            target/RBREW_TIMER_CLOCK*1000000000ull+target%RBREW_TIMER_CLOCK*1000000000ull/RBREW_TIMER_CLOCK+1u));
    uint64_t previous=s_last_vsync.load();
    while(previous<target && !s_last_vsync.compare_exchange_weak(previous,target)) {}
}

// GX2GetSwapStatus(u32* swapCount, u32* flipCount, OSTime* lastFlip,
//                  OSTime* lastVsync) — gx2.rpl 020032d0.
static void GX2GetSwapStatus(CPUState* cpu) {
    const uint32_t swaps = s_swap_count.load(), flips = s_flip_count.load();
    const uint64_t flip_time = s_last_flip.load(), vsync_time = s_last_vsync.load();
    if (ARG0) rbrew_write32(MEM, ARG0, swaps);
    if (ARG1) rbrew_write32(MEM, ARG1, flips);
    if (ARG2) { rbrew_write32(MEM, ARG2, uint32_t(flip_time >> 32)); rbrew_write32(MEM, ARG2 + 4, uint32_t(flip_time)); }
    if (ARG3) { rbrew_write32(MEM, ARG3, uint32_t(vsync_time >> 32)); rbrew_write32(MEM, ARG3 + 4, uint32_t(vsync_time)); }
}

// GX2CopyColorBufferToScanBuffer(colorBuffer*, scanTarget)
// scanTarget: 1 = GX2_SCAN_TARGET_TV, 4 = GX2_SCAN_TARGET_DRC
static void GX2CopyColorBufferToScanBuffer(CPUState* cpu) {
#if defined(HAVE_SDL2)
    // gx2.rpl encodes the color buffer into the command stream at call time:
    // snapshot it and post the copy behind the frame's draws.
    if (ARG0 && rbrew_memory_accessible(MEM, ARG0, GX2CB_SIZEOF) && gx2_native_async_active()) {
        struct ScanCopy final : Gx2AsyncWork {
            uint32_t address, target; uint8_t* mem; std::array<uint8_t, GX2CB_SIZEOF> descriptor;
            void run() override { gx2_render_copy_to_scan(address, target, mem, descriptor.data()); }
        };
        auto* copy = new ScanCopy;
        copy->address = ARG0; copy->target = ARG1; copy->mem = MEM;
        std::copy_n(MEM + ARG0, GX2CB_SIZEOF, copy->descriptor.data());
        gx2_native_post(copy);
        return;
    }
    gx2_native_run(true, [&] { gx2_render_copy_to_scan(ARG0, ARG1, MEM); });
#else
    gx2_render_copy_to_scan(ARG0, ARG1, MEM);
#endif
}

#if defined(HAVE_SDL2)
// Presents posted to the render thread and completed by it. The guest may
// record one frame while the previous one is still being rendered, as with
// the console's queued flips; it never runs further ahead than that.
static std::atomic<uint64_t> s_presents_posted{0}, s_presents_done{0};
#endif

// GX2SwapScanBuffers()
// Called every frame — pump SDL events, blit game FBO to window, swap buffers.
static void GX2SwapScanBuffers(CPUState* cpu) {
    gx2_perf_frame();
    if (s_swap_count.fetch_add(1) == 0)
        RBREW_LOGF(stderr, "[gx2] GX2SwapScanBuffers called for the first time\n");
#if defined(HAVE_SDL2)
    if (s_window) {
        // Pump SDL events first
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT:
                gx2_close_requested();

            case SDL_KEYDOWN:
                if (ev.key.keysym.scancode == SDL_SCANCODE_TAB) {
                    toggle_screen_mode();
                } else if (ev.key.keysym.scancode == SDL_SCANCODE_F11) {
                    // Toggle fullscreen / windowed
                    bool is_full = (SDL_GetWindowFlags(s_window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
                    SDL_SetWindowFullscreen(s_window,
                        is_full ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                }
                break;

            case SDL_CONTROLLERDEVICEADDED:
                // SDL2 will make the new controller visible to SDL_IsGameController;
                // VPADRead re-scans every frame so it picks it up automatically.
                RBREW_LOGF(stderr, "[vpad] Controller connected (index %d)\n", ev.cdevice.which);
                break;

            case SDL_CONTROLLERDEVICEREMOVED:
                RBREW_LOGF(stderr, "[vpad] Controller disconnected (instance %d)\n", ev.cdevice.which);
                // VPADRead detects SDL_GameControllerGetAttached==false and re-scans.
                break;

            case SDL_MOUSEBUTTONDOWN:
                if (g_screen_mode == ScreenMode::Gamepad &&
                    ev.button.button == SDL_BUTTON_LEFT) {
                    map_mouse_to_touch(ev.button.x, ev.button.y);
                    g_touch_state.touched = true;
                }
                break;

            case SDL_MOUSEBUTTONUP:
                if (g_screen_mode == ScreenMode::Gamepad &&
                    ev.button.button == SDL_BUTTON_LEFT) {
                    g_touch_state.touched = false;
                }
                break;

            case SDL_MOUSEMOTION:
                if (g_screen_mode == ScreenMode::Gamepad && g_touch_state.touched) {
                    map_mouse_to_touch(ev.motion.x, ev.motion.y);
                }
                break;

            default:
                break;
            }
        }

        // Blit game FBO to window and swap (events stay on this thread)
        bool gamepad = (g_screen_mode == ScreenMode::Gamepad);
        if (gx2_native_async_active()) {
            struct Present final : Gx2AsyncWork {
                SDL_Window* window; bool gamepad;
                Present(SDL_Window* w, bool g) : window(w), gamepad(g) {}
                void run() override {
                    struct Done { ~Done() { s_presents_done.fetch_add(1, std::memory_order_release); s_presents_done.notify_all(); } } done;
                    gx2_render_present(window, gamepad);
                }
            };
            // The frame must be complete before the swap returns: the game
            // reuses its per-frame buffers (display lists, vertex and uniform
            // data) once it has swapped, and its GPU timestamps cannot see
            // work still queued here. Commands within the frame stay async.
            const uint64_t posted = s_presents_posted.fetch_add(1) + 1;
            gx2_native_post(new Present(s_window, gamepad));
            Gx2PerfScope wait_scope(kPerfFrameWait);
            for (uint64_t done = s_presents_done.load(std::memory_order_acquire); done < posted;
                 done = s_presents_done.load(std::memory_order_acquire))
                s_presents_done.wait(done, std::memory_order_acquire);
        } else {
            gx2_native_run(true, [&] { gx2_render_present(s_window, gamepad); });
        }
    }
#endif
    (void)cpu;
    s_flip_count.store(s_swap_count.load());
    const uint64_t flip_tick = rbrew_system_ticks();
    s_last_flip.store(flip_tick);
#if defined(HAVE_SDL2)
    if (s_window) {
        if (!s_fps_sample_tick) { s_fps_sample_tick = flip_tick; s_fps_frames = 0; }
        else if (++s_fps_frames; flip_tick - s_fps_sample_tick >= RBREW_TIMER_CLOCK) {
            s_window_fps = double(s_fps_frames) * RBREW_TIMER_CLOCK / double(flip_tick - s_fps_sample_tick);
            s_fps_sample_tick = flip_tick; s_fps_frames = 0;
            update_window_title();
        }
    }
#endif
}

// GX2SetTVEnable(enable)
static void GX2SetTVEnable(CPUState* cpu) {
#if defined(HAVE_SDL2)
    s_tv_enabled = (ARG0 != 0);
#else
    (void)cpu;
#endif
}

// GX2SetDRCEnable(enable)
static void GX2SetDRCEnable(CPUState* cpu) { (void)cpu; }

// GX2GetSystemTVScanMode() → GX2_TV_SCAN_MODE_1080P (4)
static void GX2GetSystemTVScanMode(CPUState* cpu) { RET = 4; }

// GX2GetSystemDRCScanMode() → GX2_DRC_SCAN_MODE_DISABLED (0)
static void GX2GetSystemDRCScanMode(CPUState* cpu) { RET = 0; }

static void GX2SetTVGamma(CPUState* cpu)  { (void)cpu; }
static void GX2SetDRCScale(CPUState* cpu) { (void)cpu; }
static void GX2GetLastFrame(CPUState* cpu){ (void)cpu; }

static void GX2GetLastFrameGamma(CPUState* cpu) { cpu->f[1] = 2.2; }

static void GX2SetDefaultState(CPUState* cpu) { gx2_draw_set_default_state(cpu); }

static void GX2CalcContextStateSize(CPUState* cpu) {
    if (ARG0) rbrew_write32(MEM, ARG0, 0x1000u);
}

static void GX2SetupContextStateEx(CPUState* cpu) {
    if (!ARG0 || !rbrew_memory_accessible(MEM, ARG0, 0xA100)) {
        RBREW_LOGF(stderr, "[gx2] GX2SetupContextStateEx received an invalid state address %08X\n", ARG0);
        return;
    }
    const uint32_t state = ARG0;
    const uint32_t flags = ARG1;
    GX2SetDefaultState(cpu);
    snapshot_context_state(cpu, state);
    gx2_render_save_context_state(state);
    if ((flags & 2) == 0) {
        const uint32_t size = gx2_dl_create_snapshot(
            cpu, state + 0x9E00, 0x300, restore_context_snapshot,
            std::span<const uint8_t>(MEM + state, 0x9800));
        rbrew_write32(MEM, state + 0x9804, size);
    }
}

// Cafe's Latte/GPU7 compatibility revision, independent of the host GPU.
// GFD readers use this no-argument query to validate shader file headers.
static void GX2TempGetGPUVersion(CPUState* cpu) { RET = 2; }

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
#include "gx2_addrs.h"

void gx2_context_register(CPUState* cpu) {
#if defined(HAVE_SDL2)
#define REG(name) rbrew_register_func(cpu, ADDR_##name, GX2_NATIVE_CALL(name))
#else
#define REG(name) rbrew_register_func(cpu, ADDR_##name, gx2_command_call<name, gx2_is_command(#name)>)
#endif
    REG(GX2Init);
    REG(GX2SetContextState);
    REG(GX2GetContextStateDisplayList);
    REG(GX2DrawDone);
    REG(GX2Flush);
    REG(GX2WaitForVsync);
    REG(GX2GetSwapStatus);
    REG(GX2CopyColorBufferToScanBuffer);
    REG(GX2SwapScanBuffers);
    REG(GX2SetTVEnable);
    REG(GX2SetDRCEnable);
    REG(GX2SetTVGamma);
    REG(GX2SetDRCScale);
    REG(GX2GetLastFrame);
    REG(GX2SetDefaultState);
    REG(GX2SetupContextStateEx);
    REG(GX2TempGetGPUVersion);
#undef REG
}
