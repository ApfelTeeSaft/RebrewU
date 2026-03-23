// coreinit miscellaneous: time, I/O, system info, debug, mem helpers
#include "../os_common.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <condition_variable>
#include <unordered_map>

using namespace std::chrono;

// Wii U timer runs at ~244.8 MHz
static constexpr uint64_t WIIU_TIMER_FREQ = 244800000ULL;
static auto s_boot_time = steady_clock::now();

static uint64_t get_wiiu_ticks() {
    auto now = steady_clock::now();
    auto us = duration_cast<microseconds>(now - s_boot_time).count();
    return (uint64_t)us * WIIU_TIMER_FREQ / 1000000ULL;
}

// --- Time ---
static void OSGetSystemTime(CPUState* cpu) {
    uint64_t t = get_wiiu_ticks();
    cpu->r[3] = (uint32_t)(t >> 32);
    cpu->r[4] = (uint32_t)(t);
}
static void OSGetTime(CPUState* cpu)  { OSGetSystemTime(cpu); }
static void OSGetTick(CPUState* cpu)  { RET = (uint32_t)get_wiiu_ticks(); }

static void OSTicksToCalendarTime(CPUState* cpu) {
    // r3:r4 = ticks (64-bit), r5 = OSCalendarTime* out
    // Fill in a plausible UTC structure (OSCalendarTime: sec,min,hour,mday,mon,year,wday,yday,msec,usec)
    uint32_t out = ARG2;
    if (out) memset(MEM + out, 0, 40);
    RET = 0;
}

// --- Core info ---
static void OSGetCoreId(CPUState* cpu)    { RET = 0; }
static void OSGetMainCoreId(CPUState* cpu){ RET = 1; }

// --- Logging ---
static void OSReport(CPUState* cpu) { (void)cpu; }
static void OSPanic(CPUState* cpu)  {
    // OSPanic(const char* file, int line, const char* fmt, ...)
    // r3=file, r4=line, r5=fmt, r6..=varargs
    const char* file = ARG0 ? (const char*)(MEM + ARG0) : "?";
    int         line = (int)ARG1;
    const char* fmt  = ARG2 ? (const char*)(MEM + ARG2) : "";
    // Print with up to 3 integer varargs (r6/r7/r8)
    fprintf(stderr, "[OSPanic] %s:%d  ", file, line);
    fprintf(stderr, fmt, cpu->r[6], cpu->r[7], cpu->r[8]);
    fprintf(stderr, "\n");
    // Do NOT abort() — let the game continue.  A real Wii U OSPanic would
    // halt the system, but during bring-up we want to see what happens after
    // non-fatal panics (e.g. "unhandled signal 6" from the fiber scheduler).
    // If the game truly cannot continue it will loop or fault elsewhere, giving
    // us a clearer callchain to diagnose.
}
static void COSWarn(CPUState* cpu)  { (void)cpu; }
static void OSVReport(CPUState* cpu){ (void)cpu; }
static void OSConsoleWrite(CPUState* cpu) { (void)cpu; }

// --- Interrupt control (no-op on PC) ---
static void OSDisableInterrupts(CPUState* cpu) { RET = 1; }
static void OSRestoreInterrupts(CPUState* cpu) { RET = ARG0; }

// --- Process/thread exit ---
// Implemented via GuestThreadExit exception (see coreinit_threads.cpp).
// coreinit_exit_thread() is the real implementation; we just forward to it.
extern void coreinit_exit_thread(CPUState*);
static void OSExitThread(CPUState* cpu) { coreinit_exit_thread(cpu); }
static void exit_fn(CPUState* cpu)  { ::exit((int)ARG0); }
static void _Exit_fn(CPUState* cpu) { ::_Exit((int)ARG0); }
static void OSBlockThreadsOnExit(CPUState* cpu) { (void)cpu; }

// --- Dynamic loading ---
static void OSDynLoad_Acquire(CPUState* cpu) {
    if (ARG1) rbrew_write32(MEM, ARG1, 0x50000042u);
    RET = 0;
}
static void OSDynLoad_FindExport(CPUState* cpu) {
    if (ARG3) rbrew_write32(MEM, ARG3, 0u);
    RET = 0xFFFFFFFFu;
}

