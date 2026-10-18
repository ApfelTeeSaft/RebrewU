#include "../../runtime/rbrew_diagnostics.h"
// coreinit thread management
//
// Design:
//   Each OSCreateThread call allocates a GuestThread on the heap.  The struct
//   holds an independent CPUState (with shared mem / dispatch_table pointers)
//   plus a std::thread that is started by OSResumeThread.
//
//   Thread-local state:
//     t_osthread_addr   — guest OSThread* for the calling host thread
//     t_thread_specific — 16-slot per-thread storage (OSGetThreadSpecific)
//     t_in_guest_thread — true only for worker threads (enables OSExitThread throw)
//
//   OSExitThread is implemented by throwing GuestThreadExit{}; the thread
//   lambda catches this and lets the thread function cleanly return.
//
//   GPU safety: the main thread creates the SDL GPU context.  Game rendering
//   typically runs on the same thread that calls GX2Init.  If a worker thread
//   calls GX2, it will fail silently (no MakeCurrent here).

#include "../os_common.h"
#include "../../runtime/host_sleep.h"
#include <thread>
#include <mutex>
#include <atomic>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <condition_variable>
#include <memory>
#include <stdexcept>
#if defined(_WIN32)
extern "C" __declspec(dllimport) unsigned long __stdcall GetCurrentThreadId(void);
static unsigned long host_thread_id() { return GetCurrentThreadId(); }
#else
static unsigned long host_thread_id() { return 0; }
#endif

// ---------------------------------------------------------------------------
// Thread-local identity
// ---------------------------------------------------------------------------
static constexpr uint32_t MAIN_THREAD_ADDR = 0x50000010u;

static thread_local uint32_t t_osthread_addr    = 0;    // 0 → main thread
static thread_local uint32_t t_thread_specific[16] = {};
static thread_local bool     t_in_guest_thread  = false;

// Exception used to cleanly exit from a guest thread (OSExitThread).
struct GuestThreadExit {};

// ---------------------------------------------------------------------------
// GuestThread — one per OSCreateThread call
// ---------------------------------------------------------------------------
struct GuestThread {
    uint32_t    osthread_addr = 0;
    uint32_t    entry_addr    = 0;
    CPUState    cpu;              // own register file; shares mem/dispatch
    std::thread host_thread;
    std::atomic<bool> started{false};
    std::atomic<uint32_t> affinity{2}; // OSThread attr bits 0-2 (core 0/1/2)
    std::atomic<uint32_t> core{1};     // core this thread runs on (OSGetCoreId)

    GuestThread() { memset(&cpu, 0, sizeof(cpu)); }
    GuestThread(const GuestThread&) = delete;
    GuestThread& operator=(const GuestThread&) = delete;
};

static std::mutex                              s_threads_mutex;
static std::unordered_map<uint32_t, GuestThread*> s_threads; // osthread_addr → GuestThread*
static std::atomic<uint32_t> s_main_affinity{2}; // main thread: core 1
static std::vector<GuestThread*> s_retired_threads; // replaced, never freed (see OSCreateThread)

// Pick the core for an affinity set: stay on the preferred core when the set
// allows it, otherwise use the lowest allowed core; an empty set keeps it.
static uint32_t pick_core(uint32_t affinity, uint32_t preferred) {
    affinity &= 7u;
    if (!affinity || (affinity & (1u << preferred))) return preferred;
    return affinity & 1u ? 0u : affinity & 2u ? 1u : 2u;
}
static thread_local GuestThread* t_guest_thread = nullptr;
static uint32_t current_affinity() {
    return t_guest_thread ? t_guest_thread->affinity.load() : s_main_affinity.load();
}

