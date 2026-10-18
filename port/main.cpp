#include "runtime/rbrew_diagnostics.h"
// Gambit PC Port — main entry point
//
// Boot sequence:
//  1. Allocate the native guest arena, including the application heap budget
//  2. Initialize dispatch table
//  3. Register all OS module thunks
//  4. Copy data sections into guest arena
//  5. Set up initial CPUState (stack, SDA bases, etc.)
//  6. Call the game's entry point

#include "runtime/rebrewu_runtime.h"
#include "runtime/host_process.h"
#include "os/erreula/erreula.h"
#include "os/coreinit/coreinit_overlay.h"
#include "os/coreinit/coreinit_system_settings.h"
#include "os/coreinit/coreinit_user_config.h"
#include "loader/native_console_profile.h"
#include <array>
#include <atomic>
#include <exception>
#include <utility>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct OverlayHostMapping {
    uint8_t* arena;
    bool committed{};
};

#if defined(_WIN32)
#include <windows.h>
static void platform_sleep_us(unsigned us) { Sleep(us / 1000); }
static uint8_t* alloc_arena(size_t sz) {
    // Write watch lets the renderer skip guest pages the CPU did not write
    // since it last copied them (gx2_write_watch.h).
    auto p = static_cast<uint8_t*>(VirtualAlloc(nullptr, sz, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_NOACCESS));
    if (p && (!VirtualAlloc(p, RBREW_APPLICATION_ARENA_END, MEM_COMMIT, PAGE_READWRITE) ||
              !VirtualAlloc(p + RBREW_FG_FREE_BASE, RBREW_FG_FREE_SIZE, MEM_COMMIT, PAGE_READWRITE) ||
              !VirtualAlloc(p + RBREW_MEM1_BASE, RBREW_MEM1_SIZE, MEM_COMMIT, PAGE_READWRITE) ||
              !VirtualAlloc(p + RBREW_SHARED_DATA_BASE, RBREW_SHARED_DATA_SIZE, MEM_COMMIT, PAGE_READWRITE))) {
        VirtualFree(p, 0, MEM_RELEASE);
        return nullptr;
    }
    return p;
}
static bool map_overlay(void* user, uint32_t base, uint32_t size, bool enable) {
    if (!user || base != RBREW_OVERLAY_BASE || size != RBREW_OVERLAY_SIZE) return false;
    auto& mapping = *static_cast<OverlayHostMapping*>(user);
    auto p = mapping.arena + base;
    if (enable) {
        if (!mapping.committed) {
            if (VirtualAlloc(p, size, MEM_COMMIT, PAGE_READWRITE) != p) return false;
            mapping.committed = true;
        } else {
            DWORD old_protection;
            if (!VirtualProtect(p, size, PAGE_READWRITE, &old_protection)) return false;
        }
        try {
            rbrew_set_overlay_access(mapping.arena, true);
        } catch (...) {
            DWORD old_protection;
            VirtualProtect(p, size, PAGE_NOACCESS, &old_protection);
            throw;
        }
        RBREW_LOGF(stderr, "[MEM] OverlayArena mapped: base=%08X size=%08X\n", base, size);
        return true;
    }
    // Cafe unmaps the aperture; its backing storage survives a later remap.
    DWORD old_protection;
    if (!VirtualProtect(p, size, PAGE_NOACCESS, &old_protection)) return false;
    rbrew_set_overlay_access(mapping.arena, false);
    return true;
}
static void free_arena(uint8_t* p, size_t sz) { (void)sz; VirtualFree(p, 0, MEM_RELEASE); }

