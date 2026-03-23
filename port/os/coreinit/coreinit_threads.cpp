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
//   GL safety: the main thread creates the SDL GL context.  Game rendering
//   typically runs on the same thread that calls GX2Init.  If a worker thread
//   calls GX2, it will fail silently (no MakeCurrent here).

#include "../os_common.h"
#include <thread>
#include <mutex>
#include <atomic>
#include <unordered_map>
#include <chrono>
#include <cstdio>
#include <cstring>

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

    GuestThread() { memset(&cpu, 0, sizeof(cpu)); }
    GuestThread(const GuestThread&) = delete;
    GuestThread& operator=(const GuestThread&) = delete;
};

static std::mutex                              s_threads_mutex;
static std::unordered_map<uint32_t, GuestThread*> s_threads; // osthread_addr → GuestThread*

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

    fprintf(stderr, "[coreinit] OSCreateThread(thread=0x%08X entry=0x%08X)\n",
            thread_addr, entry_addr);

    if (!thread_addr || !entry_addr) { RET = 0; return; }

    auto* gt = new GuestThread();
    gt->osthread_addr = thread_addr;
    gt->entry_addr    = entry_addr;

    // Share mem and dispatch table with the caller
    gt->cpu.mem            = cpu->mem;
    gt->cpu.dispatch_table = cpu->dispatch_table;
    gt->cpu.dispatch_size  = cpu->dispatch_size;

    // Initial register state
    gt->cpu.r[1] = stack_top ? (stack_top - 8u) : 0x1FBFFFE8u; // sp
    gt->cpu.r[3] = int_arg;
    gt->cpu.r[4] = ptr_arg;

    // Write READY state into guest OSThread struct (+0x10)
    if ((uint64_t)thread_addr + 0x20u < WIIU_MEM_SIZE)
        rbrew_write32(cpu->mem, thread_addr + 0x10u, 1u); // OS_THREAD_STATE_READY

    std::lock_guard<std::mutex> lk(s_threads_mutex);
    auto it = s_threads.find(thread_addr);
    if (it != s_threads.end()) {
        if (it->second->host_thread.joinable())
            it->second->host_thread.detach();
        delete it->second;
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
    if ((uint64_t)thread_addr + 0x20u < WIIU_MEM_SIZE)
        rbrew_write32(gt->cpu.mem, thread_addr + 0x10u, 2u); // OS_THREAD_STATE_RUNNING

    gt->host_thread = std::thread([gt]() {
        t_osthread_addr   = gt->osthread_addr;
        t_in_guest_thread = true;
        memset(t_thread_specific, 0, sizeof(t_thread_specific));

        fprintf(stderr, "[coreinit] thread 0x%08X started (entry=0x%08X)\n",
                gt->osthread_addr, gt->entry_addr);
        try {
            rbrew_dispatch(&gt->cpu, gt->entry_addr);
        } catch (const GuestThreadExit&) {
            // OSExitThread was called — clean exit
        }
        fprintf(stderr, "[coreinit] thread 0x%08X finished\n", gt->osthread_addr);

        // Mark MORIBUND in guest struct
        if ((uint64_t)gt->osthread_addr + 0x20u < WIIU_MEM_SIZE)
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
    RET = t_osthread_addr ? t_osthread_addr : MAIN_THREAD_ADDR;
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
static void OSSetThreadAffinity(CPUState* cpu) { RET = 1; }
static void OSSetThreadPriority(CPUState* cpu) { RET = 1; }
static void OSGetThreadPriority(CPUState* cpu) { RET = 16; }
static void OSInitThreadQueue(CPUState* cpu)   {
    if (ARG0 && (uint64_t)ARG0 + 8u < WIIU_MEM_SIZE)
        memset(MEM + ARG0, 0, 8);
}

static void OSSleepTicks(CPUState* cpu) {
    uint64_t ticks = ((uint64_t)ARG0 << 32) | (uint64_t)ARG1;
    uint64_t us = ticks / 2000ULL;
    if (us > 0 && us < 500000ULL)
        std::this_thread::sleep_for(std::chrono::microseconds(us));
}
static void OSYieldThread(CPUState* cpu)         { (void)cpu; std::this_thread::yield(); }
static void OSWaitRendezvous(CPUState* cpu)      { (void)cpu; }
static void OSInitRendezvous(CPUState* cpu)      { if (ARG0) memset(MEM+ARG0, 0, 0x20); }
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
