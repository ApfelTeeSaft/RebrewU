#include "../../runtime/rbrew_diagnostics.h"
#include "../../runtime/texture_diagnostics.h"
// coreinit memory management
// Native application heap, disjoint from the image/stack/bootstrap reservations.
// Exp/Frm heap handles passed-through as the guest address of the heap buffer.
#include "../os_common.h"
#include <stdlib.h>
#include <string.h>
#include <iterator>
#include <map>
#include <unordered_map>
#include <mutex>
#include <stdexcept>

static constexpr uint32_t HEAP_BASE = RBREW_APPLICATION_HEAP_BASE;
static constexpr uint32_t HEAP_SIZE = RBREW_APPLICATION_HEAP_SIZE;

static uint8_t* s_mem = nullptr;
static uint32_t s_heap_ptr = HEAP_BASE;
static uint32_t s_heap_end = HEAP_BASE + HEAP_SIZE;
static std::mutex s_heap_mutex;

// Opt-in call trace (RBREW_MEM_TRACE=1): name, caller LR, r3-r5 and result.
static bool mem_trace_enabled() {
    static const bool enabled = [] {
        const char* v = rbrew_diagnostic_env("RBREW_MEM_TRACE");
        return v && *v && *v != '0';
    }();
    return enabled;
}
#define MEM_TRACE(name, a0, a1, a2) do { if (mem_trace_enabled()) \
    RBREW_LOGF(stderr, "[MEMTRACE] %s lr=%08X r3=%08X r4=%08X r5=%08X -> %08X\n", \
        name, cpu->lr, (uint32_t)(a0), (uint32_t)(a1), (uint32_t)(a2), RET); } while (0)

// Cafe uses the sign to select front/back allocation. Widen before negation
// and address arithmetic so malformed sizes/alignments cannot wrap into RAM.
static uint64_t alignment_magnitude(uint32_t alignment) {
    int64_t value = static_cast<int32_t>(alignment);
    uint64_t magnitude = value < 0 ? uint64_t(-value) : uint64_t(value);
    if (magnitude < 4) magnitude = 4;
    return (magnitude & (magnitude - 1)) ? 0 : magnitude;
}

static uint32_t guest_alloc(uint32_t size, uint32_t align) {
    if (!size) return 0;
    const uint64_t alignment = alignment_magnitude(align);
    if (!alignment) return 0;
    std::lock_guard<std::mutex> lk(s_heap_mutex);
    uint64_t p;
    if (static_cast<int32_t>(align) < 0) {
        if (size > uint64_t(s_heap_end) - s_heap_ptr) return 0;
        p = (uint64_t(s_heap_end) - size) & ~(alignment - 1);
    } else {
        p = (uint64_t(s_heap_ptr) + alignment - 1) & ~(alignment - 1);
    }
    if (p < s_heap_ptr || p + size > s_heap_end) {
        RBREW_LOGF(stderr, "[coreinit] guest heap OOM (requested 0x%X)\n", size);
        return 0;
    }
    if (static_cast<int32_t>(align) < 0) s_heap_end = uint32_t(p);
    else s_heap_ptr = uint32_t(p + size);
    memset(s_mem + p, 0, size);
    return uint32_t(p);
}

// Frame heaps follow Cemu coreinit_MEM_FrmHeap: the 0x4C-byte MEMFrmHeap
// header starts the 4-aligned region, allocation proceeds from head (positive
// alignment) or tail (negative), sizes are padded to 4 and 0 becomes 4. Heap
// state is host-side; the guest header bytes are reserved but not populated.
// Handles not created here keep the legacy shared-pool behavior below.
static constexpr uint32_t FRM_HEAP_HEADER_SIZE = 0x4C;
static constexpr uint32_t MEM_HEAP_FLAG_ZERO_CLEAR = 1;
struct FrmHeap { uint32_t head, tail; bool clear; };
static std::unordered_map<uint32_t, FrmHeap> s_frm_heaps;
static uint32_t s_mem1_heap = 0, s_fg_heap = 0;

