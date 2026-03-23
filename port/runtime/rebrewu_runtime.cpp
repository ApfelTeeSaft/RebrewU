#include "rebrewu_runtime.h"
#include <stdio.h>
#include <stdlib.h>

// Dispatch table is a flat array: dispatch_table[addr >> 2] = HostFunc
// Max guest address we care about is ~0x0D000000 (320MB covers all stubs)
#define DISPATCH_MAX_ADDR  0x0D000000u
#define DISPATCH_ENTRIES   (DISPATCH_MAX_ADDR >> 2)

static uint32_t read_u32_be(const uint8_t* mem, uint32_t addr) {
    return ((uint32_t)mem[addr] << 24) | ((uint32_t)mem[addr+1] << 16) |
           ((uint32_t)mem[addr+2] <<  8) | (uint32_t)mem[addr+3];
}

void rbrew_dispatch(CPUState* cpu, uint32_t guest_addr) {
    uint32_t idx = guest_addr >> 2;
    if (idx < cpu->dispatch_size && cpu->dispatch_table[idx]) {
        cpu->dispatch_table[idx](cpu);
    } else {
        fprintf(stderr, "[rbrew] unhandled dispatch to 0x%08X (lr=0x%08X r1=0x%08X ctr=0x%08X r3=0x%08X r12=0x%08X)\n",
                guest_addr, cpu->lr, cpu->r[1], cpu->ctr, cpu->r[3], cpu->r[12]);
        // Walk guest stack frames (PPC ABI: saved back-chain at r1+0, saved LR at r1+4)
        fprintf(stderr, "[rbrew]   guest call chain:");
        uint32_t sp = cpu->r[1];
        for (int i = 0; i < 8 && sp >= 0x10000000u && sp < 0x1FC10000u; i++) {
            uint32_t saved_lr  = read_u32_be(cpu->mem, sp + 4);
            uint32_t saved_sp  = read_u32_be(cpu->mem, sp);
            fprintf(stderr, " lr=0x%08X", saved_lr);
            if (saved_sp == 0 || saved_sp == sp) break;
            sp = saved_sp;
        }
        fprintf(stderr, "\n");
    }
}

void rbrew_call_indirect(CPUState* cpu, uint32_t guest_addr) {
    rbrew_dispatch(cpu, guest_addr);
}

void rbrew_call(CPUState* cpu, uint32_t guest_addr) {
    rbrew_dispatch(cpu, guest_addr);
}

void rbrew_register_func(CPUState* cpu, uint32_t guest_addr, HostFunc fn) {
    uint32_t idx = guest_addr >> 2;
    if (idx < cpu->dispatch_size)
        cpu->dispatch_table[idx] = fn;
}
