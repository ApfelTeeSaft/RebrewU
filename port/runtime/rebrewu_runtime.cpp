#include "rbrew_diagnostics.h"
#include "rebrewu_runtime.h"
#include "rebrewu_memory_observer.h"
#include <stdio.h>
#include <stdlib.h>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
#include <vector>
#include <stdexcept>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#ifdef RBREW_ENABLE_PPC_FALLBACK
#include "rebrewu_ppc_hybrid.h"
#endif

namespace {
// 750-style aligned 32-byte reservation granules. Every guest store bumps a
// version counter for each granule it touches, then writes; lwarx records
// the counter and the reserved word; stwcx. claims the counter (unchanged
// since lwarx) and commits with a compare-and-swap of that word. No store or
// reservation operation takes a lock: a global mutex here serialized every
// guest store of every CPU thread. Counters live in a fixed table hashed by
// arena and granule; granules sharing a counter only make a stwcx. fail
// spuriously, which the architecture permits (reservation loops retry).
constexpr uint32_t reservation_granule = 32;
constexpr unsigned reservation_table_bits = 16;
std::atomic<uint32_t> reservation_table[1u << reservation_table_bits];
std::atomic<uint32_t>& reservation_counter(const uint8_t* mem, uint32_t addr) {
    const uint32_t arena = uint32_t(reinterpret_cast<uintptr_t>(mem) >> 12) * 0x85EBCA6Bu;
    return reservation_table[((addr / reservation_granule) ^ arena) * 0x9E3779B1u >> (32 - reservation_table_bits)];
}
bool compare_exchange_word(uint8_t* p, uint32_t expected, uint32_t desired) {
#if defined(_MSC_VER)
    return _InterlockedCompareExchange(reinterpret_cast<volatile long*>(p), long(desired), long(expected)) == long(expected);
#else
    return __atomic_compare_exchange_n(reinterpret_cast<uint32_t*>(p), &expected, desired, false,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
#endif
}
thread_local RbrewMemoryWriteObserver write_observer{};
struct OverlayAccessRegistry {
    std::mutex mutex;
    std::unordered_set<const uint8_t*> arenas;
};
OverlayAccessRegistry& overlay_access_registry() {
    // Guest workers may still execute while the launch thread returns. Keep
    // shared access metadata alive until process termination; owners remove
    // individual arenas explicitly when their workers are quiescent.
    static auto* registry = new OverlayAccessRegistry;
    return *registry;
}

void observe_write(uint8_t* mem, uint32_t address, uint32_t size) noexcept {
    if (write_observer.callback && write_observer.mem == mem && size)
        write_observer.callback(address, size, write_observer.user);
}

// Guest threads are host threads, so plain stores need no emulated
// reservation monitor: stwcx. still claims the granule counter against other
// lwarx/stwcx. pairs (atomic-only ABA fails) and commits by compare-and-swap
// of the reserved word, so any store that changed the word fails it. A plain
// store that restores the reserved value (plain-store ABA) lets it succeed,
// as in Cemu/decaf. Bumping a shared counter on every guest store cost a
// locked RMW per store plus cache-line traffic between game threads.
// RBREW_STRICT_RESERVATIONS=1 restores the 750-style monitor on every store.
std::atomic<bool> strict_reservations{false}; // constant-initialized
const bool strict_reservations_from_env = [] {
    const char* v = getenv("RBREW_STRICT_RESERVATIONS");
    if (v && *v && *v != '0') strict_reservations.store(true);
    return true;
}();

// Before the bytes change: a stwcx. that still sees the old counter then
// compares the reserved word against the store's result.
void invalidate_reservations(uint8_t* mem, uint32_t addr, uint32_t size) {
    if (!size) return;
    uint32_t first = addr & ~(reservation_granule - 1u);
    uint32_t last = (addr + size - 1u) & ~(reservation_granule - 1u);
    for (uint32_t line = first;; line += reservation_granule) {
        reservation_counter(mem, line).fetch_add(1);
        if (line == last) break;
    }
}
}

bool rbrew_overlay_accessible(const uint8_t* mem) {
    if (!mem) return false;
    auto& registry = overlay_access_registry();
    std::lock_guard lock(registry.mutex);
    return registry.arenas.contains(mem);
}

void rbrew_set_overlay_access(uint8_t* mem, bool enabled) {
    if (!mem) return;
    auto& registry = overlay_access_registry();
    std::lock_guard lock(registry.mutex);
    if (enabled) registry.arenas.insert(mem);
    else registry.arenas.erase(mem);
}

RbrewMemoryWriteObserver rbrew_set_write_observer(RbrewMemoryWriteObserver observer) noexcept {
    const auto previous = write_observer;
    write_observer = observer;
    return previous;
}

void rbrew_write_memory(uint8_t* mem, uint32_t addr, const void* bytes, uint32_t size) {
    if (!rbrew_memory_accessible(mem, addr, size)) return;
    observe_write(mem, addr, size);
    if (strict_reservations.load(std::memory_order_relaxed))
        invalidate_reservations(mem, addr, size);
    uint8_t* p = mem + addr;
    switch (size) { // fixed widths become single moves
    case 1: *p = *static_cast<const uint8_t*>(bytes); break;
    case 2: memcpy(p, bytes, 2); break;
    case 4: memcpy(p, bytes, 4); break;
    case 8: memcpy(p, bytes, 8); break;
    default: memcpy(p, bytes, size); break;
    }
}

void rbrew_set_strict_reservations(bool enabled) {
    strict_reservations.store(enabled);
}

// Diagnostic: RBREW_RESERVATION_TRACE=<hex guest address> logs every
// lwarx/stwcx. on that word (host thread, value, outcome, LR).
static uint32_t reservation_trace_addr() {
    static const uint32_t addr = [] {
        const char* v = rbrew_diagnostic_env("RBREW_RESERVATION_TRACE");
        return v && *v ? uint32_t(strtoul(v, nullptr, 16)) : 0u;
    }();
    return addr;
}

// Diagnostic: RBREW_HOT_LWARX=1 reports (once per address) a word polled
// by lwarx 5,000,000 times on one thread - a spinlock that never frees.
static bool hot_lwarx_enabled() {
    static const bool on = [] { const char* v = rbrew_diagnostic_env("RBREW_HOT_LWARX"); return v && *v && *v != '0'; }();
    return on;
}

uint32_t rbrew_lwarx(CPUState* cpu, uint32_t addr) {
    if (hot_lwarx_enabled()) {
        thread_local uint64_t total = 0;
        if (++total % 5000000 == 0 && total <= 15000000)
            RBREW_LOGF(stderr, "[resv] hot lwarx cpu=%p n=%llu addr=%08X value=%08X lr=%08X r3=%08X\n",
                    (void*)cpu, (unsigned long long)total, addr, rbrew_read32(cpu->mem, addr), cpu->lr, cpu->r[3]);
    }
    if (addr && addr == reservation_trace_addr())
        RBREW_LOGF(stderr, "[resv] lwarx cpu=%p value=%08X lr=%08X\n", (void*)cpu, rbrew_read32(cpu->mem, addr), cpu->lr);
    cpu->reservation_valid = false;
    if ((addr & 3u) || !rbrew_memory_accessible(cpu->mem, addr, 4)) {
        RBREW_LOGF(stderr, "[rbrew] invalid lwarx address=0x%08X\n", addr);
        return 0;
    }
    // Counter first: a store between the two reads changes either the
    // counter or the word the reservation commits against.
    const uint32_t version = reservation_counter(cpu->mem, addr).load();
    uint32_t raw;
    memcpy(&raw, cpu->mem + addr, 4);
    cpu->reservation_mem = cpu->mem;
    cpu->reservation_addr = addr;
    cpu->reservation_version = uint64_t(version) << 32 | raw;  // counter, reserved word (guest order)
    cpu->reservation_valid = true;
    return rbrew_bswap32(raw);
}

void rbrew_stwcx(CPUState* cpu, uint32_t addr, uint32_t value) {
    bool success = false;
    if (cpu->reservation_valid && cpu->reservation_mem == cpu->mem &&
        cpu->reservation_addr == addr && !(addr & 3u) &&
        rbrew_memory_accessible(cpu->mem, addr, 4)) {
        // Claim the granule (no store since lwarx), invalidating other
        // reservations on it, then commit only if the word is still the
        // reserved value (a store may have bumped the counter after the
        // claim but not yet written; its bytes then fail the exchange).
        uint32_t version = uint32_t(cpu->reservation_version >> 32);
        if (reservation_counter(cpu->mem, addr).compare_exchange_strong(version, version + 1)) {
            success = compare_exchange_word(cpu->mem + addr, uint32_t(cpu->reservation_version), rbrew_bswap32(value));
            if (success) observe_write(cpu->mem, addr, 4);
        }
    }
    cpu->reservation_valid = false; // every attempt consumes the reservation
    cpu->cr[0] = (success ? 2u : 0u) | ((cpu->xer >> 31) & 1u);
    if (addr && addr == reservation_trace_addr())
        RBREW_LOGF(stderr, "[resv] stwcx cpu=%p value=%08X success=%d lr=%08X\n", (void*)cpu, value, success, cpu->lr);
}

// Dispatch table is a flat array: dispatch_table[addr >> 2] = HostFunc
// Max guest address we care about is ~0x0D000000 (320MB covers all stubs)
#define DISPATCH_MAX_ADDR  0x0D000000u
#define DISPATCH_ENTRIES   (DISPATCH_MAX_ADDR >> 2)

static uint32_t read_u32_be(const uint8_t* mem, uint32_t addr) {
    return ((uint32_t)mem[addr] << 24) | ((uint32_t)mem[addr+1] << 16) |
           ((uint32_t)mem[addr+2] <<  8) | (uint32_t)mem[addr+3];
}

uint32_t rbrew_run_constructors(CPUState* cpu, uint32_t table, uint32_t done_flag) {
    if (rbrew_read32(cpu->mem, done_flag)) return 0;
    rbrew_write32(cpu->mem, done_flag, 1u);
    uint32_t count = 0;
    for (;;) {
        uint32_t fn = rbrew_read32(cpu->mem, table + count * 4u);
        if (!fn) return count;
        if (rbrew_diagnostic_env("RBREW_CTOR_TRACE"))
            RBREW_LOGF(stderr, "[PPC] ctor[%u]=0x%08X\n", count, fn);
        rbrew_dispatch(cpu, fn);
        ++count;
    }
}

int32_t rbrew_run_entry(CPUState* cpu, uint32_t entry, uint32_t argc, uint32_t argv) {
    cpu->r[3] = argc;
    cpu->r[4] = argv;
    rbrew_dispatch(cpu, entry);
    return static_cast<int32_t>(cpu->r[3]);
}

// Opt-in import profiler (RBREW_IMPORT_PROFILE=1): per-PLT-entry call counts
// and last caller LR, printed as 5-second deltas by a detached host thread.
// Read-only diagnostics; guest state is never touched.
namespace {
constexpr uint32_t plt_profile_begin = 0x02D00000u, plt_profile_end = 0x02D10000u;
constexpr uint32_t plt_profile_slots = (plt_profile_end - plt_profile_begin) >> 2;
struct ProfiledThread {
    const CPUState* cpu{};
    std::atomic<uint64_t> dispatches{};
    std::atomic<uint32_t> inside{}, inside_lr{}, inside_r3{};
};
struct ImportProfile {
    std::atomic<uint64_t> calls[plt_profile_slots]{};
    std::atomic<uint32_t> last_lr[plt_profile_slots]{};
    std::mutex threads_mutex;
    std::vector<ProfiledThread*> threads;
};
ImportProfile* import_profile_start() {
    static ImportProfile* profile = [] () -> ImportProfile* {
        const char* enabled = rbrew_diagnostic_env("RBREW_IMPORT_PROFILE");
        if (!enabled || !*enabled || *enabled == '0') return nullptr;
        auto* p = new ImportProfile;
        std::thread([p] {
            std::vector<uint64_t> previous(plt_profile_slots);
            std::unordered_map<ProfiledThread*, uint64_t> thread_previous;
            for (unsigned tick = 1;; ++tick) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                std::vector<std::pair<uint64_t, uint32_t>> delta;
                for (uint32_t i = 0; i < plt_profile_slots; ++i) {
                    const uint64_t now = p->calls[i].load(std::memory_order_relaxed);
                    if (now != previous[i]) delta.emplace_back(now - previous[i], i);
                    previous[i] = now;
                }
                std::sort(delta.begin(), delta.end(), std::greater<>());
                RBREW_LOGF(stderr, "[profile] t=%us imports=%zu\n", tick * 5, delta.size());
                for (size_t k = 0; k < delta.size() && k < 24; ++k)
                    RBREW_LOGF(stderr, "[profile]   %08X calls=%llu last_lr=%08X\n",
                        plt_profile_begin + (delta[k].second << 2),
                        static_cast<unsigned long long>(delta[k].first),
                        p->last_lr[delta[k].second].load(std::memory_order_relaxed));
                std::lock_guard lock(p->threads_mutex);
                for (auto* t : p->threads) {
                    const uint64_t now = t->dispatches.load(std::memory_order_relaxed);
                    RBREW_LOGF(stderr, "[profile]   thread cpu=%p dispatch_delta=%llu inside=%08X lr=%08X r3=%08X\n",
                        (const void*)t->cpu, static_cast<unsigned long long>(now - thread_previous[t]),
                        t->inside.load(), t->inside_lr.load(), t->inside_r3.load());
                    thread_previous[t] = now;
                }
            }
        }).detach();
        return p;
    }();
    return profile;
}
ProfiledThread* profiled_thread(ImportProfile* profile, const CPUState* cpu) {
    thread_local ProfiledThread* current = nullptr;
    if (!current || current->cpu != cpu) {
        current = new ProfiledThread;
        current->cpu = cpu;
        std::lock_guard lock(profile->threads_mutex);
        profile->threads.push_back(current);
    }
    return current;
}
struct InsideImport {
    ProfiledThread* thread{};
    ~InsideImport() { if (thread) thread->inside.store(0); }
};
}

