#include "rebrewu_ppc_interpreter.h"

namespace {

uint32_t sign_extend(uint32_t value, unsigned width) {
    const uint32_t sign = 1u << (width - 1u);
    return (value ^ sign) - sign;
}

bool in_arena(const CPUState* cpu, uint32_t address, uint32_t size) {
    return cpu->mem && cpu->guest_memory_size <= WIIU_MEM_SIZE &&
        uint64_t(address) + size <= cpu->guest_memory_size &&
        rbrew_memory_accessible(cpu->mem, address, size);
}

uint32_t read_be(const uint8_t* bytes, uint32_t size) {
    uint32_t result = 0;
    for (uint32_t i = 0; i < size; ++i) result = (result << 8) | bytes[i];
    return result;
}

void write_be(CPUState* cpu, uint32_t address, uint32_t size, uint32_t value) {
    uint8_t bytes[4];
    for (uint32_t i = 0; i < size; ++i) {
        bytes[size - 1u - i] = uint8_t(value & 255u);
        value >>= 8;
    }
    // The existing shared monitor invalidates reservations, including ABA writes.
    rbrew_write_memory(cpu->mem, address, bytes, size);
}

uint32_t compare_unsigned(uint32_t a, uint32_t b, uint32_t xer) {
    return (a < b ? 8u : a > b ? 4u : 2u) | ((xer >> 31) & 1u);
}

uint32_t compare_signed(uint32_t a, uint32_t b, uint32_t xer) {
    // Bias the sign bit to obtain signed ordering without host signed overflow.
    return compare_unsigned(a ^ 0x80000000u, b ^ 0x80000000u, xer);
}

void record_integer(CPUState* cpu, uint32_t value) {
    cpu->cr[0] = compare_signed(value, 0, cpu->xer);
}

uint32_t rotate_word(uint32_t value, unsigned shift) {
    // Intentionally independent of the generated runtime's rotate/mask helpers.
    uint32_t result = 0;
    for (unsigned bit = 0; bit < 32; ++bit) {
        const unsigned source = (bit + 32u - shift) & 31u;
        result |= ((value >> source) & 1u) << bit;
    }
    return result;
}

uint32_t cyclic_mask(unsigned begin, unsigned end) {
    uint32_t mask = 0;
    for (unsigned bit = 0; bit < 32; ++bit) {
        const bool selected = begin <= end ? bit >= begin && bit <= end
                                          : bit >= begin || bit <= end;
        if (selected) mask |= 1u << (31u - bit);
    }
    return mask;
}

bool supported_bo(uint32_t bo) {
    // PowerPC pre-v2 BO encodings: prediction y is accepted; reserved z bits
    // are not silently accepted as newer ISA hint extensions.
    if ((bo & 16u) && (bo & 8u)) return false;
    if ((bo & 4u) && (bo & 2u)) return false;
    if ((bo & 20u) == 20u && (bo & 1u)) return false;
    return true;
}

bool branch_condition(CPUState* cpu, uint32_t bo, uint32_t bi) {
    bool count_ok = true;
    if (!(bo & 4u)) {
        cpu->ctr -= 1u;
        count_ok = (cpu->ctr != 0u) != bool(bo & 2u);
    }
    const uint32_t cr_bit = (cpu->cr[bi / 4u] >> (3u - bi % 4u)) & 1u;
    const bool condition_ok = (bo & 16u) || cr_bit == uint32_t(bool(bo & 8u));
    return count_ok && condition_ok;
}

} // namespace

