// gx2_host_memory.h - host RAM queries for cache sizing and memory logging.
#pragma once
#include <cstdint>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

struct Gx2HostMemory {
    uint64_t total = 0, available = 0;            // system physical RAM
    uint64_t working_set = 0, private_bytes = 0;  // this process
};

inline Gx2HostMemory gx2_host_memory() {
    Gx2HostMemory m;
#if defined(_WIN32)
    MEMORYSTATUSEX status{sizeof(status)};
    if (GlobalMemoryStatusEx(&status)) { m.total = status.ullTotalPhys; m.available = status.ullAvailPhys; }
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
        m.working_set = counters.WorkingSetSize;
        m.private_bytes = counters.PrivateUsage;
    }
#endif
    return m;
}

// Physical RAM in the machine, read once (0 if unknown).
inline uint64_t gx2_host_total_ram() {
    static const uint64_t total = gx2_host_memory().total;
    return total;
}
