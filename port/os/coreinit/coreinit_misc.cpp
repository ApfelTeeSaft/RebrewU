#include "../../runtime/rbrew_diagnostics.h"
// coreinit miscellaneous: time, I/O, system info, debug, mem helpers
#include "../os_common.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <stdexcept>
#include "coreinit_dynload.h"
#include "coreinit_system_settings.h"
#include "coreinit_user_config.h"

using namespace std::chrono;

static constexpr uint64_t WIIU_TIMER_FREQ = RBREW_TIMER_CLOCK;
// OSGetTime counts timer ticks since 2000-01-01 00:00:00; OSGetSystemTime
// counts since boot. Their difference is OSSystemInfo.baseTime.
static const uint64_t s_base_time = [] {
    const auto since_unix = duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
    const int64_t since_2000 = since_unix - int64_t(946684800) * 1000000;
    return since_2000 > 0 ? uint64_t(since_2000) / 1000000u * WIIU_TIMER_FREQ +
                            uint64_t(since_2000) % 1000000u * WIIU_TIMER_FREQ / 1000000u : 0u;
}();

static void require_guest_range(CPUState* cpu,uint32_t address,uint64_t size) {
    if (uint64_t(address)+size>cpu->guest_memory_size ||
        !rbrew_memory_accessible(MEM,address,size))
        throw std::runtime_error("Coreinit guest buffer is outside accessible memory");
}

static uint64_t get_wiiu_ticks() { return rbrew_system_ticks(); }

// --- Time ---
static void OSGetSystemTime(CPUState* cpu) {
    uint64_t t = get_wiiu_ticks();
    cpu->r[3] = (uint32_t)(t >> 32);
    cpu->r[4] = (uint32_t)(t);
}
static void OSGetTime(CPUState* cpu)  {
    const uint64_t t = s_base_time + get_wiiu_ticks();
    cpu->r[3] = (uint32_t)(t >> 32);
    cpu->r[4] = (uint32_t)(t);
}

// OSGetSystemInfo returns a pointer to coreinit's static OSSystemInfo
// (coreinit.rpl: "return &systemInfo"). Layout and boot values per decaf
// coreinit_systeminfo: busSpeed, coreSpeed, baseTime, l2CacheSize[3], cpuRatio.
uint32_t coreinit_alloc_system_block(uint32_t size); // coreinit_memory.cpp
static void OSGetSystemInfo(CPUState* cpu) {
    static uint32_t info = 0;
    static std::once_flag once;
    std::call_once(once, [&] {
        info = coreinit_alloc_system_block(0x20);
        if (!info) throw std::runtime_error("OSSystemInfo storage could not be allocated");
        rbrew_write32(MEM, info + 0x00, RBREW_BUS_CLOCK);
        rbrew_write32(MEM, info + 0x04, RBREW_CORE_CLOCK);
        rbrew_write32(MEM, info + 0x08, uint32_t(s_base_time >> 32));
        rbrew_write32(MEM, info + 0x0C, uint32_t(s_base_time));
        rbrew_write32(MEM, info + 0x10, 512u * 1024u);
        rbrew_write32(MEM, info + 0x14, 2u * 1024u * 1024u);
        rbrew_write32(MEM, info + 0x18, 512u * 1024u);
        rbrew_write32(MEM, info + 0x1C, RBREW_CORE_CLOCK / RBREW_BUS_CLOCK); // 5
    });
    RET = info;
}
static void OSGetTick(CPUState* cpu)  { RET = (uint32_t)get_wiiu_ticks(); }

static void OSTicksToCalendarTime(CPUState* cpu) {
    // r3:r4 = ticks (64-bit), r5 = OSCalendarTime* out
    // Fill in a plausible UTC structure (OSCalendarTime: sec,min,hour,mday,mon,year,wday,yday,msec,usec)
    uint32_t out = ARG2;
    if (out) { require_guest_range(cpu,out,40);memset(MEM + out, 0, 40); }
    RET = 0;
}

// --- Core info ---
static void OSGetCoreId(CPUState* cpu)    { RET = rbrew_current_core(); }
static void OSGetMainCoreId(CPUState* cpu){ RET = 1; }

