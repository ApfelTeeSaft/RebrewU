// Gambit PC Port — main entry point
//
// Boot sequence:
//  1. Allocate 512 MB guest memory arena
//  2. Initialize dispatch table
//  3. Register all OS module thunks
//  4. Copy data sections into guest arena
//  5. Set up initial CPUState (stack, SDA bases, etc.)
//  6. Call the game's entry point

#include "runtime/rebrewu_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
static void platform_sleep_us(unsigned us) { Sleep(us / 1000); }
static uint8_t* alloc_arena(size_t sz) {
    return (uint8_t*)VirtualAlloc(nullptr, sz, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}
static void free_arena(uint8_t* p, size_t sz) { (void)sz; VirtualFree(p, 0, MEM_RELEASE); }
#else
#include <sys/mman.h>
#include <unistd.h>
static void platform_sleep_us(unsigned us) { usleep(us); }
static uint8_t* alloc_arena(size_t sz) {
    void* p = mmap(nullptr, sz, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (p == MAP_FAILED) ? nullptr : (uint8_t*)p;
}
static void free_arena(uint8_t* p, size_t sz) { munmap(p, sz); }
#endif

// ---------------------------------------------------------------------------
// OS module register prototypes
// (implemented in os/ subdirectories)
// ---------------------------------------------------------------------------
void gambit_plt_auto_register(CPUState* cpu); // pre-registers STUB for all 610 PLT stubs
void Gambit_game_register(CPUState* cpu);    // registers all recompiled game functions
void coreinit_memory_init(uint8_t* mem);
void coreinit_memory_register(CPUState* cpu);
void coreinit_dimport_repatch(CPUState* cpu); // re-apply after Gambit_data_init
void coreinit_threads_register(CPUState* cpu);
void coreinit_sync_register(CPUState* cpu);
void coreinit_misc_register(CPUState* cpu);
void coreinit_fs_register(CPUState* cpu);
void gx2_context_register(CPUState* cpu);
void gx2_draw_register(CPUState* cpu);
void snd_core_register(CPUState* cpu);
void vpad_register(CPUState* cpu);
void padscore_register(CPUState* cpu);
void stubs_register(CPUState* cpu);
void nsysnet_register(CPUState* cpu);

// ---------------------------------------------------------------------------
// Data section loader
// Each data section was emitted into Gambit_data.cpp as a byte array.
// The emitter generates "void Gambit_data_init(uint8_t* arena)" inside
// Gambit_data.cpp when the cpp_emitter is updated.
// A weak fallback is defined in port/loader/gambit_loader.cpp.
// ---------------------------------------------------------------------------
void Gambit_data_init(uint8_t* arena);

// ---------------------------------------------------------------------------
// Game entry point (first function in the recompiled executable).
// The exact address comes from the RPX entry point field (0x020xxxxx range).
// Declared in Gambit.h via the generated function table.
// ---------------------------------------------------------------------------
// Include Gambit.h only if it exists; otherwise we'll call via dispatch.
#if __has_include("../Gambit/Gambit.h")
#include "../Gambit/Gambit.h"
#endif

// The entry point address is embedded in the binary.
// Use the first declared function as a fallback if we don't have a direct ref.
#ifndef GAMBIT_ENTRY_ADDR
#define GAMBIT_ENTRY_ADDR  0x02000020u   // typical RPX text base + 0x20
#endif

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    (void)argc; (void)argv;

    // 1. Allocate 512 MB guest arena
    const size_t arena_sz = WIIU_MEM_SIZE;
    uint8_t* arena = alloc_arena(arena_sz);
    if (!arena) {
        fprintf(stderr, "[gambit] Failed to allocate %zu MB guest arena\n",
                arena_sz >> 20);
        return 1;
    }
    memset(arena, 0, arena_sz);
    fprintf(stderr, "[gambit] Guest arena: %p – %p (%zu MB)\n",
            (void*)arena, (void*)(arena + arena_sz), arena_sz >> 20);

    // 2. Allocate dispatch table (covers all addresses up to 0x0D000000)
    const uint32_t DISPATCH_MAX = 0x0D000000u;
    const uint32_t DISPATCH_LEN = DISPATCH_MAX >> 2;
    HostFunc* dispatch_table = (HostFunc*)calloc(DISPATCH_LEN, sizeof(HostFunc));
    if (!dispatch_table) {
        fprintf(stderr, "[gambit] Failed to allocate dispatch table\n");
        free_arena(arena, arena_sz);
        return 1;
    }

    // 3. Set up a scratch CPUState for registration calls
    CPUState cpu_reg = {};
    cpu_reg.mem            = arena;
    cpu_reg.dispatch_table = dispatch_table;
    cpu_reg.dispatch_size  = DISPATCH_LEN;

    // 4. Initialize OS modules and register host thunks
    // Pre-register STUB for all 610 PLT stubs so every import call is safe
    gambit_plt_auto_register(&cpu_reg);
    // Now override with real implementations
    coreinit_memory_init(arena);
    coreinit_memory_register(&cpu_reg);
    coreinit_threads_register(&cpu_reg);
    coreinit_sync_register(&cpu_reg);
    coreinit_misc_register(&cpu_reg);
    coreinit_fs_register(&cpu_reg);
    gx2_context_register(&cpu_reg);
    gx2_draw_register(&cpu_reg);
    snd_core_register(&cpu_reg);
    vpad_register(&cpu_reg);
    padscore_register(&cpu_reg);
    stubs_register(&cpu_reg);
    nsysnet_register(&cpu_reg);
    // Register all recompiled game functions so rbrew_dispatch can find them
    Gambit_game_register(&cpu_reg);

    // Override fn_02817C50 (0x02817C50): GHS fiber "wait for first frame" loop.
    //
    // Called as vtable[45] from fn_02818CAC during Gambit__start.
    // It loops at L_02817CAC calling render-update helpers (fn_0281DFD8 etc.)
    // until a fiber-done flag (fiber_obj+0xB4) becomes non-zero.  On Wii U
    // the fiber scheduler running on a second core sets that flag after the
    // first render fiber completes.  In our single-threaded port the flag is
    // never set (arena is zeroed), so the loop runs forever.
    //
    // Call chain: Gambit__start → fn_02797FB4 → fn_02813F44 (vtable[9])
    //             → fn_02818CAC (vtable[23]) → fn_02817C50 (vtable[45])
    //
    // fn_02818190 (which writes the render context to 0x101C98A8) runs inside
    // fn_02797FB4 *before* vtable[9] is called, so the render context is
    // already valid when we short-circuit here.
    rbrew_register_func(&cpu_reg, 0x02817C50u, [](CPUState*) {});

    // Override fn_02818D40 and fn_02818D44 (0x02818D40 / 0x02818D44):
    // GHS fiber scheduler dispatch loops.  On Wii U these run forever calling
    // vtable[0xCC] (fn_02796E04) to schedule the next fiber frame.  In our
    // single-threaded port we drive the game loop directly from main(), so we
    // return immediately here.  These are reached via:
    //   Gambit__start → fn_02797FB4 → fn_02813F44 (vtable[9])
    //                 → fn_02818CAC (vtable[23]) → fn_02818D40 (vtable[49])
    // fn_02818190 (which writes the render context to 0x101C98A8) runs inside
    // fn_02797FB4 *before* the vtable chain above, so the context is already
    // set up when we return here and Gambit__start completes normally.
    rbrew_register_func(&cpu_reg, 0x02818D40u, [](CPUState*) {});
    rbrew_register_func(&cpu_reg, 0x02818D44u, [](CPUState*) {});

    // Override fn_02C8706C (0x02C8706C): the GHS vsync-wait frame-sync function.
    //
    // On Wii U this raises GHS signal 6 which longjmps into the fiber scheduler,
    // switching execution to the render fiber (fn_028194E4).  The render fiber
    // calls the game's render callback (GX2SetColorBuffer + all draw calls),
    // GX2CopyColorBufferToScanBuffer, and GX2SwapScanBuffers, then yields back.
    //
    // In our single-threaded port we simulate this round-trip by:
    //   1. Reading the render context pointer from 0x101C98A8
    //      (written there by fn_02818190 during Gambit__start below).
    //   2. Directly calling fn_028194E4 (render fiber per-frame) with that ctx.
    //      fn_028194E4 invokes the render callback and, if ctx+0x28==2,
    //      calls GX2SwapScanBuffers (pumps SDL events + swaps the GL buffer).
    //   3. Unconditionally calling GX2SwapScanBuffers afterwards to guarantee
    //      event pumping and buffer swap even if ctx+0x28 is not yet 2.
    rbrew_register_func(&cpu_reg, 0x02C8706Cu,
                        [](CPUState* cpu) {
                            uint32_t render_ctx = rbrew_read32(cpu->mem, 0x101C98A8u);
                            if (render_ctx != 0) {
                                // Run render fiber: calls render callback, copy-to-scan,
                                // and (conditionally) GX2SwapScanBuffers.
                                cpu->r[3] = render_ctx;
                                rbrew_dispatch(cpu, 0x028194E4u); // fn_028194E4
                            }
                            // Always swap to pump SDL events and present the frame.
                            rbrew_dispatch(cpu, 0x02D062F8u); // GX2SwapScanBuffers
                        });

    fprintf(stderr, "[gambit] OS modules registered\n");

    // 5. Copy data sections into arena
    Gambit_data_init(arena);
    fprintf(stderr, "[gambit] Data sections loaded\n");

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
    fprintf(stderr, "[gambit] Dimport function pointers re-patched\n");

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

        fprintf(stderr, "[gambit] Heap pre-seeded: free=[0x%08X,0x%08X) %u MB\n",
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
    cpu.r[1]  = STACK_TOP - 8;   // r1 = stack pointer (8-byte aligned)
    cpu.r[2]  = 0x10000000u;     // r2  = SDA2 base (.rodata anchor)
    cpu.r[13] = 0x101C0BC0u;     // r13 = SDA  base (.data anchor)

    // 6b. Run static constructors from the GHS ctor table in .rodata.
    //
    // The GHS runtime normally calls these from _start before jumping to main().
    // Our port enters at the game's own entry directly, so we must run them here.
    //
    // Ctor table: array of function pointers in .rodata at 0x101BFA54.
    // Terminated by a null entry.  The GHS "ctors done" flag lives in .bss at
    // 0x101DC020 (zeroed = not yet run); we set it to 1 after running so the
    // game's own _start won't re-run them if reached.
    {
        const uint32_t CTOR_TABLE     = 0x101BFA54u;
        const uint32_t CTOR_DONE_FLAG = 0x101DC020u;
        uint32_t n_ctors = 0;
        for (;;) {
            uint32_t fn = rbrew_read32(arena, CTOR_TABLE + n_ctors * 4u);
            if (fn == 0) break;
            rbrew_dispatch(&cpu, fn);
            n_ctors++;
        }
        rbrew_write32(arena, CTOR_DONE_FLAG, 1u);
        fprintf(stderr, "[gambit] Ran %u static constructors\n", n_ctors);
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
    fprintf(stderr, "[gambit] GHS TLS allocator handle cleared\n");

    fprintf(stderr, "[gambit] Entering game entry point 0x%08X\n",
            GAMBIT_ENTRY_ADDR);

    // 7c. Call entry point via dispatch.
    // fn_02000020 (GHS _start) expects r3=0 to allocate a new fiber context.
    // It returns the context pointer in r3.
    cpu.r[3] = 0;
    rbrew_dispatch(&cpu, GAMBIT_ENTRY_ADDR);

    uint32_t game_ctx = cpu.r[3];
    fprintf(stderr, "[gambit] GHS context created: 0x%08X\n", game_ctx);

    // 7d. Run the game main loop directly (fn_02C83190).
    //
    // On real Wii U the GHS OS scheduler populates the fiber task queue before
    // calling fn_02000130, which then dispatches via sched->vtable[9] into the
    // game loop.  In our single-threaded port the task slot starts at 0 because
    // the factory fn_02826F20 depends on a global (0x101C994C) that is only set
    // by the OS, not by any static constructor.
    //
    // fn_02C83190 is a self-contained infinite update loop.  It begins with
    //   L_02C83190: r3 = 0xD; goto body
    // and then calls a chain of per-frame update functions before looping back.
    // Dispatching directly to it bypasses the broken fiber scheduler and runs
    // the game loop correctly.
    //
    // IMPORTANT: The C++ translation of Gambit_fn_02C83190 starts at label
    // L_02C83138 (the real function body), not at L_02C83190 (the loop-back
    // entry that sets r3=0xD).  Pre-set r3=0xD here to match what L_02C83190
    // would have done before branching to the body.

    // 7e. Run the game's own _start (Gambit__start @ 0x02C6BCA4).
    //
    // On Wii U the GHS scheduler calls this after fn_02000020 returns.  It runs
    // the render-system initialisation chain:
    //   Gambit__start → fn_027962E8 → fn_02797FB4 → fn_02818190
    // fn_02818190 calls GX2Init (opens the SDL window), allocates the render
    // context struct (0x160 bytes), sets up framebuffers and shader state, and
    // stores the render context pointer at 0x101C98A8.  The fn_02C8706C override
    // above reads that pointer every frame to drive fn_028194E4 (render fiber).
    //
    // r3=0, r4=0 are safe: fn_027962E8 stores them as "parent object" pointers
    // at 0x101CB240/244 but fn_02797FB4 (the actual renderer init) ignores them.
    cpu.r[3] = 0u;
    cpu.r[4] = 0u;
    rbrew_dispatch(&cpu, 0x02C6BCA4u); // Gambit__start
    {
        uint32_t render_ctx = rbrew_read32(arena, 0x101C98A8u);
        fprintf(stderr, "[gambit] Gambit__start done; render_ctx=0x%08X\n", render_ctx);
    }

    // 7f. Ensure the SDL window and GL context exist (Gambit__start calls GX2Init
    //     internally via fn_02818190; this is a no-op if the window is already open).
    cpu.r[3] = 0u; // attribs = NULL
    rbrew_dispatch(&cpu, 0x02D05B78u); // GX2Init (idempotent)
    fprintf(stderr, "[gambit] GX2 init confirmed\n");

    cpu.r[3] = 0x0000000Du; // mimic L_02C83190: li r3, 0xD
    fprintf(stderr, "[gambit] ctx=0x%08X — entering game main loop (fn_02C83190)\n",
            game_ctx);
    rbrew_dispatch(&cpu, 0x02C83190u);
    fprintf(stderr, "[gambit] Game loop returned (should not happen)\n");

    free(dispatch_table);
    free_arena(arena, arena_sz);
    return 0;
}