static uint32_t frm_heap_create(uint32_t start, uint32_t size, uint32_t flags) {
    const uint64_t first = (uint64_t(start) + 3) & ~uint64_t(3);
    const uint64_t end = (uint64_t(start) + size) & ~uint64_t(3);
    if (!first || first > end || end - first < FRM_HEAP_HEADER_SIZE) return 0;
    if (!rbrew_memory_accessible(s_mem, uint32_t(first), end - first))
        throw std::runtime_error("Frame heap region is outside accessible guest memory");
    std::lock_guard<std::mutex> lk(s_heap_mutex);
    s_frm_heaps[uint32_t(first)] = FrmHeap{uint32_t(first + FRM_HEAP_HEADER_SIZE),
        uint32_t(end), (flags & MEM_HEAP_FLAG_ZERO_CLEAR) != 0};
    return uint32_t(first);
}
static uint64_t frm_alignment(int32_t alignment) {
    const uint64_t magnitude = alignment < 0 ? uint64_t(-int64_t(alignment)) : uint64_t(alignment);
    if (!magnitude || (magnitude & (magnitude - 1)))
        throw std::runtime_error("Frame heap alignment must be a nonzero power of two");
    return magnitude;
}

static void exp_heaps_reset();
void coreinit_memory_init(uint8_t* mem) {
    s_mem = mem;
    s_heap_ptr = HEAP_BASE;
    s_heap_end = HEAP_BASE + HEAP_SIZE;
    {
        std::lock_guard<std::mutex> lk(s_heap_mutex);
        s_frm_heaps.clear();
        exp_heaps_reset();
    }
    s_mem1_heap = frm_heap_create(RBREW_MEM1_BASE, RBREW_MEM1_SIZE, 0);
    s_fg_heap = frm_heap_create(RBREW_FG_FREE_BASE, RBREW_FG_FREE_SIZE, 0);
}

// Base heap slots as Cemu InitDefaultHeaps assigns them: 0 = MEM1 frame heap,
// 1 = MEM2 (legacy shared application pool), 8 = foreground-bucket frame heap.
// Unassigned slots are null.
static void MEMGetBaseHeapHandle(CPUState* cpu) {
    const uint32_t a=ARG0;
    RET = a == 0 ? s_mem1_heap : a == 1 ? 0x50000001u : a == 8 ? s_fg_heap : 0u;
    MEM_TRACE("MEMGetBaseHeapHandle",a,0,0);
}

// Expanded heaps follow Cemu coreinit_MEM_ExpHeap in first-fit mode without
// align margins: a 0x54-byte MEMExpHeapHead2 starts the 4-aligned region,
// every block carries a 0x14-byte MBlock header, head allocations fold their
// alignment padding into the used block, tail allocations fold the trailing
// remainder, and split-off pieces under 0x18 bytes stay inside the used block.
// Block chains are host-side; guest header bytes are reserved, not populated.
static constexpr uint32_t EXP_HEAP_HEADER_SIZE = 0x54;
static constexpr uint32_t EXP_MBLOCK_SIZE = 0x14;
static constexpr uint32_t EXP_MIN_SPLIT = 0x18;
struct ExpUsed { uint32_t start, end; };
struct ExpHeap {
    std::map<uint32_t, uint32_t> free; // MBlock header address -> region end
    std::map<uint32_t, ExpUsed> used;  // returned memory -> owned region
    bool clear;
};
static std::unordered_map<uint32_t, ExpHeap> s_exp_heaps;
static void exp_heaps_reset() { s_exp_heaps.clear(); } // caller holds s_heap_mutex