// --- Logging ---
// Diagnostic only: the format string and its first two raw arguments.
static void OSReport(CPUState* cpu) {
    if (!ARG0 || !rbrew_memory_accessible(cpu->mem, ARG0, 1)) return;
    const char* fmt = (const char*)(MEM + ARG0);
    size_t length = strnlen(fmt, size_t(std::min<uint64_t>(256, cpu->guest_memory_size - ARG0)));
    while (length && fmt[length - 1] == '\n') --length;
    RBREW_LOGF(stderr, "[OSReport] %.*s [r4=%08X r5=%08X]\n", int(length), fmt, ARG1, ARG2);
}
static void OSPanic(CPUState* cpu)  {
    // OSPanic(const char* file, int line, const char* fmt, ...)
    // r3=file, r4=line, r5=fmt, r6..=varargs
    const char* file = ARG0 ? (const char*)(MEM + ARG0) : "?";
    int         line = (int)ARG1;
    const char* fmt  = ARG2 ? (const char*)(MEM + ARG2) : "";
    // Print with up to 3 integer varargs (r6/r7/r8)
    RBREW_LOGF(stderr, "[OSPanic] %s:%d  ", file, line);
    RBREW_LOGF(stderr, fmt, cpu->r[6], cpu->r[7], cpu->r[8]);
    RBREW_LOGF(stderr, "\n");
    // Read-only caller attribution. EABI frames hold the previous SP at +0 and
    // the callee-saved LR in the previous frame at +4. Stop at any unreadable,
    // misaligned or non-increasing link; never write guest state.
    RBREW_LOGF(stderr, "[OSPanic]   LR=%08X SP=%08X chain:", cpu->lr, cpu->r[1]);
    uint32_t sp = cpu->r[1];
    for (int depth = 0; depth < 16; ++depth) {
        if ((sp & 3u) || !rbrew_memory_accessible(cpu->mem, sp, 4)) break;
        const uint32_t prev = rbrew_read32(cpu->mem, sp);
        if (!prev || (prev & 3u) || prev <= sp || !rbrew_memory_accessible(cpu->mem, prev, 8)) break;
        RBREW_LOGF(stderr, " %08X", rbrew_read32(cpu->mem, prev + 4));
        sp = prev;
    }
    RBREW_LOGF(stderr, "\n");
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
    rbrew_dynload_acquire(cpu);
}
static void OSDynLoad_FindExport(CPUState* cpu) {
    rbrew_dynload_find_export(cpu);
}

// --- Cache (no-op on cache-coherent host) ---
static void DCFlushRange(CPUState* cpu)      { (void)cpu; }
static void DCFlushRangeNoSync(CPUState* cpu){ (void)cpu; }
static void DCInvalidateRange(CPUState* cpu) { (void)cpu; }
static void DCStoreRange(CPUState* cpu)      { (void)cpu; }
static void DCZeroRange(CPUState* cpu) {
    uint32_t addr = ARG0, len = ARG1;
    if (addr && len) { require_guest_range(cpu,addr,len);memset(MEM + addr, 0, len); }
}
static void OSMemoryBarrier(CPUState* cpu) { (void)cpu; }

// --- Memory block ops ---
static void OSBlockMove(CPUState* cpu) {
    uint32_t dst = ARG0, src = ARG1, len = ARG2;
    if (dst && src && len) {
        require_guest_range(cpu,dst,len);require_guest_range(cpu,src,len);
        memmove(MEM + dst, MEM + src, len);
    }
}
static void OSBlockSet(CPUState* cpu) {
    uint32_t dst = ARG0, val = ARG1, len = ARG2;
    if (dst && len) { require_guest_range(cpu,dst,len);memset(MEM + dst, (int)val, len); }
}