// Opt-in entry trace (RBREW_ENTRY_TRACE=<file of hex addresses>): logs the
// first dispatch to each listed guest address. Used with diagnostic forced-hint
// regenerations to learn which undiscovered targets startup really reaches.
namespace {
struct EntryTrace { std::unordered_set<uint32_t> watched, seen; std::mutex mutex; };
EntryTrace* entry_trace() {
    static EntryTrace* trace = []() -> EntryTrace* {
        const char* path = rbrew_diagnostic_env("RBREW_ENTRY_TRACE");
        if (!path || !*path) return nullptr;
        FILE* file = fopen(path, "r");
        if (!file) return nullptr;
        auto* t = new EntryTrace;
        char line[64];
        while (fgets(line, sizeof line, file))
            if (line[0] != '#' && line[0] != '\n') t->watched.insert(uint32_t(strtoul(line, nullptr, 16)));
        fclose(file);
        return t;
    }();
    return trace;
}
}

void rbrew_dispatch(CPUState* cpu, uint32_t guest_addr) {
    if (cpu->differential_validation)
        throw std::runtime_error("dispatch/HLE is forbidden during detached differential replay");
    if (EntryTrace* trace = entry_trace(); trace && trace->watched.contains(guest_addr)) {
        std::lock_guard lock(trace->mutex);
        if (trace->seen.insert(guest_addr).second)
            RBREW_LOGF(stderr, "[entry] reached %08X lr=%08X r3=%08X r4=%08X\n", guest_addr, cpu->lr, cpu->r[3], cpu->r[4]);
    }
    InsideImport inside_import;
    if (ImportProfile* profile = import_profile_start()) {
        ProfiledThread* thread = profiled_thread(profile, cpu);
        thread->dispatches.fetch_add(1, std::memory_order_relaxed);
        if (guest_addr >= plt_profile_begin && guest_addr < plt_profile_end) {
            const uint32_t slot = (guest_addr - plt_profile_begin) >> 2;
            profile->calls[slot].fetch_add(1, std::memory_order_relaxed);
            profile->last_lr[slot].store(cpu->lr, std::memory_order_relaxed);
            thread->inside_lr.store(cpu->lr); thread->inside_r3.store(cpu->r[3]);
            thread->inside.store(guest_addr);
            inside_import.thread = thread;
        }
    }
    uint32_t idx = guest_addr >> 2;
    if (idx < cpu->dispatch_size && cpu->dispatch_table[idx]) {
        RBREW_COVERAGE_EVENT('D',cpu->lr,guest_addr);
        cpu->dispatch_table[idx](cpu);
    } else {
#ifdef RBREW_ENABLE_PPC_FALLBACK
        // Diagnostic builds: tail/indirect transfers reach plain dispatch too.
        if (cpu->ppc_fallback_enabled && guest_addr && !(guest_addr & 3u)) {
            {
                static std::mutex seen_mutex;
                static std::unordered_set<uint32_t> seen;
                std::lock_guard lock(seen_mutex);
                if (seen.insert(guest_addr).second)
                    RBREW_LOGF(stderr,"[hybrid] fallback target=%08X lr=%08X\n",guest_addr,cpu->lr);
            }
            const auto result = rbrew_ppc_run(cpu, guest_addr, cpu->lr, cpu->ppc_instruction_budget);
            if (result.stop == RbrewHybridStop::Returned) return;
            RBREW_LOGF(stderr,"[hybrid] rejected target=%08X pc=%08X word=%08X steps=%llu: %s\n",
                guest_addr, result.pc, result.word,
                static_cast<unsigned long long>(result.instructions), result.detail);
            throw RbrewHybridFailure(result);
        }
#endif
        RBREW_COVERAGE_EVENT('U',cpu->lr,guest_addr);
        static const bool stop_on_unhandled = getenv("RBREW_STOP_ON_UNHANDLED") != nullptr;
        // A recurring unresolved call (e.g. once per object per frame) must
        // not turn the diagnostic into the frame's cost: report each target
        // in full eight times, then only its running count at powers of two.
        uint64_t occurrence = 0;
        {
            static std::mutex unhandled_mutex;
            static std::unordered_map<uint32_t, uint64_t> unhandled_counts;
            std::lock_guard lock(unhandled_mutex);
            occurrence = ++unhandled_counts[guest_addr];
        }
        if (occurrence > 8 && !stop_on_unhandled) {
            if ((occurrence & (occurrence - 1)) == 0)
                RBREW_LOGF(stderr, "[rbrew] unhandled dispatch to 0x%08X seen %llu times (last lr=0x%08X)\n",
                        guest_addr, static_cast<unsigned long long>(occurrence), cpu->lr);
            return;
        }
        RBREW_LOGF(stderr, "[rbrew] unhandled dispatch to 0x%08X (lr=0x%08X r1=0x%08X ctr=0x%08X r3=0x%08X r12=0x%08X)\n",
                guest_addr, cpu->lr, cpu->r[1], cpu->ctr, cpu->r[3], cpu->r[12]);
        // Walk guest stack frames (PPC ABI: saved back-chain at r1+0, saved LR at r1+4)
        RBREW_LOGF(stderr, "[rbrew]   guest call chain:");
        uint32_t sp = cpu->r[1];
        for (int i = 0; i < 8 && sp >= 0x10000000u && sp < 0x1FC10000u; i++) {
            uint32_t saved_lr  = read_u32_be(cpu->mem, sp + 4);
            uint32_t saved_sp  = read_u32_be(cpu->mem, sp);
            RBREW_LOGF(stderr, " lr=0x%08X", saved_lr);
            if (saved_sp == 0 || saved_sp == sp) break;
            sp = saved_sp;
        }
        RBREW_LOGF(stderr, "\n");
        if (stop_on_unhandled) {
            RBREW_LOGF(stderr, "[CRASH] Stopping at first unresolved guest dispatch\n");
            exit(2);
        }
    }
}

