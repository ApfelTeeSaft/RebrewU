#pragma once
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

struct RbrewProcessPolicy {
    bool high_priority = false;
    bool all_processors = false;
};

inline RbrewProcessPolicy rbrew_configure_game_process() {
    const HANDLE process = GetCurrentProcess();
    const HANDLE thread = GetCurrentThread();
    RbrewProcessPolicy policy;
    // Clear inherited CPU-set selections as well as hard affinity masks.
    // Resolve these optional Windows 10 APIs without changing the minimum OS.
    using SetCpuSets = BOOL (WINAPI*)(HANDLE, const ULONG*, ULONG);
    const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    const auto process_sets = reinterpret_cast<SetCpuSets>(GetProcAddress(kernel, "SetProcessDefaultCpuSets"));
    const auto thread_sets = reinterpret_cast<SetCpuSets>(GetProcAddress(kernel, "SetThreadSelectedCpuSets"));
    const bool cleared_process = !process_sets || process_sets(process, nullptr, 0);
    const bool cleared_thread = !thread_sets || thread_sets(thread, nullptr, 0);
    DWORD_PTR current = 0, system = 0;
    if (GetProcessAffinityMask(process, &current, &system) && system) {
        const bool expanded_process = SetProcessAffinityMask(process, system) != FALSE;
        const bool expanded_thread = SetThreadAffinityMask(thread, system) != 0;
        policy.all_processors = cleared_process && cleared_thread && expanded_process && expanded_thread;
    }
    policy.high_priority = SetPriorityClass(process, HIGH_PRIORITY_CLASS) != FALSE;
    return policy;
}
#endif