// --- Host memcpy/memmove/memset passed through ---
static void memcpy_fn(CPUState* cpu) {
    uint32_t dst = ARG0, src = ARG1, n = ARG2;
    if (dst && src && n && rbrew_memory_accessible(MEM,dst,n) && rbrew_memory_accessible(MEM,src,n))
        memcpy(MEM + dst, MEM + src, n);
    RET = dst;
}
static void memmove_fn(CPUState* cpu) {
    uint32_t dst = ARG0, src = ARG1, n = ARG2;
    if (dst && src && n && rbrew_memory_accessible(MEM,dst,n) && rbrew_memory_accessible(MEM,src,n))
        memmove(MEM + dst, MEM + src, n);
    RET = dst;
}
static void memset_fn(CPUState* cpu) {
    uint32_t dst = ARG0, val = ARG1, n = ARG2;
    if (dst && n) {
        // Retain the OOB guard for unmapped holes and a disabled overlay too.
        if (rbrew_memory_accessible(MEM,dst,n))
            memset(MEM + dst, (int)val, n);
        else
            RBREW_LOGF(stderr, "[coreinit] memset OOB: dst=0x%08X n=0x%X\n", dst, n);
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
// Layout per wut coreinit/messagequeue.h and decaf-emu (0x3C bytes):
//   +0x00 tag 'mSgQ'  +0x04 name  +0x0C sendQueue  +0x1C recvQueue
//   +0x2C OSMessage* messages  +0x30 size  +0x34 first  +0x38 used
// OSMessage is 16 bytes {message, args[3]}; send/receive copy whole
// entries. Flags: 1 = blocking, 2 = high priority (pushed to the front).
// Blocking uses host condition variables; the guest-visible fields are
// mirrored after every change.
// ---------------------------------------------------------------------------
constexpr uint32_t kMessageQueueTag = 0x6D536751u;
constexpr uint32_t kMessageSize = 0x10;
constexpr uint32_t kMessageFlagBlocking = 1, kMessageFlagHighPriority = 2;
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
// Caller holds mq->mtx.
static void mirror_mq(CPUState* cpu, uint32_t q_addr, const MsgQueueImpl* mq) {
    rbrew_write32(MEM, q_addr + 0x34, mq->head);
    rbrew_write32(MEM, q_addr + 0x38, mq->count);
}
static void copy_message(CPUState* cpu, uint32_t to, uint32_t from) {
    require_guest_range(cpu, to, kMessageSize); require_guest_range(cpu, from, kMessageSize);
    for (uint32_t i = 0; i < kMessageSize; i += 4) rbrew_write32(MEM, to + i, rbrew_read32(MEM, from + i));
}

// OSInitMessageQueue(queue, messages, size)
static void OSInitMessageQueue(CPUState* cpu) {
    const uint32_t q_addr = ARG0, buf_ptr = ARG1, cap = ARG2;
    require_guest_range(cpu, q_addr, 0x3C);
    if (!cap || !buf_ptr) throw std::runtime_error("OSInitMessageQueue requires a message array");
    require_guest_range(cpu, buf_ptr, uint64_t(cap) * kMessageSize);
    MsgQueueImpl* mq = get_mq(q_addr);
    std::lock_guard<std::mutex> lk(mq->mtx);
    mq->buf_addr = buf_ptr;
    mq->capacity = cap;
    mq->head     = 0;
    mq->count    = 0;
    memset(MEM + q_addr, 0, 0x3C);
    rbrew_write32(MEM, q_addr, kMessageQueueTag);
    rbrew_write32(MEM, q_addr + 0x2C, buf_ptr);
    rbrew_write32(MEM, q_addr + 0x30, cap);
    mirror_mq(cpu, q_addr, mq);
}

// OSSendMessage(queue, const OSMessage* message, flags) -> BOOL
static void OSSendMessage(CPUState* cpu) {
    const uint32_t q_addr = ARG0, message = ARG1, flags = ARG2;
    MsgQueueImpl* mq = get_mq(q_addr);
    std::unique_lock<std::mutex> lk(mq->mtx);
    if (!mq->capacity) throw std::runtime_error("OSSendMessage on an uninitialized queue");
    if (flags & kMessageFlagBlocking) mq->not_full.wait(lk, [mq]{ return mq->count < mq->capacity; });
    else if (mq->count >= mq->capacity) { RET = 0; return; }
    uint32_t index;
    if (flags & kMessageFlagHighPriority) {
        mq->head = mq->head ? mq->head - 1 : mq->capacity - 1;
        index = mq->head;
    } else {
        index = (mq->head + mq->count) % mq->capacity;
    }
    copy_message(cpu, mq->buf_addr + index * kMessageSize, message);
    mq->count++;
    mirror_mq(cpu, q_addr, mq);
    lk.unlock();
    mq->not_empty.notify_all();
    RET = 1;
}

// OSReceiveMessage(queue, OSMessage* message, flags) -> BOOL
static void OSReceiveMessage(CPUState* cpu) {
    const uint32_t q_addr = ARG0, out_ptr = ARG1, flags = ARG2;
    MsgQueueImpl* mq = get_mq(q_addr);
    std::unique_lock<std::mutex> lk(mq->mtx);
    if (flags & kMessageFlagBlocking) mq->not_empty.wait(lk, [mq]{ return mq->count > 0; });
    else if (mq->count == 0) { RET = 0; return; } // message left untouched
    copy_message(cpu, out_ptr, mq->buf_addr + mq->head * kMessageSize);
    mq->head = (mq->head + 1) % mq->capacity;
    mq->count--;
    mirror_mq(cpu, q_addr, mq);
    lk.unlock();
    mq->not_full.notify_all();
    RET = 1;
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
static void MCP_Open(CPUState* cpu)  { rbrew_mcp_open(cpu); }
static void MCP_Close(CPUState* cpu) { rbrew_mcp_close(cpu); }
static void MCP_GetSysProdSettings(CPUState* cpu) { rbrew_mcp_get_sys_prod_settings(cpu); }

// --- UC (user config) ---
static void UCOpen(CPUState* cpu)  { rbrew_uc_open(cpu); }
static void UCClose(CPUState* cpu) { rbrew_uc_close(cpu); }
static void UCReadSysConfig(CPUState* cpu) { rbrew_uc_read_sys_config(cpu); }

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
    REG(OSGetSystemInfo);
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
