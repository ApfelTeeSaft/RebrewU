#include "disasm.h"
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace ppc {

// PowerPC instruction table (simplified for WiiU)
static const ppc_opcode opcodes[] = {
    // Arithmetic instructions
    { "add", 0x7C000214, 0xFC0007FE, {0, 1, 2, -1}, PPC_INST_ADD },
    { "add.", 0x7C000215, 0xFC0007FF, {0, 1, 2, -1}, PPC_INST_ADD },
    { "addi", 0x38000000, 0xFC000000, {0, 1, 2, -1}, PPC_INST_ADDI },
    { "addis", 0x3C000000, 0xFC000000, {0, 1, 2, -1}, PPC_INST_ADDIS },
    { "addic", 0x30000000, 0xFC000000, {0, 1, 2, -1}, PPC_INST_ADDIC },
    { "addic.", 0x34000000, 0xFC000000, {0, 1, 2, -1}, PPC_INST_ADDIC },
    
    // Logic instructions
    { "and", 0x7C000038, 0xFC0007FE, {0, 1, 2, -1}, PPC_INST_AND },
    { "and.", 0x7C000039, 0xFC0007FF, {0, 1, 2, -1}, PPC_INST_AND },
    { "andi.", 0x70000000, 0xFC000000, {0, 1, 2, -1}, PPC_INST_ANDI },
    { "andis.", 0x74000000, 0xFC000000, {0, 1, 2, -1}, PPC_INST_ANDIS },
    { "or", 0x7C000378, 0xFC0007FE, {0, 1, 2, -1}, PPC_INST_OR },
    { "or.", 0x7C000379, 0xFC0007FF, {0, 1, 2, -1}, PPC_INST_OR },
    { "ori", 0x60000000, 0xFC000000, {0, 1, 2, -1}, PPC_INST_ORI },
    { "oris", 0x64000000, 0xFC000000, {0, 1, 2, -1}, PPC_INST_ORIS },
    { "xor", 0x7C000278, 0xFC0007FE, {0, 1, 2, -1}, PPC_INST_XOR },
    { "xori", 0x68000000, 0xFC000000, {0, 1, 2, -1}, PPC_INST_XORI },
    { "xoris", 0x6C000000, 0xFC000000, {0, 1, 2, -1}, PPC_INST_XORIS },
    
    // Branch instructions
    { "b", 0x48000000, 0xFC000003, {0, -1, -1, -1}, PPC_INST_B },
    { "ba", 0x48000002, 0xFC000003, {0, -1, -1, -1}, PPC_INST_BA },
    { "bl", 0x48000001, 0xFC000003, {0, -1, -1, -1}, PPC_INST_BL },
    { "bc", 0x40000000, 0xFC000003, {0, 1, 2, -1}, PPC_INST_BC },
    { "bclr", 0x4C000020, 0xFC00FFFE, {0, 1, -1, -1}, PPC_INST_BCLR },
    { "bcctr", 0x4C000420, 0xFC00FFFE, {0, 1, -1, -1}, PPC_INST_BCCTR },
    { "blr", 0x4E800020, 0xFFFFFFFF, {-1, -1, -1, -1}, PPC_INST_BLR },
    { "bctr", 0x4E800420, 0xFFFFFFFF, {-1, -1, -1, -1}, PPC_INST_BCTR },
    
    // Load/Store instructions
    { "lwz", 0x80000000, 0xFC000000, {0, 1, 2, -1}, PPC_INST_LWZ },
    { "lwzu", 0x84000000, 0xFC000000, {0, 1, 2, -1}, PPC_INST_LWZU },
    { "lwzx", 0x7C00002E, 0xFC0007FE, {0, 1, 2, -1}, PPC_INST_LWZX },
    { "stw", 0x90000000, 0xFC000000, {0, 1, 2, -1}, PPC_INST_STW },
    { "stwu", 0x94000000, 0xFC000000, {0, 1, 2, -1}, PPC_INST_STWU },
    { "stwx", 0x7C00012E, 0xFC0007FE, {0, 1, 2, -1}, PPC_INST_STWX },
    { "lbz", 0x88000000, 0xFC000000, {0, 1, 2, -1}, PPC_INST_LBZ },
    { "lbzx", 0x7C0000AE, 0xFC0007FE, {0, 1, 2, -1}, PPC_INST_LBZX },
    { "stb", 0x98000000, 0xFC000000, {0, 1, 2, -1}, PPC_INST_STB },
    { "stbx", 0x7C0001AE, 0xFC0007FE, {0, 1, 2, -1}, PPC_INST_STBX },
    { "lhz", 0xA0000000, 0xFC000000, {0, 1, 2, -1}, PPC_INST_LHZ },
    { "lhzx", 0x7C00022E, 0xFC0007FE, {0, 1, 2, -1}, PPC_INST_LHZX },
    { "sth", 0xB0000000, 0xFC000000, {0, 1, 2, -1}, PPC_INST_STH },
    { "sthx", 0x7C00032E, 0xFC0007FE, {0, 1, 2, -1}, PPC_INST_STHX },
    
    // Floating point instructions
    { "fadd", 0xFC00002A, 0xFC0007FE, {0, 1, 2, -1}, PPC_INST_FADD },
    { "fadds", 0xEC00002A, 0xFC0007FE, {0, 1, 2, -1}, PPC_INST_FADDS },
    { "fsub", 0xFC000028, 0xFC0007FE, {0, 1, 2, -1}, PPC_INST_FSUB },
    { "fsubs", 0xEC000028, 0xFC0007FE, {0, 1, 2, -1}, PPC_INST_FSUBS },
    { "fmul", 0xFC000032, 0xFC0007FE, {0, 1, 3, -1}, PPC_INST_FMUL },
    { "fmuls", 0xEC000032, 0xFC0007FE, {0, 1, 3, -1}, PPC_INST_FMULS },
    { "fdiv", 0xFC000024, 0xFC0007FE, {0, 1, 2, -1}, PPC_INST_FDIV },
    { "fdivs", 0xEC000024, 0xFC0007FE, {0, 1, 2, -1}, PPC_INST_FDIVS },
    { "fmadd", 0xFC00003A, 0xFC0007FE, {0, 1, 3, 2}, PPC_INST_FMADD },
    { "fmadds", 0xEC00003A, 0xFC0007FE, {0, 1, 3, 2}, PPC_INST_FMADDS },
    { "lfs", 0xC0000000, 0xFC000000, {0, 1, 2, -1}, PPC_INST_LFS },
    { "lfd", 0xC8000000, 0xFC000000, {0, 1, 2, -1}, PPC_INST_LFD },
    { "stfs", 0xD0000000, 0xFC000000, {0, 1, 2, -1}, PPC_INST_STFS },
    { "stfd", 0xD8000000, 0xFC000000, {0, 1, 2, -1}, PPC_INST_STFD },
    
    // Paired single instructions (WiiU specific)
    { "ps_add", 0x1000002A, 0xFC0007FE, {0, 1, 2, -1}, PPC_INST_PS_ADD },
    { "ps_sub", 0x10000028, 0xFC0007FE, {0, 1, 2, -1}, PPC_INST_PS_SUB },
    { "ps_mul", 0x10000032, 0xFC0007FE, {0, 1, 3, -1}, PPC_INST_PS_MUL },
    { "ps_div", 0x10000024, 0xFC0007FE, {0, 1, 2, -1}, PPC_INST_PS_DIV },
    { "ps_madd", 0x1000003A, 0xFC0007FE, {0, 1, 3, 2}, PPC_INST_PS_MADD },
    { "ps_merge00", 0x10000420, 0xFC0007FE, {0, 1, 2, -1}, PPC_INST_PS_MERGE00 },
    { "ps_merge01", 0x10000460, 0xFC0007FE, {0, 1, 2, -1}, PPC_INST_PS_MERGE01 },
    { "ps_merge10", 0x100004A0, 0xFC0007FE, {0, 1, 2, -1}, PPC_INST_PS_MERGE10 },
    { "ps_merge11", 0x100004E0, 0xFC0007FE, {0, 1, 2, -1}, PPC_INST_PS_MERGE11 },
    { "psq_l", 0xE0000000, 0xFC000000, {0, 1, 2, 3}, PPC_INST_PSQ_L },
    { "psq_st", 0xF0000000, 0xFC000000, {0, 1, 2, 3}, PPC_INST_PSQ_ST },
    
    // System instructions
    { "mflr", 0x7C0802A6, 0xFC1FFFFF, {0, -1, -1, -1}, PPC_INST_MFLR },
    { "mtlr", 0x7C0803A6, 0xFC1FFFFF, {0, -1, -1, -1}, PPC_INST_MTLR },
    { "mfctr", 0x7C0902A6, 0xFC1FFFFF, {0, -1, -1, -1}, PPC_INST_MFCTR },
    { "mtctr", 0x7C0903A6, 0xFC1FFFFF, {0, -1, -1, -1}, PPC_INST_MTCTR },
    { "mfcr", 0x7C000026, 0xFC1FFFFF, {0, -1, -1, -1}, PPC_INST_MFCR },
    { "mtcrf", 0x7C000120, 0xFC100FFF, {0, 1, -1, -1}, PPC_INST_MTCRF },
    { "isync", 0x4C00012C, 0xFFFFFFFF, {-1, -1, -1, -1}, PPC_INST_ISYNC },
    { "sync", 0x7C0004AC, 0xFFFFFFFF, {-1, -1, -1, -1}, PPC_INST_SYNC },
    
    // Compare instructions
    { "cmpw", 0x7C000000, 0xFC400000, {0, 1, 2, -1}, PPC_INST_CMPW },
    { "cmpwi", 0x2C000000, 0xFC400000, {0, 1, 2, -1}, PPC_INST_CMPWI },
    { "cmplw", 0x7C000040, 0xFC400000, {0, 1, 2, -1}, PPC_INST_CMPLW },
    { "cmplwi", 0x28000000, 0xFC400000, {0, 1, 2, -1}, PPC_INST_CMPLWI },
    
    // Simplified aliases
    { "li", 0x38000000, 0xFFE00000, {0, 1, -1, -1}, PPC_INST_LI },
    { "lis", 0x3C000000, 0xFFE00000, {0, 1, -1, -1}, PPC_INST_LIS },
    { "mr", 0x7C000378, 0xFC0007FE, {0, 1, 1, -1}, PPC_INST_OR }, // mr is or rA,rS,rS
    { "nop", 0x60000000, 0xFFFFFFFF, {-1, -1, -1, -1}, PPC_INST_NOP },
    
    // End marker
    { nullptr, 0, 0, {-1, -1, -1, -1}, PPC_INST_INVALID }
};

static uint32_t ExtractOperand(uint32_t instruction, int start, int end) {
    uint32_t mask = (1U << (end - start + 1)) - 1;
    return (instruction >> (31 - end)) & mask;
}

static int32_t SignExtend(uint32_t value, int bits) {
    uint32_t sign_bit = 1U << (bits - 1);
    return (value ^ sign_bit) - sign_bit;
}

const ppc_opcode* GetOpcode(uint32_t instruction) {
    for (const auto* op = opcodes; op->name; op++) {
        if ((instruction & op->mask) == op->opcode) {
            return op;
        }
    }
    return nullptr;
}

bool DecodeInstruction(uint32_t instruction, ppc_insn& insn) {
    insn.instruction = instruction;
    insn.opcode = GetOpcode(instruction);
    
    if (!insn.opcode) {
        strcpy(insn.op_str, "unknown");
        return false;
    }

    // Extract operands based on instruction format
    uint32_t opcode = PPC_OP(instruction);
    
    if (insn.opcode->id == PPC_INST_B || insn.opcode->id == PPC_INST_BA || insn.opcode->id == PPC_INST_BL) {
        // Branch instructions
        int32_t target = SignExtend(ExtractOperand(instruction, 6, 29) << 2, 26);
        insn.operands[0] = target;
    }
    else if (insn.opcode->id == PPC_INST_BC) {
        // Conditional branch
        insn.operands[0] = ExtractOperand(instruction, 6, 10);  // BO
        insn.operands[1] = ExtractOperand(instruction, 11, 15); // BI
        int32_t target = SignExtend(ExtractOperand(instruction, 16, 29) << 2, 16);
        insn.operands[2] = target;
    }
    else if (opcode >= 32 && opcode <= 47) {
        // Load/Store with immediate
        insn.operands[0] = ExtractOperand(instruction, 6, 10);   // rD/rS
        int32_t offset = SignExtend(ExtractOperand(instruction, 16, 31), 16);
        insn.operands[1] = offset;
        insn.operands[2] = ExtractOperand(instruction, 11, 15);  // rA
    }
    else {
        // Generic 3-register format
        insn.operands[0] = ExtractOperand(instruction, 6, 10);   // rD
        insn.operands[1] = ExtractOperand(instruction, 11, 15);  // rA
        insn.operands[2] = ExtractOperand(instruction, 16, 20);  // rB
        insn.operands[3] = ExtractOperand(instruction, 21, 25);  // rC (for 4-operand instructions)
    }

    // Format the instruction string
    FormatInstruction(insn, insn.op_str, sizeof(insn.op_str));
    return true;
}

bool Disassemble(const uint32_t* data, uint32_t address, ppc_insn& insn) {
    uint32_t instruction = ByteSwap(*data);
    return DecodeInstruction(instruction, insn);
}

bool Disassemble(const void* data, size_t offset, uint32_t address, ppc_insn& insn) {
    const uint32_t* ptr = static_cast<const uint32_t*>(data) + (offset / 4);
    return Disassemble(ptr, address, insn);
}

void FormatInstruction(const ppc_insn& insn, char* buffer, size_t buffer_size) {
    if (!insn.opcode || !buffer || buffer_size == 0) {
        if (buffer && buffer_size > 0) {
            strcpy(buffer, "invalid");
        }
        return;
    }

    const char* name = insn.opcode->name;
    snprintf(buffer, buffer_size, "%s", name);
    
    // Add operands
    bool first_operand = true;
    for (int i = 0; i < 4 && insn.opcode->operands[i] != -1; i++) {
        if (first_operand) {
            strncat(buffer, " ", buffer_size - strlen(buffer) - 1);
            first_operand = false;
        } else {
            strncat(buffer, ", ", buffer_size - strlen(buffer) - 1);
        }
        
        char operand_str[32];
        if (insn.opcode->id == PPC_INST_B || insn.opcode->id == PPC_INST_BL || insn.opcode->id == PPC_INST_BC) {
            snprintf(operand_str, sizeof(operand_str), "0x%X", insn.operands[i]);
        } else {
            snprintf(operand_str, sizeof(operand_str), "r%u", insn.operands[i]);
        }
        strncat(buffer, operand_str, buffer_size - strlen(buffer) - 1);
    }
}

const char* GetRegisterName(int reg, char reg_type) {
    static char buffer[8];
    snprintf(buffer, sizeof(buffer), "%c%d", reg_type, reg);
    return buffer;
}

uint32_t CalculateBranchTarget(uint32_t address, const ppc_insn& insn) {
    if (!IsBranchInstruction(insn)) {
        return 0;
    }
    
    if (insn.opcode->id == PPC_INST_BA) {
        return insn.operands[0]; // Absolute address
    } else {
        return address + insn.operands[0]; // Relative address
    }
}

bool IsBranchInstruction(const ppc_insn& insn) {
    if (!insn.opcode) return false;
    
    switch (insn.opcode->id) {
        case PPC_INST_B:
        case PPC_INST_BA:
        case PPC_INST_BL:
        case PPC_INST_BC:
        case PPC_INST_BCLR:
        case PPC_INST_BCCTR:
        case PPC_INST_BLR:
        case PPC_INST_BCTR:
            return true;
        default:
            return false;
    }
}

bool IsConditionalBranch(const ppc_insn& insn) {
    if (!insn.opcode) return false;
    
    switch (insn.opcode->id) {
        case PPC_INST_BC:
        case PPC_INST_BCLR:
        case PPC_INST_BCCTR:
            return true;
        default:
            return false;
    }
}

bool IsUnconditionalBranch(const ppc_insn& insn) {
    return IsBranchInstruction(insn) && !IsConditionalBranch(insn);
}

bool IsLoadInstruction(const ppc_insn& insn) {
    if (!insn.opcode) return false;
    
    switch (insn.opcode->id) {
        case PPC_INST_LWZ:
        case PPC_INST_LWZU:
        case PPC_INST_LWZX:
        case PPC_INST_LBZ:
        case PPC_INST_LBZX:
        case PPC_INST_LHZ:
        case PPC_INST_LHZX:
        case PPC_INST_LFS:
        case PPC_INST_LFD:
        case PPC_INST_PSQ_L:
            return true;
        default:
            return false;
    }
}

bool IsStoreInstruction(const ppc_insn& insn) {
    if (!insn.opcode) return false;
    
    switch (insn.opcode->id) {
        case PPC_INST_STW:
        case PPC_INST_STWU:
        case PPC_INST_STWX:
        case PPC_INST_STB:
        case PPC_INST_STBX:
        case PPC_INST_STH:
        case PPC_INST_STHX:
        case PPC_INST_STFS:
        case PPC_INST_STFD:
        case PPC_INST_PSQ_ST:
            return true;
        default:
            return false;
    }
}

bool IsFloatingPointInstruction(const ppc_insn& insn) {
    if (!insn.opcode) return false;
    
    switch (insn.opcode->id) {
        case PPC_INST_FADD:
        case PPC_INST_FADDS:
        case PPC_INST_FSUB:
        case PPC_INST_FSUBS:
        case PPC_INST_FMUL:
        case PPC_INST_FMULS:
        case PPC_INST_FDIV:
        case PPC_INST_FDIVS:
        case PPC_INST_FMADD:
        case PPC_INST_FMADDS:
        case PPC_INST_LFS:
        case PPC_INST_LFD:
        case PPC_INST_STFS:
        case PPC_INST_STFD:
            return true;
        default:
            return false;
    }
}

bool IsPairedSingleInstruction(const ppc_insn& insn) {
    if (!insn.opcode) return false;
    
    switch (insn.opcode->id) {
        case PPC_INST_PS_ADD:
        case PPC_INST_PS_SUB:
        case PPC_INST_PS_MUL:
        case PPC_INST_PS_DIV:
        case PPC_INST_PS_MADD:
        case PPC_INST_PS_MERGE00:
        case PPC_INST_PS_MERGE01:
        case PPC_INST_PS_MERGE10:
        case PPC_INST_PS_MERGE11:
        case PPC_INST_PSQ_L:
        case PPC_INST_PSQ_ST:
            return true;
        default:
            return false;
    }
}

bool IsPrivilegedInstruction(const ppc_insn& insn) {
    if (!insn.opcode) return false;
    
    switch (insn.opcode->id) {
        case PPC_INST_MFMSR:
        case PPC_INST_MTMSR:
        case PPC_INST_RFI:
        case PPC_INST_SC:
            return true;
        default:
            return false;
    }
}

bool IsGameCubeCompatInstruction(const ppc_insn& insn) {
    // Most instructions are GameCube compatible, 
    // but some WiiU extensions are not
    return !IsPairedSingleInstruction(insn) || 
           (insn.opcode && strstr(insn.opcode->name, "ps_") != nullptr);
}

bool RequiresSpecialHandling(const ppc_insn& insn) {
    // Instructions that might need special handling in recompilation
    return IsPairedSingleInstruction(insn) || 
           IsPrivilegedInstruction(insn) ||
           (insn.opcode && (insn.opcode->id == PPC_INST_SC || 
                           insn.opcode->id == PPC_INST_RFI));
}

}