static uint32_t exp_heap_create(uint32_t start, uint32_t size, uint32_t flags) {
    const uint64_t first = (uint64_t(start) + 3) & ~uint64_t(3);
    const uint64_t end = (uint64_t(start) + size) & ~uint64_t(3);
    if (!first || first > end || end - first < EXP_HEAP_HEADER_SIZE + EXP_MBLOCK_SIZE) return 0;
    if (!rbrew_memory_accessible(s_mem, uint32_t(first), end - first))
        throw std::runtime_error("Expanded heap region is outside accessible guest memory");
    std::lock_guard<std::mutex> lk(s_heap_mutex);
    ExpHeap heap{{}, {}, (flags & MEM_HEAP_FLAG_ZERO_CLEAR) != 0};
    heap.free[uint32_t(first + EXP_HEAP_HEADER_SIZE)] = uint32_t(end);
    s_exp_heaps[uint32_t(first)] = std::move(heap);
    return uint32_t(first);
}
// Caller holds s_heap_mutex. Returns 0 when no free block fits.
static uint32_t exp_heap_alloc(ExpHeap& heap, uint32_t requested, int32_t alignment) {
    const bool tail = alignment < 0;
    const uint64_t align = tail ? uint64_t(-int64_t(alignment)) : uint64_t(alignment);
    if (!align || (align & (align - 1)))
        throw std::runtime_error("Expanded heap alignment must be a nonzero power of two");
    const uint64_t size = ((requested ? uint64_t(requested) : 1u) + 3) & ~uint64_t(3);
    auto fits = [&](uint32_t header, uint32_t end, uint64_t& memory) {
        const uint64_t data = uint64_t(header) + EXP_MBLOCK_SIZE;
        if (tail) {
            if (size > end - data) return false;
            memory = (uint64_t(end) - size) & ~(align - 1);
            return memory >= data;
        }
        memory = (data + align - 1) & ~(align - 1);
        return memory + size <= end;
    };
    uint64_t memory = 0;
    auto block = heap.free.end();
    if (tail) {
        for (auto it = heap.free.rbegin(); it != heap.free.rend(); ++it)
            if (fits(it->first, it->second, memory)) { block = std::prev(it.base()); break; }
    } else {
        for (auto it = heap.free.begin(); it != heap.free.end(); ++it)
            if (fits(it->first, it->second, memory)) { block = it; break; }
    }
    if (block == heap.free.end()) return 0;
    const uint32_t region_start = block->first, region_end = block->second;
    heap.free.erase(block);
    uint64_t used_start = region_start, used_end = region_end;
    const uint64_t left_end = memory - EXP_MBLOCK_SIZE, right_start = memory + size;
    if (tail && left_end - region_start >= EXP_MIN_SPLIT) {
        heap.free[region_start] = uint32_t(left_end); used_start = left_end;
    }
    if (!tail && region_end - right_start >= EXP_MIN_SPLIT) {
        heap.free[uint32_t(right_start)] = region_end; used_end = right_start;
    }
    if (heap.clear) memset(s_mem + used_start, 0, size_t(used_end - used_start));
    heap.used[uint32_t(memory)] = ExpUsed{uint32_t(used_start), uint32_t(used_end)};
    return uint32_t(memory);
}
// Caller holds s_heap_mutex. Coalesces with adjacent free blocks.
static void exp_heap_free(ExpHeap& heap, uint32_t memory) {
    const auto used = heap.used.find(memory);
    if (used == heap.used.end()) throw std::runtime_error("MEMFreeToExpHeap block is not allocated from this heap");
    uint32_t start = used->second.start, end = used->second.end;
    heap.used.erase(used);
    const auto next = heap.free.find(end);
    if (next != heap.free.end()) { end = next->second; heap.free.erase(next); }
    auto prev = heap.free.lower_bound(start);
    if (prev != heap.free.begin() && std::prev(prev)->second == start) {
        --prev; start = prev->first; heap.free.erase(prev);
    }
    if (end - start >= EXP_MBLOCK_SIZE) heap.free[start] = end;
}

