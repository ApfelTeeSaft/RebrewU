// coreinit memory management
// Guest heap: bump allocator in 0x20000000-0x2FFFFFFF (256MB)
// Exp/Frm heap handles passed-through as the guest address of the heap buffer.
#include "../os_common.h"
#include <stdlib.h>
#include <string.h>
#include <unordered_map>
#include <mutex>

static constexpr uint32_t HEAP_BASE = 0x28000000u; // starts after pre-seeded region [0x20000000,0x27FFF008)
static constexpr uint32_t HEAP_SIZE = 0x08000000u; // 128MB

static uint8_t* s_mem = nullptr;
static uint32_t s_heap_ptr = HEAP_BASE;
static std::mutex s_heap_mutex;

static uint32_t guest_alloc(uint32_t size, uint32_t align) {
    if (!size) return 0;
    if (align < 4) align = 4;
    std::lock_guard<std::mutex> lk(s_heap_mutex);
    uint32_t p = (s_heap_ptr + align - 1) & ~(align - 1);
    if (p + size > HEAP_BASE + HEAP_SIZE) {
        fprintf(stderr, "[coreinit] guest heap OOM (requested 0x%X)\n", size);
        return 0;
    }
    s_heap_ptr = p + size;
    memset(s_mem + p, 0, size);
    return p;
}

void coreinit_memory_init(uint8_t* mem) {
    s_mem = mem;
    s_heap_ptr = HEAP_BASE;
}

// MEMGetBaseHeapHandle — return a dummy non-null handle
static void MEMGetBaseHeapHandle(CPUState* cpu) { (void)ARG0; RET = 0x50000001u; }

// Exp/Frm/Unit heap — use the passed buffer address as the "handle"
// All variants (with/without Ex suffix) use the same bump allocator
static void MEMCreateExpHeapEx(CPUState* cpu)  { RET = ARG0; } // addr=handle
static void MEMCreateFrmHeapEx(CPUState* cpu)  { RET = ARG0; }
static void MEMDestroyExpHeap(CPUState* cpu)   { (void)cpu; }
static void MEMDestroyFrmHeap(CPUState* cpu)   { (void)cpu; }

static void MEMAllocFromExpHeapEx(CPUState* cpu) {
    RET = guest_alloc(ARG1, ARG2 ? ARG2 : 4);
}
static void MEMAllocFromFrmHeapEx(CPUState* cpu) {
    RET = guest_alloc(ARG1, ARG2 ? ARG2 : 4);
}
static void MEMFreeToExpHeap(CPUState* cpu) { (void)cpu; }

static void MEMGetAllocatableSizeForExpHeapEx(CPUState* cpu) {
    std::lock_guard<std::mutex> lk(s_heap_mutex);
    uint32_t remain = (HEAP_BASE + HEAP_SIZE) - s_heap_ptr;
    RET = remain;
}
static void MEMGetAllocatableSizeForFrmHeapEx(CPUState* cpu) {
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
    RET = guest_alloc(ARG0, ARG1 ? ARG1 : 4);
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