void rbrew_dispatch_linked(CPUState* cpu, uint32_t guest_addr) {
    if (cpu->differential_validation)
        throw std::runtime_error("dispatch/HLE is forbidden during detached differential replay");
#ifdef RBREW_ENABLE_PPC_FALLBACK
    const uint32_t index = guest_addr >> 2;
    if (cpu->ppc_fallback_enabled) {
        if (!guest_addr || (guest_addr & 3u) || index >= cpu->dispatch_size ||
            !cpu->dispatch_table || !cpu->dispatch_table[index]) {
            // Diagnostic inventory: report each undiscovered target once.
            {
                static std::mutex seen_mutex;
                static std::unordered_set<uint32_t> seen;
                std::lock_guard lock(seen_mutex);
                if (seen.insert(guest_addr).second)
                    RBREW_LOGF(stderr,"[hybrid] fallback target=%08X lr=%08X\n",guest_addr,cpu->lr);
            }
            const auto result = rbrew_ppc_run(cpu, guest_addr, cpu->lr,
                cpu->ppc_instruction_budget);
            if (result.stop == RbrewHybridStop::Returned) return;
            RBREW_LOGF(stderr,"[hybrid] rejected target=%08X pc=%08X word=%08X steps=%llu: %s\n",
                guest_addr, result.pc, result.word,
                static_cast<unsigned long long>(result.instructions), result.detail);
            throw RbrewHybridFailure(result);
        }
    }
#endif
    rbrew_dispatch(cpu, guest_addr);
}