// Handles not created by MEMCreateExpHeapEx (MEM2 base handle) keep the
// legacy shared-pool behavior.
static void MEMCreateExpHeapEx(CPUState* cpu)  {
    const uint32_t a=ARG0,b=ARG1,c=ARG2;
    RET = exp_heap_create(a,b,c);
    TXDIAG(txdiag::event("heap_create","\"kind\":\"expanded\",\"heap\":%u,\"size\":%u,\"result\":%u,\"lr\":%u",a,b,RET,cpu->lr));
    MEM_TRACE("MEMCreateExpHeapEx",a,b,c);
}
static void MEMCreateFrmHeapEx(CPUState* cpu)  {
    const uint32_t a=ARG0,b=ARG1,c=ARG2;
    RET = frm_heap_create(a,b,c);
    TXDIAG(txdiag::event("heap_create","\"kind\":\"frame\",\"heap\":%u,\"size\":%u,\"result\":%u,\"lr\":%u",a,b,RET,cpu->lr));
    MEM_TRACE("MEMCreateFrmHeapEx",a,b,c);
}
static void MEMDestroyExpHeap(CPUState* cpu)   {
    TXDIAG(txdiag::event("heap_destroy","\"kind\":\"expanded\",\"heap\":%u,\"lr\":%u",ARG0,cpu->lr));
    std::lock_guard<std::mutex> lk(s_heap_mutex);
    if (!s_exp_heaps.erase(ARG0)) RET = 0;
}
static void MEMDestroyFrmHeap(CPUState* cpu)   {
    TXDIAG(txdiag::event("heap_destroy","\"kind\":\"frame\",\"heap\":%u,\"lr\":%u",ARG0,cpu->lr));
    std::lock_guard<std::mutex> lk(s_heap_mutex);
    if (!s_frm_heaps.erase(ARG0)) RET = 0;
}

static void MEMAllocFromExpHeapEx(CPUState* cpu) {
    const uint32_t a=ARG0,b=ARG1,c=ARG2;
    {
        std::lock_guard<std::mutex> lk(s_heap_mutex);
        const auto heap = s_exp_heaps.find(a);
        if (heap != s_exp_heaps.end()) {
            RET = exp_heap_alloc(heap->second, b, int32_t(c));
            MEM_TRACE("MEMAllocFromExpHeapEx",a,b,c);
            return;
        }
    }
    RET = guest_alloc(ARG1, ARG2 ? ARG2 : 4);
    MEM_TRACE("MEMAllocFromExpHeapEx",a,b,c);
}
static void MEMAllocFromFrmHeapEx(CPUState* cpu) {
    const uint32_t a=ARG0,b=ARG1,c=ARG2;
    bool owned = false, clear = false;
    uint32_t result = 0;
    uint64_t cleared_begin = 0, cleared_end = 0;
    {
        std::lock_guard<std::mutex> lk(s_heap_mutex);
        auto heap = s_frm_heaps.find(a);
        if (heap != s_frm_heaps.end()) {
            owned = true;
            const uint64_t alignment = frm_alignment(int32_t(c));
            const uint64_t size = ((b ? uint64_t(b) : 4u) + 3) & ~uint64_t(3);
            FrmHeap& h = heap->second;
            if (int32_t(c) >= 0) {
                const uint64_t start = (uint64_t(h.head) + alignment - 1) & ~(alignment - 1);
                if (start + size <= h.tail) {
                    cleared_begin = h.head; cleared_end = start + size;
                    h.head = uint32_t(start + size); result = uint32_t(start);
                }
            } else if (size <= h.tail) {
                const uint64_t start = (uint64_t(h.tail) - size) & ~(alignment - 1);
                if (start >= h.head) {
                    cleared_begin = start; cleared_end = h.tail;
                    h.tail = uint32_t(start); result = uint32_t(start);
                }
            }
            clear = h.clear && result;
        }
    }
    if (clear) memset(s_mem + cleared_begin, 0, size_t(cleared_end - cleared_begin));
    RET = owned ? result : guest_alloc(b, c ? c : 4);
    MEM_TRACE("MEMAllocFromFrmHeapEx",a,b,c);
}
static void MEMFreeToExpHeap(CPUState* cpu) {
    TXDIAG(if(txdiag::sample("heap_free",uint64_t(ARG0)<<32|ARG1))
        txdiag::event("heap_free","\"heap\":%u,\"address\":%u,\"lr\":%u",ARG0,ARG1,cpu->lr));
    std::lock_guard<std::mutex> lk(s_heap_mutex);
    const auto heap = s_exp_heaps.find(ARG0);
    if (heap != s_exp_heaps.end() && ARG1) exp_heap_free(heap->second, ARG1);
}