// --- Cache (no-op on cache-coherent host) ---
static void DCFlushRange(CPUState* cpu)      { (void)cpu; }
static void DCFlushRangeNoSync(CPUState* cpu){ (void)cpu; }
static void DCInvalidateRange(CPUState* cpu) { (void)cpu; }
static void DCStoreRange(CPUState* cpu)      { (void)cpu; }
static void DCZeroRange(CPUState* cpu) {
    uint32_t addr = ARG0, len = ARG1;
    if (addr && len) memset(MEM + addr, 0, len);
}
static void OSMemoryBarrier(CPUState* cpu) { (void)cpu; }

// --- Memory block ops ---
static void OSBlockMove(CPUState* cpu) {
    uint32_t dst = ARG0, src = ARG1, len = ARG2;
    if (dst && src && len) memmove(MEM + dst, MEM + src, len);
}
static void OSBlockSet(CPUState* cpu) {
    uint32_t dst = ARG0, val = ARG1, len = ARG2;
    if (dst && len) memset(MEM + dst, (int)val, len);
}

// --- Host memcpy/memmove/memset passed through ---
static void memcpy_fn(CPUState* cpu) {
    uint32_t dst = ARG0, src = ARG1, n = ARG2;
    if (dst && src && n && (uint64_t)dst + n <= WIIU_MEM_SIZE && (uint64_t)src + n <= WIIU_MEM_SIZE)
        memcpy(MEM + dst, MEM + src, n);
    RET = dst;
}
static void memmove_fn(CPUState* cpu) {
    uint32_t dst = ARG0, src = ARG1, n = ARG2;
    if (dst && src && n && (uint64_t)dst + n <= WIIU_MEM_SIZE && (uint64_t)src + n <= WIIU_MEM_SIZE)
        memmove(MEM + dst, MEM + src, n);
    RET = dst;
}
static void memset_fn(CPUState* cpu) {
    uint32_t dst = ARG0, val = ARG1, n = ARG2;
    if (dst && n) {
        // Guard against OOB: skip if dst+n would exceed the 768MB arena
        if ((uint64_t)dst + n <= WIIU_MEM_SIZE)
            memset(MEM + dst, (int)val, n);
        else
            fprintf(stderr, "[coreinit] memset OOB: dst=0x%08X n=0x%X\n", dst, n);
    }
    RET = dst;
}

// --- Environment variable ---
static void ENVGetEnvironmentVariable(CPUState* cpu) {
    // r3 = name ptr, r4 = out buf, r5 = buf size
    if (ARG1 && ARG2) rbrew_write8(MEM, ARG1, 0); // empty string
    RET = 0;
}

// --- Debugger ---
static void OSIsDebuggerPresent(CPUState* cpu)    { RET = 0; }
static void OSIsDebuggerInitialized(CPUState* cpu) { RET = 0; }

// --- Home menu ---
static void OSEnableHomeButtonMenu(CPUState* cpu) { (void)cpu; }

// --- Saves done ---
static void OSSavesDone_ReadyToRelease(CPUState* cpu) { (void)cpu; }

// ---------------------------------------------------------------------------
// OSMessageQueue — bounded blocking message queue
//
// Wii U layout (approximate):
//   +0x00 uint32_t tag        (0x4D534751)
//   +0x04 OSThreadQueue recvQ (8 bytes)
//   +0x0C OSThreadQueue sendQ (8 bytes)
//   +0x14 uint32_t* messages  (guest ptr to uint32_t[msgCount])
//   +0x18 uint32_t  msgCount  (capacity)
//   +0x1C uint32_t  firstIndex
//   +0x20 uint32_t  usedCount
//
// We maintain a host-side struct keyed by guest queue address.
// Messages are 32-bit guest values (pointers or opaque IDs).
//
// OSSendMessage flags:    0=non-blocking, 1=blocking, 2=high-priority
// OSReceiveMessage flags: 0=non-blocking, 1=blocking
// ---------------------------------------------------------------------------
struct MsgQueueImpl {
    std::mutex              mtx;
    std::condition_variable not_empty;
    std::condition_variable not_full;
    uint32_t buf_addr = 0;
    uint32_t capacity = 0;
    uint32_t head     = 0;
    uint32_t count    = 0;
};

static std::mutex                                    s_mq_map_lock;
static std::unordered_map<uint32_t, MsgQueueImpl*>  s_msg_queues;