// ---------------------------------------------------------------------------
// OSCreateThread
// r3=OSThread*, r4=entry, r5=intArg(→r3), r6=ptrArg(→r4),
// r7=stackTop, r8=stackSize, r9=priority, r10=attr
// ---------------------------------------------------------------------------
static void OSCreateThread(CPUState* cpu) {
    uint32_t thread_addr = ARG0;
    uint32_t entry_addr  = ARG1;
    uint32_t int_arg     = ARG2;
    uint32_t ptr_arg     = ARG3;
    uint32_t stack_top   = ARG4; // r7 = high end of stack

    RBREW_LOGF(stderr, "[coreinit] OSCreateThread(thread=0x%08X entry=0x%08X)\n",
            thread_addr, entry_addr);

    if (!thread_addr || !entry_addr) { RET = 0; return; }

    auto* gt = new GuestThread();
    gt->osthread_addr = thread_addr;
    gt->entry_addr    = entry_addr;

    // Share mem and dispatch table with the caller
    gt->cpu.mem            = cpu->mem;
    gt->cpu.dispatch_table = cpu->dispatch_table;
    gt->cpu.dispatch_size  = cpu->dispatch_size;
    gt->cpu.guest_memory_size = cpu->guest_memory_size;
#ifdef RBREW_ENABLE_PPC_FALLBACK
    gt->cpu.guest_code_ranges = cpu->guest_code_ranges;
    gt->cpu.guest_code_range_count = cpu->guest_code_range_count;
    gt->cpu.ppc_fallback_enabled = cpu->ppc_fallback_enabled;
    gt->cpu.ppc_instruction_budget = cpu->ppc_instruction_budget;
#endif

    // Initial register state
    gt->cpu.r[1] = stack_top ? (stack_top - 8u) : 0x1FBFFFE8u; // sp
    // Cafe OSInitContext supplies the application's SDA2/SDA bases. This
    // native process runs one generated guest module, so the creator carries
    // those module bases. Leaving them zero breaks SDA-relative guest loads.
    gt->cpu.r[2] = cpu->r[2];
    gt->cpu.r[13] = cpu->r[13];
    gt->cpu.r[3] = int_arg;
    gt->cpu.r[4] = ptr_arg;
    // Affinity: attribute bits 0-2; none given inherits the creator's (decaf).
    uint32_t affinity = ARG7 & 7u;
    if (!affinity) affinity = current_affinity();
    gt->affinity = affinity;
    gt->core = pick_core(affinity, rbrew_current_core());
    rbrew_init_thread_gqrs(&gt->cpu); // decaf OSCreateThread GQR2-5

    // Write READY state into guest OSThread struct (+0x10)
    if (rbrew_memory_accessible(cpu->mem,thread_addr,0x20u))
        rbrew_write32(cpu->mem, thread_addr + 0x10u, 1u); // OS_THREAD_STATE_READY

    std::lock_guard<std::mutex> lk(s_threads_mutex);
    auto it = s_threads.find(thread_addr);
    if (it != s_threads.end()) {
        // Recreating an OSThread (Gambit 0x329F03B0, entry 0284E5F4): the old
        // host thread may still be finishing (it writes the guest state and
        // reads its GuestThread after "finished"), and OSJoinThread may hold
        // it. Retire it instead of freeing it under them.
        GuestThread* old = it->second;
        if (old->host_thread.joinable() && old->host_thread.get_id() != std::this_thread::get_id())
            old->host_thread.detach();
        s_retired_threads.push_back(old);
    }
    s_threads[thread_addr] = gt;
    RET = 1; // TRUE
}

// ---------------------------------------------------------------------------
// OSResumeThread — kick off the thread
// ---------------------------------------------------------------------------
static void OSResumeThread(CPUState* cpu) {
    uint32_t thread_addr = ARG0;

    std::lock_guard<std::mutex> lk(s_threads_mutex);
    auto it = s_threads.find(thread_addr);
    if (it == s_threads.end()) { RET = -1; return; }

    GuestThread* gt = it->second;
    if (gt->started.exchange(true)) { RET = 0; return; } // already running

    // Mark RUNNING in guest struct
    if (rbrew_memory_accessible(gt->cpu.mem,thread_addr,0x20u))
        rbrew_write32(gt->cpu.mem, thread_addr + 0x10u, 2u); // OS_THREAD_STATE_RUNNING

    gt->host_thread = std::thread([gt]() {
        t_osthread_addr   = gt->osthread_addr;
        t_in_guest_thread = true;
        t_guest_thread    = gt;
        rbrew_thread_core_slot = &gt->core;
        memset(t_thread_specific, 0, sizeof(t_thread_specific));

        RBREW_LOGF(stderr, "[coreinit] thread 0x%08X started (entry=0x%08X host_tid=%lu cpu=%p)\n",
                gt->osthread_addr, gt->entry_addr, host_thread_id(), (void*)&gt->cpu);
        try {
            rbrew_dispatch(&gt->cpu, gt->entry_addr);
        } catch (const GuestThreadExit&) {
            // OSExitThread was called — clean exit
        }
        RBREW_LOGF(stderr, "[coreinit] thread 0x%08X finished\n", gt->osthread_addr);

        // Mark MORIBUND in guest struct
        if (rbrew_memory_accessible(gt->cpu.mem,gt->osthread_addr,0x20u))
            rbrew_write32(gt->cpu.mem, gt->osthread_addr + 0x10u, 8u);
    });

    RET = 1; // previous suspend count
}