RbrewPPCStep rbrew_ppc_step(CPUState* cpu, uint32_t pc) {
    RbrewPPCStep result{RbrewPPCStop::Continue, RbrewPPCTransfer::Sequential,
                        pc + 4u, 0u, "completed"};
    const auto stop = [&](RbrewPPCStop reason, const char* detail) {
        result.stop = reason;
        result.next_pc = pc;
        result.detail = detail;
        return result;
    };
    if (!cpu || (pc & 3u) || !in_arena(cpu, pc, 4u)) {
        return stop(RbrewPPCStop::InvalidPC, "instruction fetch outside aligned arena");
    }

    const uint32_t word = read_be(cpu->mem + pc, 4u);
    result.word = word;
    const uint32_t op = word >> 26;
    const uint32_t rt = (word >> 21) & 31u;
    const uint32_t ra = (word >> 16) & 31u;
    const uint32_t rb = (word >> 11) & 31u;
    const uint32_t immediate = word & 65535u;
    const bool rc = (word & 1u) != 0u;

    switch (op) {
    case 14: // addi (including li); RA=0 is an architectural zero base.
    case 15: { // addis
        const uint32_t value = sign_extend(immediate, 16u);
        cpu->r[rt] = (ra ? cpu->r[ra] : 0u) + (op == 15 ? value << 16 : value);
        return result;
    }
    case 24: // ori
    case 25: // oris
    case 26: // xori
    case 27: // xoris
    case 28: // andi. (record is mandatory)
    case 29: { // andis.
        const uint32_t value = (op & 1u) ? immediate << 16 : immediate;
        uint32_t output;
        if (op < 26) output = cpu->r[rt] | value;
        else if (op < 28) output = cpu->r[rt] ^ value;
        else output = cpu->r[rt] & value;
        cpu->r[ra] = output;
        if (op >= 28) record_integer(cpu, output);
        return result;
    }
    case 10: // cmpli
    case 11: { // cmpi: BF, reserved bit, L, RA, immediate.
        if (word & 0x00600000u) {
            return stop(RbrewPPCStop::Unsupported, "64-bit or reserved immediate compare");
        }
        const uint32_t field = (word >> 23) & 7u;
        cpu->cr[field] = op == 10 ? compare_unsigned(cpu->r[ra], immediate, cpu->xer)
            : compare_signed(cpu->r[ra], sign_extend(immediate, 16u), cpu->xer);
        return result;
    }
    case 20: // rlwimi
    case 21: { // rlwinm
        const unsigned begin = (word >> 6) & 31u;
        const unsigned end = (word >> 1) & 31u;
        const uint32_t mask = cyclic_mask(begin, end);
        const uint32_t rotated = rotate_word(cpu->r[rt], rb);
        const uint32_t output = op == 20 ? (cpu->r[ra] & ~mask) | (rotated & mask)
                                        : rotated & mask;
        cpu->r[ra] = output;
        if (rc) record_integer(cpu, output);
        return result;
    }
    case 32: // lwz
    case 34: // lbz
    case 40: // lhz
    case 42: // lha
    case 36: // stw
    case 37: // stwu
    case 38: // stb
    case 44: { // sth
        if (op == 37 && !ra) {
            return stop(RbrewPPCStop::Unsupported, "store-update requires nonzero RA");
        }
        const uint32_t size = op == 34 || op == 38 ? 1u
                            : op == 40 || op == 42 || op == 44 ? 2u : 4u;
        const uint32_t address = (ra ? cpu->r[ra] : 0u) + sign_extend(immediate, 16u);
        if (address & (size - 1u)) {
            return stop(RbrewPPCStop::MemoryFault, "unaligned data access is outside prototype");
        }
        if (!in_arena(cpu, address, size)) {
            return stop(RbrewPPCStop::MemoryFault, "data access outside guest arena");
        }
        const bool store = op == 36 || op == 37 || op == 38 || op == 44;
        if (store) {
            write_be(cpu, address, size, cpu->r[rt]);
            if (op == 37) cpu->r[ra] = address; // Store old RS even when RS=RA.
        } else {
            uint32_t value = read_be(cpu->mem + address, size);
            if (op == 42) value = sign_extend(value, 16u);
            cpu->r[rt] = value;
        }
        return result;
    }
    case 18: { // b: relative target uses CIA, not CIA+4.
        uint32_t target = sign_extend(word & 0x03FFFFFCu, 26u);
        if (!(word & 2u)) target += pc;
        if (rc) cpu->lr = pc + 4u;
        result.next_pc = target;
        result.transfer = rc ? RbrewPPCTransfer::Call : RbrewPPCTransfer::Branch;
        return result;
    }
    case 16: // bc
    case 19: { // bclr/bcctr; all other XL encodings fail closed.
        const uint32_t bo = rt;
        const uint32_t bi = ra;
        const uint32_t xo = (word >> 1) & 1023u;
        if (op == 19 && (xo != 16u && xo != 528u)) {
            return stop(RbrewPPCStop::Unsupported, "unsupported XL instruction");
        }
        if (!supported_bo(bo)) {
            return stop(RbrewPPCStop::Unsupported, "unproved reserved BO encoding");
        }
        if (op == 19 && rb) {
            return stop(RbrewPPCStop::Unsupported, "unproved XL reserved or branch-hint field");
        }
        if (op == 19 && xo == 528u && !(bo & 4u)) {
            return stop(RbrewPPCStop::Unsupported, "bcctr cannot decrement its target CTR");
        }
        // Capture old LR/CTR before predicate effects or linked LR publication.
        uint32_t target;
        if (op == 19) target = (xo == 16u ? cpu->lr : cpu->ctr) & ~3u;
        else {
            target = sign_extend(word & 0xFFFCu, 16u);
            if (!(word & 2u)) target += pc;
        }
        const bool taken = branch_condition(cpu, bo, bi);
        if (rc) cpu->lr = pc + 4u; // LK applies even on conditional fallthrough.
        if (taken) {
            result.next_pc = target;
            result.transfer = rc ? RbrewPPCTransfer::Call : RbrewPPCTransfer::Branch;
        }
        return result;
    }
    case 31: {
        const uint32_t xo = (word >> 1) & 1023u;
        switch (xo) {
        case 0: // cmp
        case 32: { // cmpl
            if (rc || (word & 0x00600000u)) {
                return stop(RbrewPPCStop::Unsupported, "64-bit or reserved register compare");
            }
            const uint32_t field = (word >> 23) & 7u;
            cpu->cr[field] = xo == 0 ? compare_signed(cpu->r[ra], cpu->r[rb], cpu->xer)
                                  : compare_unsigned(cpu->r[ra], cpu->r[rb], cpu->xer);
            return result;
        }
        case 28: // and
        case 316: // xor
        case 444: { // or (including mr)
            const uint32_t output = xo == 28 ? cpu->r[rt] & cpu->r[rb]
                                  : xo == 316 ? cpu->r[rt] ^ cpu->r[rb]
                                              : cpu->r[rt] | cpu->r[rb];
            cpu->r[ra] = output;
            if (rc) record_integer(cpu, output);
            return result;
        }
        case 40: // subf: RB - RA, without OE or CA changes.
        case 266: { // add: without OE or CA changes.
            const uint32_t output = xo == 40 ? cpu->r[rb] - cpu->r[ra]
                                            : cpu->r[ra] + cpu->r[rb];
            cpu->r[rt] = output;
            if (rc) record_integer(cpu, output);
            return result;
        }
        case 339: // mfspr
        case 467: { // mtspr; five-bit SPR halves are swapped in the encoding.
            if (rc) return stop(RbrewPPCStop::Unsupported, "reserved SPR record bit");
            const uint32_t spr = ra | (rb << 5u);
            uint32_t* special = spr == 1 ? &cpu->xer : spr == 8 ? &cpu->lr
                                : spr == 9 ? &cpu->ctr : nullptr;
            if (!special) return stop(RbrewPPCStop::Unsupported, "unsupported special register");
            if (xo == 339) cpu->r[rt] = *special;
            else *special = cpu->r[rt];
            return result;
        }
        default:
            return stop(RbrewPPCStop::Unsupported, "unsupported X/XO instruction");
        }
    }
    default:
        return stop(RbrewPPCStop::Unsupported, "unsupported primary opcode");
    }
}