// Diagnostic (RBREW_HANG_SAMPLE=<seconds>): after the delay, suspend every
// other thread a few times, read RIP and name the nearest registered guest
// function (host entry <= RIP) from the dispatch table. Read-only otherwise.
#include <tlhelp32.h>
#include <algorithm>
#include <thread>
#include <vector>
static void start_hang_sampler(const CPUState* cpu) {
    const char* value = rbrew_diagnostic_env("RBREW_HANG_SAMPLE");
    if (!value || !*value) return;
    const unsigned delay = unsigned(strtoul(value, nullptr, 10));
    const HostFunc* table = cpu->dispatch_table;
    const uint32_t entries = cpu->dispatch_size;
    std::thread([delay, table, entries] {
        Sleep(delay * 1000);
        std::vector<std::pair<uintptr_t, uint32_t>> hosts;
        for (uint32_t i = 0; i < entries; ++i)
            if (table[i]) hosts.push_back({reinterpret_cast<uintptr_t>(table[i]), i << 2});
        std::sort(hosts.begin(), hosts.end());
        const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
        RBREW_LOGF(stderr, "[hang] module base=%p (map preferred base 0x140000000)\n", (void*)GetModuleHandleW(nullptr));
        for (int round = 0; round < 12; ++round) {
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 entry{}; entry.dwSize = sizeof(entry);
            for (BOOL ok = Thread32First(snap, &entry); ok; ok = Thread32Next(snap, &entry)) {
                if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self) continue;
                HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, entry.th32ThreadID);
                if (!thread) continue;
                CONTEXT context{}; context.ContextFlags = CONTEXT_CONTROL;
                if (SuspendThread(thread) != DWORD(-1)) {
                    const bool got = GetThreadContext(thread, &context);
                    ResumeThread(thread);
                    if (got) {
                        const uintptr_t rip = uintptr_t(context.Rip);
                        auto it = std::upper_bound(hosts.begin(), hosts.end(), std::make_pair(rip, UINT32_MAX));
                        if (it != hosts.begin()) {
                            --it;
                            RBREW_LOGF(stderr, "[hang] round=%d tid=%lu rip=%p nearest guest fn %08X +0x%llX\n", round,
                                    entry.th32ThreadID, (void*)rip, it->second, (unsigned long long)(rip - it->first));
                        }
                    }
                }
                CloseHandle(thread);
            }
            CloseHandle(snap);
            Sleep(500);
        }
    }).detach();
}
// Diagnostic (RBREW_PROFILE=<start s>:<duration s>:<file>): sampling
// profiler. Every ~2 ms suspend each other thread and count its stack: up to
// four Gambit return addresses (RVAs, innermost first; symbolize with the
// linker map), prefixed by "module|" when the thread is in a system DLL or
// driver (time in ntdll/KernelBase is mostly waiting). Writes
// "tid count [module|]rva<rva<...". With RBREW_PROFILE_TRIGGER=<path> the
// window starts <start> seconds after that file appears instead of at launch.
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#include <map>
#include <string>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
// Up to four Gambit return addresses (RVAs) from a resumed thread's context.
// Its stack may change underneath: faults end the walk instead of the game.
static unsigned profile_unwind(CONTEXT walk, uintptr_t exe, uintptr_t exe_end, uintptr_t (&chain)[4]) {
    unsigned found = 0;
    __try {
        for (int depth = 0; depth < 64 && walk.Rip && found < 4; ++depth) {
            if (walk.Rip >= exe && walk.Rip < exe_end) chain[found++] = uintptr_t(walk.Rip) - exe;
            DWORD64 image = 0;
            auto* function = RtlLookupFunctionEntry(walk.Rip, &image, nullptr);
            if (function) {
                void* handler_data = nullptr; DWORD64 frame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, walk.Rip, function, &walk, &handler_data, &frame, nullptr);
            } else {
                walk.Rip = *reinterpret_cast<DWORD64*>(walk.Rsp); walk.Rsp += 8;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return found;
}
static void start_profiler() {
    const char* value = rbrew_diagnostic_env("RBREW_PROFILE");
    if (!value || !*value) return;
    char* end = nullptr;
    const unsigned start = unsigned(strtoul(value, &end, 10));
    const unsigned seconds = end && *end == ':' ? unsigned(strtoul(end + 1, &end, 10)) : 10u;
    const std::string path = end && *end == ':' ? std::string(end + 1) : std::string("profile.txt");
    const char* trigger_value = rbrew_diagnostic_env("RBREW_PROFILE_TRIGGER");
    const std::string trigger = trigger_value ? trigger_value : "";
    std::thread([start, seconds, path, trigger] {
        if (!trigger.empty())
            while (GetFileAttributesA(trigger.c_str()) == INVALID_FILE_ATTRIBUTES) Sleep(250);
        RBREW_LOGF(stderr, "[profile] sampling %u s -> %s\n", seconds, path.c_str());
        Sleep(start * 1000);
        const auto exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        MODULEINFO info{};
        GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(nullptr), &info, sizeof(info));
        const uintptr_t exe_end = exe + info.SizeOfImage;
        auto in_exe = [&](DWORD64 rip) { return rip >= exe && rip < exe_end; };
        const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
        std::map<std::pair<DWORD, std::string>, unsigned> hits;
        std::map<uintptr_t, std::string> module_names;
        std::map<DWORD, HANDLE> threads;
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        const ULONGLONG stop = GetTickCount64() + seconds * 1000ull;
        ULONGLONG refreshed = 0;
        unsigned rounds = 0;
        while (GetTickCount64() < stop) {
            if (GetTickCount64() - refreshed >= 500) {  // thread list changes rarely
                refreshed = GetTickCount64();
                HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                THREADENTRY32 entry{}; entry.dwSize = sizeof(entry);
                for (BOOL ok = Thread32First(snap, &entry); ok; ok = Thread32Next(snap, &entry)) {
                    if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self || threads.count(entry.th32ThreadID)) continue;
                    if (HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, entry.th32ThreadID))
                        threads[entry.th32ThreadID] = thread;
                }
                CloseHandle(snap);
            }
            for (const auto& [tid, thread] : threads) {
                CONTEXT context{}; context.ContextFlags = CONTEXT_FULL;
                if (SuspendThread(thread) == DWORD(-1)) continue;
                const bool got = GetThreadContext(thread, &context);
                // Resume before unwinding: RtlLookupFunctionEntry can take
                // locks the suspended thread holds, which hung the profiler
                // (no output file). The first Gambit frames say who is running
                // or waiting; a racy walk is guarded and only drops a sample.
                ResumeThread(thread);
                if (!got) continue;
                uintptr_t chain[4]{};
                unsigned depth_found = profile_unwind(context, exe, exe_end, chain);
                std::string key;
                if (!in_exe(context.Rip)) {
                    HMODULE module = nullptr;
                    key = "?";
                    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                           reinterpret_cast<LPCWSTR>(context.Rip), &module)) {
                        auto& cached = module_names[reinterpret_cast<uintptr_t>(module)];
                        if (cached.empty()) {
                            char file[MAX_PATH]{};
                            GetModuleBaseNameA(GetCurrentProcess(), module, file, MAX_PATH);
                            cached = file;
                        }
                        key = cached;
                    }
                    key += '|';
                }
                for (unsigned i = 0; i < depth_found; ++i) {
                    char rva[24]; snprintf(rva, sizeof(rva), i ? "<%llX" : "%llX", (unsigned long long)chain[i]);
                    key += rva;
                }
                ++hits[{tid, key}];
            }
            ++rounds;
            if (timer) {
                LARGE_INTEGER due{}; due.QuadPart = -20000;  // 2 ms
                SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                WaitForSingleObject(timer, INFINITE);
            } else {
                Sleep(1);
            }
        }
        for (const auto& [tid, thread] : threads) CloseHandle(thread);
        if (timer) CloseHandle(timer);
        if (FILE* f = fopen(path.c_str(), "w")) {
            std::fprintf(f, "# rounds %u seconds %u\n", rounds, seconds);
            for (const auto& [key, n] : hits) std::fprintf(f, "%lu %u %s\n", key.first, n, key.second.c_str());
            fclose(f);
        } else RBREW_LOGF(stderr, "[profile] cannot write %s\n", path.c_str());
        RBREW_LOGF(stderr, "[profile] finished %u rounds\n", rounds);
    }).detach();
}
#else
static void start_profiler() {}
static void start_hang_sampler(const CPUState*) {}
#include <sys/mman.h>
#include <unistd.h>
static void platform_sleep_us(unsigned us) { usleep(us); }
static uint8_t* alloc_arena(size_t sz) {
    void* p = mmap(nullptr, sz, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    auto* base = static_cast<uint8_t*>(p);
    if (mprotect(p, RBREW_APPLICATION_ARENA_END, PROT_READ | PROT_WRITE) ||
        mprotect(base + RBREW_FG_FREE_BASE, RBREW_FG_FREE_SIZE, PROT_READ | PROT_WRITE) ||
        mprotect(base + RBREW_MEM1_BASE, RBREW_MEM1_SIZE, PROT_READ | PROT_WRITE) ||
        mprotect(base + RBREW_SHARED_DATA_BASE, RBREW_SHARED_DATA_SIZE, PROT_READ | PROT_WRITE)) {
        munmap(p, sz);
        return nullptr;
    }
    return static_cast<uint8_t*>(p);
}
static bool map_overlay(void* user, uint32_t base, uint32_t size, bool enable) {
    if (!user || base != RBREW_OVERLAY_BASE || size != RBREW_OVERLAY_SIZE) return false;
    auto& mapping = *static_cast<OverlayHostMapping*>(user);
    auto p = mapping.arena + base;
    if (mprotect(p, size, enable ? (PROT_READ | PROT_WRITE) : PROT_NONE)) return false;
    mapping.committed = true;
    try {
        rbrew_set_overlay_access(mapping.arena, enable);
    } catch (...) {
        mprotect(p, size, enable ? PROT_NONE : (PROT_READ | PROT_WRITE));
        throw;
    }
    if (enable) RBREW_LOGF(stderr, "[MEM] OverlayArena mapped: base=%08X size=%08X\n", base, size);
    return true;
}
static void free_arena(uint8_t* p, size_t sz) { munmap(p, sz); }
#endif

// ---------------------------------------------------------------------------
// OS module register prototypes
// (implemented in os/ subdirectories)
// ---------------------------------------------------------------------------
void gambit_plt_auto_register(CPUState* cpu); // pre-registers STUB for all 610 PLT stubs

// RBREW_STUB_REPORT diagnostics: one trampoline per blank import slot logs
// its first call (import address and caller LR) and otherwise does nothing,
// exactly like the blank STUB it replaces.
namespace {
constexpr size_t kBlankLoggerSlots = 640;
uint32_t g_blank_import_addr[kBlankLoggerSlots];
std::atomic<bool> g_blank_import_seen[kBlankLoggerSlots];
template <size_t I> void blank_import_trampoline(CPUState* cpu) {
    if (!g_blank_import_seen[I].exchange(true))
        RBREW_LOGF(stderr, "[IMPORT] first call to blank import %08X lr=%08X r3=%08X\n",
                g_blank_import_addr[I], cpu->lr, cpu->r[3]);
}
template <size_t... I>
constexpr std::array<HostFunc, sizeof...(I)> make_blank_loggers(std::index_sequence<I...>) {
    return {&blank_import_trampoline<I>...};
}
HostFunc blank_import_logger(unsigned slot, uint32_t addr) {
    static constexpr auto loggers = make_blank_loggers(std::make_index_sequence<kBlankLoggerSlots>{});
    if (slot >= kBlankLoggerSlots) return nullptr;
    g_blank_import_addr[slot] = addr;
    return loggers[slot];
}
}
#ifdef RBREW_DISTRIBUTABLE_RUNTIME
static void (*Gambit_game_register)(CPUState*);
static void (*Gambit_data_init)(uint8_t*);
namespace rebrewu_generated { static uint32_t Gambit_entry_point; }
#else
void Gambit_game_register(CPUState* cpu);    // registers all recompiled game functions
#endif
void coreinit_memory_init(uint8_t* mem);
void coreinit_memory_register(CPUState* cpu);
void coreinit_dimport_repatch(CPUState* cpu); // re-apply after Gambit_data_init
void coreinit_threads_register(CPUState* cpu);
void coreinit_sync_register(CPUState* cpu);
void coreinit_misc_register(CPUState* cpu);
void coreinit_shared_data_register(CPUState* cpu);
void coreinit_fs_register(CPUState* cpu);
void gx2_context_register(CPUState* cpu);
void gx2_display_list_register(CPUState* cpu);
void gx2_draw_register(CPUState* cpu);
void snd_core_register(CPUState* cpu);
void vpad_register(CPUState* cpu);
void padscore_register(CPUState* cpu);
void stubs_register(CPUState* cpu);
void nsysnet_register(CPUState* cpu);
void nn_act_register(CPUState* cpu);
void sysapp_title_register(CPUState* cpu);

// ---------------------------------------------------------------------------
// Data section loader
// Each data section was emitted into Gambit_data.cpp as a byte array.
// The emitter generates "void Gambit_data_init(uint8_t* arena)" inside
// Gambit_data.cpp when the cpp_emitter is updated.
// A weak fallback is defined in port/loader/gambit_loader.cpp.
// ---------------------------------------------------------------------------
#ifndef RBREW_DISTRIBUTABLE_RUNTIME
void Gambit_data_init(uint8_t* arena);

// The selected generated module supplies its actual RPX entry address.
#include "Gambit.h"
#endif

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
#ifdef RBREW_DISTRIBUTABLE_RUNTIME
static int rbrew_game_main(int argc, char** argv) {
#else
int main(int argc, char** argv) {
#endif
#if defined(_WIN32)
    (void)rbrew_configure_game_process();
    // Explorer, shortcuts and external launchers need the same data/save root.
    // The release must not depend on the launcher's current directory.
    wchar_t executable[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, 32768);
    if (!length || length >= 32768) return 1;
    std::error_code directory_error;
    std::filesystem::current_path(std::filesystem::path(executable).parent_path(), directory_error);
    if (directory_error) return 1;
#endif
    RBREW_LOGF(stderr, "[BOOT] Splatoon launch runtime; RPX header entry=0x%08X\n", rebrewu_generated::Gambit_entry_point);
    RBREW_LOGF(stderr, "[FS] RBREW_CONTENT_DIR=%s\n",
        getenv("RBREW_CONTENT_DIR") ? getenv("RBREW_CONTENT_DIR") : "./content");

    // 1. Allocate the shared native layout, including application heap memory
    const size_t arena_sz = WIIU_MEM_SIZE;
    uint8_t* arena = alloc_arena(arena_sz);
    if (!arena) {
        RBREW_LOGF(stderr, "[gambit] Failed to allocate %zu MB guest arena\n",
                arena_sz >> 20);
        return 1;
    }
    // VirtualAlloc and anonymous mmap already return zero-filled pages. Keep
    // unused arena pages lazy rather than touching every reserved guest byte.
    RBREW_LOGF(stderr, "[MEM] Guest arena: %p – %p (%zu MB)\n",
            (void*)arena, (void*)(arena + arena_sz), arena_sz >> 20);
#if defined(_WIN32)
    {
        // Owner request: use the RAM this machine has. Ask Windows to keep a
        // large working set resident (soft limits: under real memory
        // pressure Windows may still trim; nothing fails if refused).
        const uint64_t committed = uint64_t(RBREW_APPLICATION_ARENA_END) + RBREW_FG_FREE_SIZE + RBREW_MEM1_SIZE + RBREW_SHARED_DATA_SIZE;
        MEMORYSTATUSEX status{};
        status.dwLength = sizeof(status);
        if (GlobalMemoryStatusEx(&status)) {
            const uint64_t min_ws = std::min<uint64_t>(status.ullAvailPhys / 2, committed + (2ull << 30));
            const uint64_t max_ws = std::max<uint64_t>(status.ullTotalPhys / 4 * 3, min_ws + (64ull << 20));
            const bool ok = SetProcessWorkingSetSizeEx(GetCurrentProcess(), SIZE_T(min_ws), SIZE_T(max_ws),
                                                       QUOTA_LIMITS_HARDWS_MIN_DISABLE | QUOTA_LIMITS_HARDWS_MAX_DISABLE) != 0;
            RBREW_LOGF(stderr, "[MEM] System RAM %llu MB, free %llu MB | guest reserved %zu MB, committed %llu MB | "
                            "working set min %llu MB max %llu MB (%s)\n",
                    (unsigned long long)(status.ullTotalPhys >> 20), (unsigned long long)(status.ullAvailPhys >> 20),
                    arena_sz >> 20, (unsigned long long)(committed >> 20), (unsigned long long)(min_ws >> 20),
                    (unsigned long long)(max_ws >> 20), ok ? "set" : "refused");
        }
    }
#endif

    // 2. Allocate dispatch table (covers all addresses up to 0x0D000000)
    const uint32_t DISPATCH_MAX = 0x0D000000u;
    const uint32_t DISPATCH_LEN = DISPATCH_MAX >> 2;
    HostFunc* dispatch_table = (HostFunc*)calloc(DISPATCH_LEN, sizeof(HostFunc));
    if (!dispatch_table) {
        RBREW_LOGF(stderr, "[gambit] Failed to allocate dispatch table\n");
        free_arena(arena, arena_sz);
        return 1;
    }

    // 3. Set up a scratch CPUState for registration calls
    CPUState cpu_reg = {};
    cpu_reg.mem            = arena;
    cpu_reg.dispatch_table = dispatch_table;
    cpu_reg.dispatch_size  = DISPATCH_LEN;
    // Shared with guest workers, like the arena and dispatch table. It must
    // survive main's stack frame while process termination stops the workers.
    auto* overlay_mapping = new OverlayHostMapping{arena};

    // Native Cafe console configuration is separate from content path mapping.
    // Use the owned title's declared region mask; multi-region titles require
    // explicit console selection. No title name or game ordinal is hardcoded.
    try {
        std::filesystem::path metadata;
        if (const char* configured = getenv("RBREW_TITLE_METADATA"); configured && *configured)
            metadata = configured;
#if defined(RBREW_TITLE_METADATA_PATH) && !defined(RBREW_DISABLE_LOGGING)
        else metadata = RBREW_TITLE_METADATA_PATH;
#else
        else {
            const char* content = getenv("RBREW_CONTENT_DIR");
            metadata = std::filesystem::weakly_canonical(content ? content : "./content").parent_path()
                / "meta" / "meta.xml";
        }
#endif
        std::optional<uint32_t> selected;
        if (const char* region = getenv("RBREW_CONSOLE_REGION"); region && *region)
            selected = rbrew_parse_console_region(region);
        const auto regions = rbrew_read_native_regions(metadata, selected);
        RbrewMcpSystemProfile profile;
        profile.product_area = regions.console_region;
        profile.game_region = regions.title_regions;
        rbrew_mcp_configure_profile(&cpu_reg, profile);
        // Explicit native offline profile: English and unregistered country.
        // These match the selected local reference profile, independently of
        // title region. Neither the provider nor guest code depends on Cemu.
        RbrewUcSystemProfile uc_profile{1,0};
        if(const char* value=getenv("RBREW_CONSOLE_LANGUAGE");value && *value)
            uc_profile.language=rbrew_parse_native_uc_scalar(value,11);
        if(const char* value=getenv("RBREW_COUNTRY_REGISTRATION");value && *value)
            uc_profile.country_registration=rbrew_parse_native_uc_scalar(value,255);
        rbrew_uc_configure_profile(&cpu_reg,uc_profile);
        RBREW_LOGF(stderr,"[UC] Native profile: language=%u country_registration=%u\n",
            uc_profile.language,uc_profile.country_registration);
        RBREW_LOGF(stderr, "[MCP] Native profile: console=0x%X title_regions=0x%X metadata=%s\n",
            regions.console_region, regions.title_regions, metadata.string().c_str());
    } catch (const std::exception& error) {
        RBREW_LOGF(stderr, "[MCP] Native profile setup failed: %s\n", error.what());
        delete overlay_mapping;
        free(dispatch_table);
        free_arena(arena, arena_sz);
        return 1;
    }

    // 4. Initialize OS modules and register host thunks
    // Pre-register STUB for all 610 PLT stubs so every import call is safe
    gambit_plt_auto_register(&cpu_reg);
    // Every PLT slot now holds the same blank STUB; remember it so imports
    // that no native module overrides can be reported (RBREW_STUB_REPORT).
    const HostFunc blank_stub = cpu_reg.dispatch_table[0x02D051A0u >> 2];
    // Now override with real implementations
    coreinit_memory_init(arena);
    coreinit_memory_register(&cpu_reg);
    coreinit_threads_register(&cpu_reg);
    coreinit_sync_register(&cpu_reg);
    coreinit_misc_register(&cpu_reg);
    coreinit_shared_data_register(&cpu_reg);
    rbrew_overlay_set_mapper(&cpu_reg, map_overlay, overlay_mapping);
    coreinit_overlay_register(&cpu_reg);
    coreinit_fs_register(&cpu_reg);
    gx2_context_register(&cpu_reg);
    gx2_display_list_register(&cpu_reg);
    gx2_draw_register(&cpu_reg);
    snd_core_register(&cpu_reg);
    vpad_register(&cpu_reg);
    padscore_register(&cpu_reg);
    stubs_register(&cpu_reg);
    nsysnet_register(&cpu_reg);
    nn_act_register(&cpu_reg);
    sysapp_title_register(&cpu_reg);
    // Register all recompiled game functions so rbrew_dispatch can find them
    Gambit_game_register(&cpu_reg);
    erreula_register(&cpu_reg);

    RBREW_LOGF(stderr, "[IMPORT] OS modules and game dispatch registered\n");
    if (const char* report = rbrew_diagnostic_env("RBREW_STUB_REPORT"); report && *report) {
        // PLT trampolines occupy 0x02D051A0..0x02D0E098 in Gambit.
        // Each blank slot is rebound to a logging trampoline with the same
        // do-nothing behavior, so the first call to it is reported.
        if (FILE* out = fopen(report, "w")) {
            unsigned blank = 0;
            for (uint32_t addr = 0x02D05000u; addr < 0x02D10000u; addr += 4)
                if (cpu_reg.dispatch_table[addr >> 2] == blank_stub) {
                    RBREW_LOGF(out, "%08X\n", addr);
                    if (HostFunc logger = blank_import_logger(blank, addr))
                        cpu_reg.dispatch_table[addr >> 2] = logger;
                    ++blank;
                }
            fclose(out);
            RBREW_LOGF(stderr, "[IMPORT] %u PLT imports still bound to the blank STUB -> %s\n", blank, report);
        }
    }

    start_hang_sampler(&cpu_reg);
    start_profiler();

    // 5. Copy data sections into arena
    Gambit_data_init(arena);
    RBREW_LOGF(stderr, "[RPX] Bundled generated data sections loaded\n");

    // 5a. Re-apply the dimport function-pointer patch.
    //
    // Gambit_data_init copies the RPX .data / import-stub sections verbatim,
    // which overwrites the self-referential pointers that coreinit_dimport_patch
    // wrote into the dimport area (0x02D0B8F8 … 0x02D0B908).  Those slots
    // contain 0x00000000 in the original RPX (they are dynamic-reloc placeholders
    // that the Wii U loader resolves at runtime; our static port must do it here).
    //
    // Without this second call every MEMAllocFromDefaultHeap call dispatches to
    // address 0, the game gets a garbage allocation pointer, corrupts the heap,
    // and the GHS runtime calls raise(6) → OSPanic("ind_sgnl.c:105 unhandled signal 6").
    coreinit_dimport_repatch(&cpu_reg);
    RBREW_LOGF(stderr, "[gambit] Dimport function pointers re-patched\n");

    // 5b. Pre-seed the game's custom heap allocator at 0x101D2554.
    //
    // On Wii U the OS initialises this allocator with heap memory before the
    // application's static constructors run.  In our port we inject an initial
    // free block covering [0x20000000, 0x27FFF008) so that the allocator can
    // satisfy malloc requests from ctor[968] onwards without needing to call
    // sbrk (MEMAllocFromDefaultHeapEx) first.
    //
    // Allocator data structure layout (discovered by tracing Gambit_fn_02C86EA8
    // and Gambit_fn_02C86B90):
    //   base+0x00 : type/magic word
    //   base+0x08 : default sbrk chunk size
    //   base+0x10 : free-list head pointer
    //   base+0x1C : sentinel size field  (= sentinel_node - 4)
    //   base+0x20 : sentinel node start  (circular doubly-linked sentinel)
    //   base+0x28 : extent descriptor    (start, end, low, high at +0,+4,+8,+C)
    //   base+0x3C : pointer to extent descriptor (= base+0x28)
    //
    // Free-node layout:
    //   node - 4 : size of this free block (header)
    //   node + 0 : next free node
    //   node + 4 : prev free node
    {
        static constexpr uint32_t ALLOC_BASE = 0x101D2554u;
        static constexpr uint32_t SENTINEL   = ALLOC_BASE + 0x20u; // 0x101D2574
        static constexpr uint32_t EXTENT_HDR = ALLOC_BASE + 0x28u; // 0x101D257C
        // The first free node sits 8 bytes into the heap region so that the
        // 4-byte size field (node-4) and any alignment pad fit before it.
        static constexpr uint32_t FREE_NODE  = 0x20000008u;
        static constexpr uint32_t FREE_SIZE  = 0x07FFF000u; // 128MB - overhead
        static constexpr uint32_t HEAP_START = 0x20000000u; // FREE_NODE - 8
        static constexpr uint32_t HEAP_END   = FREE_NODE + FREE_SIZE; // 0x27FFF008

        // Allocator header
        rbrew_write32(arena, ALLOC_BASE + 0x00u, 0x02080101u); // type marker
        rbrew_write32(arena, ALLOC_BASE + 0x08u, 0x00020000u); // chunk size

        // Free-list head points directly at the first real free node (not the
        // sentinel).  The scan loop reads head.next on its first iteration, so
        // starting from the free node makes the sentinel the "previous" anchor.
        rbrew_write32(arena, ALLOC_BASE + 0x10u, FREE_NODE);   // head

        // Sentinel size field sits at sentinel-4 = ALLOC_BASE+0x1C
        rbrew_write32(arena, ALLOC_BASE + 0x1Cu, 0u);          // size = 0 (sentinel marker)

        // Sentinel node: doubly-linked to the free node
        rbrew_write32(arena, SENTINEL + 0u, FREE_NODE);   // sentinel.next
        rbrew_write32(arena, SENTINEL + 4u, FREE_NODE);   // sentinel.prev

        // Free node: size header + circular links back to sentinel
        rbrew_write32(arena, FREE_NODE - 4u, FREE_SIZE);  // size
        rbrew_write32(arena, FREE_NODE + 0u, SENTINEL);   // free_node.next
        rbrew_write32(arena, FREE_NODE + 4u, SENTINEL);   // free_node.prev

        // Extent descriptor used by bounds-check in Gambit_fn_02C86B90 and by
        // the heap-grow path in Gambit_fn_02C86CB4.
        rbrew_write32(arena, EXTENT_HDR + 0u, HEAP_START);  // extent start (min addr)
        rbrew_write32(arena, EXTENT_HDR + 4u, HEAP_END);    // extent end   (max addr)
        rbrew_write32(arena, ALLOC_BASE + 0x30u, HEAP_START); // tracked low bound
        rbrew_write32(arena, ALLOC_BASE + 0x34u, HEAP_END);   // tracked high bound
        rbrew_write32(arena, ALLOC_BASE + 0x3Cu, EXTENT_HDR); // ptr to extent header

        RBREW_LOGF(stderr, "[gambit] Heap pre-seeded: free=[0x%08X,0x%08X) %u MB\n",
                FREE_NODE, HEAP_END, (HEAP_END - FREE_NODE) >> 20);
    }

    // 6. Set up initial guest CPU state
    //    - r1  = guest stack (top of a 1MB stack block placed at 0x1FC00000)
    //    - r2  = SDA2 base (.rodata anchor)
    //    - r13 = SDA  base (.data anchor)
    const uint32_t STACK_TOP   = 0x1FC00000u;
    const uint32_t STACK_SIZE  = 0x00100000u; // 1MB guest stack

    CPUState cpu = {};
    cpu.mem            = arena;
    cpu.dispatch_table = dispatch_table;
    cpu.dispatch_size  = DISPATCH_LEN;
    rbrew_init_thread_gqrs(&cpu); // main thread starts like any coreinit thread (decaf)
#ifdef RBREW_ENABLE_PPC_FALLBACK
    if (const char* opt = getenv("RBREW_PPC_FALLBACK"); opt && strcmp(opt,"1") == 0) {
        if (!Gambit_ppc_image_init(&cpu, GAMBIT_PPC_IMAGE_PATH)) {
            RBREW_LOGF(stderr,"[hybrid] cannot initialize relocated executable image\n");
            return 1;
        }
        cpu.ppc_fallback_enabled = true;
        RBREW_LOGF(stderr,"[hybrid] bounded integer/control prototype enabled; executable ranges=%u\n",
            cpu.guest_code_range_count);
    }
#endif
    cpu.r[1]  = STACK_TOP - 8;   // r1 = stack pointer (8-byte aligned)
    cpu.r[2]  = 0x10008000u;     // verified FILEINFO SDA2 (offset 0x28)
    cpu.r[13] = 0x10256C80u;     // verified FILEINFO SDA (offset 0x24)

    // 6b. Run static constructors from the GHS ctor table in .rodata.
    //
    // The GHS runtime normally calls these from _start before jumping to main().
    // Our port enters at the game's own entry directly, so we must run them here.
    //
    // Ctor table: array of function pointers in .rodata at 0x101BFA54.
    // Terminated by a null entry.  The GHS "ctors done" flag lives in .bss at
    // 0x101CC020 (zeroed = not yet run). Mark before the first constructor,
    // matching the original GHS initializer, so reentrant or later guest
    // startup observes that initialization has already begun.
    {
        const uint32_t CTOR_TABLE     = 0x101BFA54u;
        const uint32_t CTOR_DONE_FLAG = 0x101CC020u;
        RBREW_LOGF(stderr, "[PPC] Running constructor table at 0x%08X\n", CTOR_TABLE);
        uint32_t n_ctors = rbrew_run_constructors(&cpu, CTOR_TABLE, CTOR_DONE_FLAG);
        RBREW_LOGF(stderr, "[gambit] Ran %u static constructors\n", n_ctors);
    }

    // 7b. Clear the GHS thread-local allocator SDA variable (0x101C98F0).
    //
    // During static constructors the GHS runtime installs a TLS-based
    // allocator and stores its handle in SDA[0x101C98F0].  When that handle
    // is non-null, fn_02801A78 (the GHS malloc wrapper) tries to dereference
    // it through OSGetThreadSpecific, which returns 0 in our single-threaded
    // port, causing every allocation to fail.
    //
    // Clearing the handle forces fn_02801A78 to take the simple fallback path
    // (L_028019F4 → MEMAllocFromDefaultHeap), which works correctly.
    rbrew_write32(arena, 0x101C98F0u, 0u);
    RBREW_LOGF(stderr, "[gambit] GHS TLS allocator handle cleared\n");

    // The loader invokes the module entry; all application callbacks and loops
    // remain guest-owned. A genuine return ends this host launch with its result.
    RBREW_LOGF(stderr, "[gambit] Entering RPX entry point 0x%08X\n", rebrewu_generated::Gambit_entry_point);
    int result = rbrew_run_entry(&cpu, rebrewu_generated::Gambit_entry_point, 0u, 0u);
    RBREW_LOGF(stderr, "[gambit] RPX entry returned %d\n", result);

    // Arena and dispatch are process-owned: guest workers may still reference
    // them until process termination. Do not free them underneath live workers.
    return result;
}

#ifdef RBREW_DISTRIBUTABLE_RUNTIME
// Only boot crosses this boundary. Guest dispatch keeps the existing direct
// function pointers, CPUState layout and exception ABI (clang-cl/MSVC x64).
extern "C" __declspec(dllexport) unsigned rbrew_runtime_abi() { return 1; }
extern "C" __declspec(dllexport) int rbrew_launch(
    void (*register_game)(CPUState*), void (*initialize_data)(uint8_t*), uint32_t entry) {
    if (!register_game || !initialize_data || !entry) return 1;
    // A moved player installation always owns its local resources, even when
    // started by Explorer from a shell with old developer path overrides.
    _putenv_s("RBREW_CONTENT_DIR", "./content");
    _putenv_s("RBREW_MLC_DIR", "./mlc01");
    _putenv_s("RBREW_SYSTEM_MLC_DIR", "./mlc01");
    _putenv_s("RBREW_TITLE_METADATA", "./meta/meta.xml");
    _putenv_s("RBREW_VK_CACHE_DIR", "./vulkan_cache");
    Gambit_game_register = register_game;
    Gambit_data_init = initialize_data;
    rebrewu_generated::Gambit_entry_point = entry;
    return rbrew_game_main(0, nullptr);
}
#endif