// ---------------------------------------------------------------------------
// OSJoinThread — block until the thread finishes
// r3=OSThread*, r4=optional exit value out ptr
// ---------------------------------------------------------------------------
static void OSJoinThread(CPUState* cpu) {
    uint32_t thread_addr = ARG0;

    GuestThread* gt = nullptr;
    {
        std::lock_guard<std::mutex> lk(s_threads_mutex);
        auto it = s_threads.find(thread_addr);
        if (it != s_threads.end()) gt = it->second;
    }
    if (gt && gt->host_thread.joinable())
        gt->host_thread.join();

    // Write exit value 0 to output ptr if given
    if (ARG1) rbrew_write32(MEM, ARG1, 0u);
    RET = 1;
}

// ---------------------------------------------------------------------------
// OSExitThread — gracefully terminate the current thread
// Implemented as a C++ throw so the stack unwinds cleanly back to the
// thread lambda in OSResumeThread, which catches GuestThreadExit.
// For the main thread (t_in_guest_thread == false) this is a no-op.
// ---------------------------------------------------------------------------
void coreinit_exit_thread(CPUState*) {
    if (t_in_guest_thread) throw GuestThreadExit{};
}

// ---------------------------------------------------------------------------
// OSGetCurrentThread
// ---------------------------------------------------------------------------
static void OSGetCurrentThread(CPUState* cpu) {
    RET = rbrew_system_thread ? rbrew_system_thread : (t_osthread_addr ? t_osthread_addr : MAIN_THREAD_ADDR);
}

// ---------------------------------------------------------------------------
// Thread-specific storage (16 slots; thread_local)
// ---------------------------------------------------------------------------
static void OSGetThreadSpecific(CPUState* cpu) {
    uint32_t slot = ARG0;
    RET = (slot < 16) ? t_thread_specific[slot] : 0u;
}
static void OSSetThreadSpecific(CPUState* cpu) {
    uint32_t slot = ARG0;
    if (slot < 16) t_thread_specific[slot] = ARG1;
}

// ---------------------------------------------------------------------------
// Misc thread operations
// ---------------------------------------------------------------------------
static void OSSetThreadName(CPUState* cpu)     { (void)cpu; }
// OSSetThreadAffinity(thread, affinity): the thread moves to an allowed core
// (staying put when its current core is still allowed). Unknown threads are
// the main thread (its OSThread is not tracked here).
static void OSSetThreadAffinity(CPUState* cpu) {
    const uint32_t thread_addr = ARG0, affinity = ARG1 & 7u;
    {
        std::lock_guard<std::mutex> lk(s_threads_mutex);
        auto it = s_threads.find(thread_addr);
        if (it != s_threads.end()) {
            it->second->affinity = affinity;
            it->second->core = pick_core(affinity, it->second->core.load());
            RET = 1;
            return;
        }
    }
    s_main_affinity = affinity;
    rbrew_main_thread_core = pick_core(affinity, rbrew_main_thread_core.load());
    RET = 1;
}
static void OSSetThreadPriority(CPUState* cpu) { RET = 1; }
static void OSGetThreadPriority(CPUState* cpu) { RET = 16; }
static void OSInitThreadQueue(CPUState* cpu)   {
    if (ARG0 && rbrew_memory_accessible(MEM,ARG0,8u))
        memset(MEM + ARG0, 0, 8);
}