static MsgQueueImpl* get_mq(uint32_t ptr) {
    std::lock_guard<std::mutex> lk(s_mq_map_lock);
    auto it = s_msg_queues.find(ptr);
    if (it != s_msg_queues.end()) return it->second;
    auto* q = new MsgQueueImpl();
    s_msg_queues[ptr] = q;
    return q;
}

// OSInitMessageQueue(queue, msgbuf, msgCount)
// r3=queue ptr, r4=message buffer guest ptr, r5=capacity
static void OSInitMessageQueue(CPUState* cpu) {
    uint32_t q_addr  = ARG0;
    uint32_t buf_ptr = ARG1;
    uint32_t cap     = ARG2;
    if (!q_addr) return;

    MsgQueueImpl* mq = get_mq(q_addr);
    std::lock_guard<std::mutex> lk(mq->mtx);
    mq->buf_addr = buf_ptr;
    mq->capacity = cap ? cap : 1;
    mq->head     = 0;
    mq->count    = 0;

    // Zero the guest struct (tag + counts)
    if ((uint64_t)q_addr + 0x28u < WIIU_MEM_SIZE)
        memset(MEM + q_addr, 0, 0x28);
}

// OSSendMessage(queue, message, flags)
// r3=queue, r4=msg (uint32), r5=flags (1=blocking)
static void OSSendMessage(CPUState* cpu) {
    uint32_t q_addr = ARG0;
    uint32_t msg    = ARG1;
    bool     block  = (ARG2 & 1) != 0;

    MsgQueueImpl* mq = get_mq(q_addr);
    std::unique_lock<std::mutex> lk(mq->mtx);

    if (block) {
        mq->not_full.wait(lk, [mq]{ return mq->count < mq->capacity; });
    } else {
        if (mq->count >= mq->capacity) { RET = 0; return; } // FALSE = full
    }

    uint32_t tail = (mq->head + mq->count) % mq->capacity;
    if (mq->buf_addr && (uint64_t)mq->buf_addr + tail * 4u + 4u < WIIU_MEM_SIZE)
        rbrew_write32(MEM, mq->buf_addr + tail * 4u, msg);
    mq->count++;
    lk.unlock();
    mq->not_empty.notify_one();
    RET = 1; // TRUE
}

// OSReceiveMessage(queue, msg_out, flags)
// r3=queue, r4=out ptr, r5=flags (1=blocking)
static void OSReceiveMessage(CPUState* cpu) {
    uint32_t q_addr  = ARG0;
    uint32_t out_ptr = ARG1;
    bool     block   = (ARG2 & 1) != 0;

    MsgQueueImpl* mq = get_mq(q_addr);
    std::unique_lock<std::mutex> lk(mq->mtx);

    if (block) {
        mq->not_empty.wait(lk, [mq]{ return mq->count > 0; });
    } else {
        if (mq->count == 0) {
            if (out_ptr) rbrew_write32(MEM, out_ptr, 0u);
            RET = 0; return;
        }
    }

    uint32_t msg = 0;
    if (mq->buf_addr && (uint64_t)mq->buf_addr + mq->head * 4u + 4u < WIIU_MEM_SIZE)
        msg = rbrew_read32(MEM, mq->buf_addr + mq->head * 4u);

    mq->head = (mq->head + 1) % mq->capacity;
    mq->count--;
    lk.unlock();

    if (out_ptr) rbrew_write32(MEM, out_ptr, msg);
    mq->not_full.notify_one();
    RET = 1; // TRUE
}

// --- Alarms (no-op) ---
static void OSCreateAlarm(CPUState* cpu)  { if (ARG0) memset(MEM+ARG0, 0, 0x40); }
static void OSSetAlarm(CPUState* cpu)     { (void)cpu; }
static void OSCancelAlarm(CPUState* cpu)  { RET = 1; }
static void OSCancelAlarms(CPUState* cpu) { (void)cpu; }