static void MEMGetAllocatableSizeForExpHeapEx(CPUState* cpu) {
    const uint64_t alignment = alignment_magnitude(ARG1);
    if (!alignment) { RET = 0; return; }
    std::lock_guard<std::mutex> lk(s_heap_mutex);
    const auto heap = s_exp_heaps.find(ARG0);
    if (heap != s_exp_heaps.end()) {
        // Cemu: largest (block end - aligned data start) over free blocks.
        uint64_t best = 0;
        for (const auto& [header, end] : heap->second.free) {
            const uint64_t data = uint64_t(header) + EXP_MBLOCK_SIZE;
            const uint64_t aligned = (data + alignment - 1) & ~(alignment - 1);
            if (aligned < end && end - aligned > best) best = end - aligned;
        }
        RET = uint32_t(best);
        MEM_TRACE("MEMGetAllocatableSize",ARG0,ARG1,0);
        return;
    }
    const uint64_t start = (uint64_t(s_heap_ptr) + alignment - 1) & ~(alignment - 1);
    RET = start <= s_heap_end ? uint32_t(s_heap_end - start) : 0;
    MEM_TRACE("MEMGetAllocatableSize",ARG0,ARG1,0);
}
static void MEMGetAllocatableSizeForFrmHeapEx(CPUState* cpu) {
    {
        std::lock_guard<std::mutex> lk(s_heap_mutex);
        auto heap = s_frm_heaps.find(ARG0);
        if (heap != s_frm_heaps.end()) {
            const uint32_t a = ARG0;
            const uint64_t alignment = frm_alignment(int32_t(ARG1));
            // Cemu leaves negative alignment unimplemented; do not guess.
            if (int32_t(ARG1) < 0)
                throw std::runtime_error("Frame heap allocatable size for tail alignment is unproven");
            const uint64_t start = (uint64_t(heap->second.head) + alignment - 1) & ~(alignment - 1);
            RET = start <= heap->second.tail ? uint32_t(heap->second.tail - start) : 0;
            MEM_TRACE("MEMGetAllocatableSizeForFrmHeapEx",a,ARG1,0);
            return;
        }
    }
    MEMGetAllocatableSizeForExpHeapEx(cpu);
}

// ---------------------------------------------------------------------------
// Default heap functions accessed via R_PPC_ADDR16 relocations (not PLT BL).
// The emitter preserved pre-relocation lis+lwz values, so the game reads a
// function pointer from the dimport area in the guest arena. We register these
// host stubs at those dimport addresses and write self-referential pointers.
// ---------------------------------------------------------------------------

// Dimport guest addresses (computed from lis r12,0x02D1 + lwz r12,OFF(r12))
static constexpr uint32_t DIMPORT_MEMAllocFromDefaultHeap   = 0x02D0B8F8u;
static constexpr uint32_t DIMPORT_MEMAllocFromDefaultHeapEx = 0x02D0B900u;
static constexpr uint32_t DIMPORT_MEMFreeToDefaultHeap      = 0x02D0B908u;
// 0x02D0B948 = __gh_FOPEN_MAX  (uint16, data — not a function pointer)
static constexpr uint32_t DIMPORT_GH_FOPEN_MAX              = 0x02D0B948u;