void rbrew_call_indirect(CPUState* cpu, uint32_t guest_addr) {
    rbrew_dispatch_linked(cpu, guest_addr);
}

void rbrew_call(CPUState* cpu, uint32_t guest_addr) {
    rbrew_dispatch(cpu, guest_addr);
}

void rbrew_register_func(CPUState* cpu, uint32_t guest_addr, HostFunc fn) {
    uint32_t idx = guest_addr >> 2;
    if (idx < cpu->dispatch_size)
        cpu->dispatch_table[idx] = fn;
}

#ifdef RBREW_ENABLE_COVERAGE
#include <set>
#include <tuple>
#include <array>
#include <unordered_set>
void rbrew_coverage_event(char kind, uint32_t owner, uint32_t pc) {
    struct Sink {
        std::mutex mutex;
        std::set<std::tuple<char,uint32_t,uint32_t>> seen;
        FILE* file=nullptr; bool limited=false;
        Sink() {
            if(const char* path=rbrew_diagnostic_env("RBREW_COVERAGE_FILE")) {
                file=fopen(path,"wb");
                if(!file)RBREW_LOGF(stderr,"[coverage] cannot open output %s\n",path);
            }
        }
        // Process-lifetime object: worker exit/abort cannot lose buffered records.
    };
    static Sink* sink=new Sink;
    if(!sink->file)return;
    // Hot loops revisit PCs millions of times. Deduplicate per thread before
    // taking the global sink lock; first events still merge across workers.
    unsigned slot=kind=='E'?0:kind=='R'?1:kind=='P'?2:kind=='C'?3:kind=='D'?4:5;
    thread_local std::array<std::unordered_set<uint64_t>,6> local;
    uint64_t key=(uint64_t(owner)<<32)|pc;
    if(local[slot].contains(key))return;
    if(local[slot].size()<1000000)local[slot].insert(key);
    std::lock_guard<std::mutex> lock(sink->mutex);
    if(sink->seen.size()>=1000000) {
        if(!sink->limited){RBREW_LOGF(sink->file,"LIMIT\n");fflush(sink->file);sink->limited=true;}
        return;
    }
    if(sink->seen.emplace(kind,owner,pc).second) {
        RBREW_LOGF(sink->file,"%c %08X %08X\n",kind,owner,pc);fflush(sink->file);
    }
}
#endif