static void OSSleepTicks(CPUState* cpu) {
    // Ticks of the OS timer (bus clock / 4); the full duration is slept.
    uint64_t ticks = ((uint64_t)ARG0 << 32) | (uint64_t)ARG1;
    uint64_t us = ticks / RBREW_TIMER_CLOCK * 1000000ULL + ticks % RBREW_TIMER_CLOCK * 1000000ULL / RBREW_TIMER_CLOCK;
    if (us > 0)
        rbrew_host_sleep_for(us);
}
static void OSYieldThread(CPUState* cpu)         { (void)cpu; std::this_thread::yield(); }
// coreinit.rpl 0201FC1C / 0201FC38: three big-endian arrival words and a
// self pointer, exactly 0x10 bytes. Arrival stays published until reinit.
// The native condition variable supplies the original release/acquire barrier
// without consuming a host core while another guest core initializes a library.
struct RendezvousImpl {
    std::mutex mutex;
    std::condition_variable arrived;
};
static std::mutex s_rendezvous_map_mutex;
static std::unordered_map<const uint8_t*,std::unique_ptr<RendezvousImpl>> s_rendezvous;
static RendezvousImpl& get_rendezvous(CPUState* cpu,uint32_t address) {
    if(!address || uint64_t(address)+0x10>cpu->guest_memory_size ||
       !rbrew_memory_accessible(MEM,address,0x10))
        throw std::runtime_error("Cafe rendezvous lies outside accessible guest memory");
    std::lock_guard lock(s_rendezvous_map_mutex);
    auto& item=s_rendezvous[MEM+address];
    if(!item) item=std::make_unique<RendezvousImpl>();
    return *item;
}
static void OSInitRendezvous(CPUState* cpu) {
    const uint32_t address=ARG0;
    auto& rendezvous=get_rendezvous(cpu,address);
    {
        std::lock_guard lock(rendezvous.mutex);
        for(unsigned core=0;core<3;++core) rbrew_write32(MEM,address+core*4,0);
        rbrew_write32(MEM,address+12,address);
    }
    rendezvous.arrived.notify_all();
}
static void OSWaitRendezvous(CPUState* cpu) {
    const uint32_t address=ARG0,mask=ARG1&7,core=rbrew_current_core();
    if(core>=3) throw std::runtime_error("Cafe rendezvous has an invalid caller core");
    auto& rendezvous=get_rendezvous(cpu,address);
    std::unique_lock lock(rendezvous.mutex);
    rbrew_write32(MEM,address+core*4,1);
    rendezvous.arrived.notify_all();
    rendezvous.arrived.wait(lock,[&] {
        for(unsigned participant=0;participant<3;++participant)
            if((mask&(1u<<participant)) && rbrew_read32(MEM,address+participant*4)!=1) return false;
        return true;
    });
    RET=1;
}
static void OSWaitEventWithTimeout(CPUState* cpu){ (void)cpu; RET = 0; }
static void OSDriver_Register(CPUState* cpu)     { (void)cpu; RET = 0; }

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
#include "coreinit_addrs.h"
void coreinit_threads_register(CPUState* cpu) {
#define REG(name) rbrew_register_func(cpu, ADDR_##name, name)
    REG(OSCreateThread);
    REG(OSJoinThread);
    REG(OSResumeThread);
    REG(OSYieldThread);
    REG(OSGetCurrentThread);
    REG(OSSetThreadName);
    REG(OSSetThreadAffinity);
    REG(OSSetThreadPriority);
    REG(OSGetThreadPriority);
    REG(OSGetThreadSpecific);
    REG(OSSetThreadSpecific);
    REG(OSInitThreadQueue);
    REG(OSSleepTicks);
    REG(OSWaitRendezvous);
    REG(OSInitRendezvous);
    REG(OSWaitEventWithTimeout);
    REG(OSDriver_Register);
    // OSExitThread is registered in coreinit_misc.cpp via coreinit_exit_thread()
#undef REG
}