static void MEMAllocFromDefaultHeap(CPUState* cpu) {
    RET = guest_alloc(ARG0, 4);
}
static void MEMAllocFromDefaultHeapEx(CPUState* cpu) {
    const uint32_t a=ARG0,b=ARG1;
    RET = guest_alloc(ARG0, ARG1 ? ARG1 : 4);
    MEM_TRACE("MEMAllocFromDefaultHeapEx",a,b,0);
}
static void MEMFreeToDefaultHeap(CPUState* cpu) { (void)cpu; }

// Write a big-endian 32-bit value into the flat arena byte array.
static void arena_write32(uint8_t* mem, uint32_t addr, uint32_t val) {
    mem[addr]   = (val >> 24) & 0xFF;
    mem[addr+1] = (val >> 16) & 0xFF;
    mem[addr+2] = (val >>  8) & 0xFF;
    mem[addr+3] = (val      ) & 0xFF;
}
static void arena_write16(uint8_t* mem, uint32_t addr, uint16_t val) {
    mem[addr]   = (val >> 8) & 0xFF;
    mem[addr+1] = (val     ) & 0xFF;
}

// Patch the dimport area of the arena and register host stubs.
// Must be called after coreinit_memory_init() so s_mem is set.
// ALSO called by coreinit_dimport_repatch() after Gambit_data_init() because
// the data-section loader overwrites these relocation slots with zeros.
static void coreinit_dimport_patch(CPUState* cpu) {
    // For each function pointer slot: write the slot address into the arena
    // (so rbrew_read32(mem, slot) == slot), then register the host function at
    // that address so rbrew_dispatch(cpu, slot) calls it.
    auto reg_fp = [&](uint32_t addr, HostFunc fn) {
        arena_write32(s_mem, addr, addr);
        rbrew_register_func(cpu, addr, fn);
    };
    reg_fp(DIMPORT_MEMAllocFromDefaultHeap,   MEMAllocFromDefaultHeap);
    reg_fp(DIMPORT_MEMAllocFromDefaultHeapEx, MEMAllocFromDefaultHeapEx);
    reg_fp(DIMPORT_MEMFreeToDefaultHeap,      MEMFreeToDefaultHeap);

    // __gh_FOPEN_MAX: 16-bit constant read by the GHS stdio init code.
    // Value 20 matches the GHS default FOPEN_MAX.
    arena_write16(s_mem, DIMPORT_GH_FOPEN_MAX, 20);
    // _iob and _iob+16 (0x02D0B960 / 0x02D0B970) are FILE struct arrays.
    // The arena is already zeroed; checking code treats zero FILE fields as
    // "not open" / "uninitialized", so no further action needed.
}

// Public re-patch entry point — call this after Gambit_data_init() to restore
// the function-pointer slots that the data-section loader zeros out.
void coreinit_dimport_repatch(CPUState* cpu) {
    coreinit_dimport_patch(cpu);
}

#include "coreinit_addrs.h"
// Persistent guest storage for coreinit-owned data (e.g. OSSystemInfo).
uint32_t coreinit_alloc_system_block(uint32_t size) {
    if (!s_mem) throw std::runtime_error("coreinit memory is not initialized");
    return guest_alloc(size, 64);
}

void coreinit_memory_register(CPUState* cpu) {
#define REG(name) rbrew_register_func(cpu, ADDR_##name, name)
    REG(MEMGetBaseHeapHandle);
    REG(MEMCreateExpHeapEx);
    REG(MEMCreateFrmHeapEx);
    REG(MEMDestroyExpHeap);
    REG(MEMDestroyFrmHeap);
    REG(MEMAllocFromExpHeapEx);
    REG(MEMAllocFromFrmHeapEx);
    REG(MEMFreeToExpHeap);
    REG(MEMGetAllocatableSizeForExpHeapEx);
    REG(MEMGetAllocatableSizeForFrmHeapEx);
#undef REG
    coreinit_dimport_patch(cpu);
}