// --- GHS C++ runtime stubs ---
static void __ghs_mtx_init(CPUState* cpu)   { (void)cpu; }
static void __ghs_mtx_dst(CPUState* cpu)    { (void)cpu; }
static void __ghs_mtx_lock(CPUState* cpu)   { (void)cpu; }
static void __ghs_mtx_unlock(CPUState* cpu) { (void)cpu; }
static void __ghs_flock_file(CPUState* cpu)   { (void)cpu; }
static void __ghs_flock_ptr(CPUState* cpu)    { (void)cpu; }
static void __ghs_funlock_file(CPUState* cpu) { (void)cpu; }
static void __ghsLock(CPUState* cpu)   { (void)cpu; }
static void __ghsUnlock(CPUState* cpu) { (void)cpu; }
static void __gh_set_errno(CPUState* cpu) { (void)cpu; }

// --- MCP (system settings) ---
static void MCP_Open(CPUState* cpu)  { RET = 1; } // handle 1
static void MCP_Close(CPUState* cpu) { RET = 0; }
static void MCP_GetSysProdSettings(CPUState* cpu) {
    uint32_t out = ARG1;
    if (out) memset(MEM + out, 0, 0x60); // zeroed MCPSysProdSettings
    RET = 0;
}

// --- UC (user config) ---
static void UCOpen(CPUState* cpu)  { RET = 1; }
static void UCClose(CPUState* cpu) { RET = 0; }
static void UCReadSysConfig(CPUState* cpu) { RET = 0xFFFFFFFFu; } // error = not found

// --- Misc ---
static void bspGetHardwareVersion(CPUState* cpu) { RET = 0x00000001u; }
static void OSCoherencyBarrier(CPUState* cpu) { (void)cpu; }
static void OSIsAddressRangeDCValid(CPUState* cpu) { RET = 1; }

#include "coreinit_addrs.h"

// NOTE: Functions not called by this game are not registered here.
// gambit_plt_auto_register() pre-registers STUB for all 610 PLT stubs.
void coreinit_misc_register(CPUState* cpu) {
#define REG(name) rbrew_register_func(cpu, ADDR_##name, name)
    REG(OSGetSystemTime);
    REG(OSGetTime);
    REG(OSGetTick);
    REG(OSTicksToCalendarTime);
    REG(OSGetCoreId);
    REG(OSGetMainCoreId);
    REG(OSReport);
    REG(OSPanic);
    REG(COSWarn);
    REG(OSVReport);
    REG(OSConsoleWrite);
    REG(OSDisableInterrupts);
    REG(OSRestoreInterrupts);
    REG(OSExitThread);
    rbrew_register_func(cpu, ADDR_exit,  exit_fn);
    rbrew_register_func(cpu, ADDR__Exit, _Exit_fn);
    REG(OSBlockThreadsOnExit);
    REG(OSDynLoad_Acquire);
    REG(OSDynLoad_FindExport);
    REG(DCFlushRange);
    REG(DCFlushRangeNoSync);
    REG(DCInvalidateRange);
    REG(DCStoreRange);
    REG(DCZeroRange);
    REG(OSMemoryBarrier);
    REG(OSBlockMove);
    REG(OSBlockSet);
    rbrew_register_func(cpu, ADDR_memcpy,  memcpy_fn);
    rbrew_register_func(cpu, ADDR_memmove, memmove_fn);
    rbrew_register_func(cpu, ADDR_memset,  memset_fn);
    REG(ENVGetEnvironmentVariable);
    REG(OSIsDebuggerPresent);
    REG(OSIsDebuggerInitialized);
    REG(OSEnableHomeButtonMenu);
    REG(OSSavesDone_ReadyToRelease);
    REG(OSInitMessageQueue);
    REG(OSSendMessage);
    REG(OSReceiveMessage);
    REG(OSCreateAlarm);
    REG(OSSetAlarm);
    REG(OSCancelAlarm);
    REG(OSCancelAlarms);
    // OSFastMutex_* are registered in coreinit_sync.cpp
    REG(__ghs_mtx_init);
    REG(__ghs_mtx_dst);
    REG(__ghs_mtx_lock);
    REG(__ghs_mtx_unlock);
    REG(__ghsLock);
    REG(__ghsUnlock);
    REG(__gh_set_errno);
    REG(MCP_Open);
    REG(MCP_Close);
    REG(MCP_GetSysProdSettings);
    REG(UCOpen);
    REG(UCClose);
    REG(UCReadSysConfig);
    REG(bspGetHardwareVersion);
    REG(OSCoherencyBarrier);
    REG(OSIsAddressRangeDCValid);
#undef REG
}
