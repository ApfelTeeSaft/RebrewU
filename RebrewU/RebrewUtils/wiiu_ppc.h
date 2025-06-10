#pragma once

#include <cstdint>
#include <string_view>
#include <string>

#if defined(_MSC_VER)
#include <intrin.h>
#include <stdlib.h>
#endif

// WiiU PowerPC Architecture Definitions
// Based on PowerPC 750 (Gekko/Broadway derivatives)

#if defined(_MSC_VER)
#define WIIU_PACKED __pragma(pack(push, 1))
#define WIIU_PACKED_END __pragma(pack(pop))
#else
#define WIIU_PACKED __attribute__((packed))
#define WIIU_PACKED_END
#endif

// Byte swap macros for cross-platform compatibility
#if defined(_MSC_VER)
#define BSWAP16(x) _byteswap_ushort(x)
#define BSWAP32(x) _byteswap_ulong(x)
#define BSWAP64(x) _byteswap_uint64(x)
#else
#define BSWAP16(x) __builtin_bswap16(x)
#define BSWAP32(x) __builtin_bswap32(x)
#define BSWAP64(x) __builtin_bswap64(x)
#endif

// Endianness utilities for WiiU (big-endian)
template<typename T>
struct be;

template<>
struct be<uint16_t> {
    uint16_t value;
    operator uint16_t() const { return BSWAP16(value); }
    be& operator=(uint16_t v) { value = BSWAP16(v); return *this; }
};

template<>
struct be<uint32_t> {
    uint32_t value;
    operator uint32_t() const { return BSWAP32(value); }
    be& operator=(uint32_t v) { value = BSWAP32(v); return *this; }
};

template<>
struct be<uint64_t> {
    uint64_t value;
    operator uint64_t() const { return BSWAP64(value); }
    be& operator=(uint64_t v) { value = BSWAP64(v); return *this; }
};

// PowerPC Instruction field extraction macros
#define PPC_OP(instr)      (((instr) >> 26) & 0x3F)
#define PPC_XOP(instr)     (((instr) >> 1) & 0x3FF)
#define PPC_BL(instr)      ((instr) & 1)
#define PPC_BA(instr)      (((instr) >> 1) & 1)
#define PPC_BO(instr)      (((instr) >> 21) & 0x1F)
#define PPC_BI(instr)      ((int32_t(instr) << 6) >> 6)
#define PPC_BD(instr)      ((int32_t(instr) << 16) >> 16)

// PowerPC Instruction opcodes (WiiU specific subset)
enum {
    PPC_OP_B = 18,
    PPC_OP_BC = 16,
    PPC_OP_CTR = 19,
    PPC_OP_ADDIC = 12,
    PPC_OP_ADDI = 14,
    PPC_OP_ADDIS = 15,
    PPC_OP_LWZ = 32,
    PPC_OP_STW = 36,
    PPC_OP_LBZ = 34,
    PPC_OP_STB = 38,
    PPC_OP_LHZ = 40,
    PPC_OP_STH = 44,
};

// WiiU PowerPC instruction IDs
enum ppc_insn_id {
    PPC_INST_INVALID = 0,
    PPC_INST_ADD,
    PPC_INST_ADDC,
    PPC_INST_ADDE,
    PPC_INST_ADDI,
    PPC_INST_ADDIC,
    PPC_INST_ADDIS,
    PPC_INST_ADDME,
    PPC_INST_ADDZE,
    PPC_INST_AND,
    PPC_INST_ANDC,
    PPC_INST_ANDI,
    PPC_INST_ANDIS,
    PPC_INST_B,
    PPC_INST_BA,
    PPC_INST_BC,
    PPC_INST_BCCTR,
    PPC_INST_BCLR,
    PPC_INST_BCTR,
    PPC_INST_BDZ,
    PPC_INST_BDZL,
    PPC_INST_BDNZ,
    PPC_INST_BDNZL,
    PPC_INST_BEQ,
    PPC_INST_BEQL,
    PPC_INST_BGE,
    PPC_INST_BGEL,
    PPC_INST_BGT,
    PPC_INST_BGTL,
    PPC_INST_BL,
    PPC_INST_BLE,
    PPC_INST_BLEL,
    PPC_INST_BLR,
    PPC_INST_BLRL,
    PPC_INST_BLT,
    PPC_INST_BLTL,
    PPC_INST_BNE,
    PPC_INST_BNEL,
    PPC_INST_BNS,
    PPC_INST_BNSL,
    PPC_INST_BSO,
    PPC_INST_BSOL,
    PPC_INST_CLRLWI,
    PPC_INST_CMPW,
    PPC_INST_CMPWI,
    PPC_INST_CMPLW,
    PPC_INST_CMPLWI,
    PPC_INST_CNTLZW,
    PPC_INST_CROR,
    PPC_INST_DCBF,
    PPC_INST_DCBI,
    PPC_INST_DCBST,
    PPC_INST_DCBT,
    PPC_INST_DCBTST,
    PPC_INST_DCBZ,
    PPC_INST_DIVW,
    PPC_INST_DIVWU,
    PPC_INST_EIEIO,
    PPC_INST_EQV,
    PPC_INST_EXTSB,
    PPC_INST_EXTSH,
    PPC_INST_FABS,
    PPC_INST_FADD,
    PPC_INST_FADDS,
    PPC_INST_FCMPO,
    PPC_INST_FCMPU,
    PPC_INST_FCTIW,
    PPC_INST_FCTIWZ,
    PPC_INST_FDIV,
    PPC_INST_FDIVS,
    PPC_INST_FMADD,
    PPC_INST_FMADDS,
    PPC_INST_FMR,
    PPC_INST_FMSUB,
    PPC_INST_FMSUBS,
    PPC_INST_FMUL,
    PPC_INST_FMULS,
    PPC_INST_FNABS,
    PPC_INST_FNEG,
    PPC_INST_FNMADD,
    PPC_INST_FNMADDS,
    PPC_INST_FNMSUB,
    PPC_INST_FNMSUBS,
    PPC_INST_FRES,
    PPC_INST_FRSP,
    PPC_INST_FRSQRTE,
    PPC_INST_FSEL,
    PPC_INST_FSQRT,
    PPC_INST_FSQRTS,
    PPC_INST_FSUB,
    PPC_INST_FSUBS,
    PPC_INST_ICBI,
    PPC_INST_ISYNC,
    PPC_INST_LBZ,
    PPC_INST_LBZU,
    PPC_INST_LBZUX,
    PPC_INST_LBZX,
    PPC_INST_LFD,
    PPC_INST_LFDU,
    PPC_INST_LFDUX,
    PPC_INST_LFDX,
    PPC_INST_LFS,
    PPC_INST_LFSU,
    PPC_INST_LFSUX,
    PPC_INST_LFSX,
    PPC_INST_LHA,
    PPC_INST_LHAU,
    PPC_INST_LHAUX,
    PPC_INST_LHAX,
    PPC_INST_LHBRX,
    PPC_INST_LHZ,
    PPC_INST_LHZU,
    PPC_INST_LHZUX,
    PPC_INST_LHZX,
    PPC_INST_LI,
    PPC_INST_LIS,
    PPC_INST_LMW,
    PPC_INST_LSWI,
    PPC_INST_LSWX,
    PPC_INST_LWARX,
    PPC_INST_LWBRX,
    PPC_INST_LWZ,
    PPC_INST_LWZU,
    PPC_INST_LWZUX,
    PPC_INST_LWZX,
    PPC_INST_MCRF,
    PPC_INST_MCRFS,
    PPC_INST_MFCR,
    PPC_INST_MFFS,
    PPC_INST_MFLR,
    PPC_INST_MFMSR,
    PPC_INST_MFSPR,
    PPC_INST_MFSR,
    PPC_INST_MFSRIN,
    PPC_INST_MFTB,
    PPC_INST_MTCRF,
    PPC_INST_MTFSB0,
    PPC_INST_MTFSB1,
    PPC_INST_MTFSF,
    PPC_INST_MTFSFI,
    PPC_INST_MTLR,
    PPC_INST_MTMSR,
    PPC_INST_MTSPR,
    PPC_INST_MTSR,
    PPC_INST_MTSRIN,
    PPC_INST_MULHW,
    PPC_INST_MULHWU,
    PPC_INST_MULLI,
    PPC_INST_MULLW,
    PPC_INST_NAND,
    PPC_INST_NEG,
    PPC_INST_NOP,
    PPC_INST_NOR,
    PPC_INST_OR,
    PPC_INST_ORC,
    PPC_INST_ORI,
    PPC_INST_ORIS,
    PPC_INST_RFI,
    PPC_INST_RLWIMI,
    PPC_INST_RLWINM,
    PPC_INST_RLWNM,
    PPC_INST_SC,
    PPC_INST_SLW,
    PPC_INST_SRAW,
    PPC_INST_SRAWI,
    PPC_INST_SRW,
    PPC_INST_STB,
    PPC_INST_STBU,
    PPC_INST_STBUX,
    PPC_INST_STBX,
    PPC_INST_STFD,
    PPC_INST_STFDU,
    PPC_INST_STFDUX,
    PPC_INST_STFDX,
    PPC_INST_STFIWX,
    PPC_INST_STFS,
    PPC_INST_STFSU,
    PPC_INST_STFSUX,
    PPC_INST_STFSX,
    PPC_INST_STH,
    PPC_INST_STHBRX,
    PPC_INST_STHU,
    PPC_INST_STHUX,
    PPC_INST_STHX,
    PPC_INST_STMW,
    PPC_INST_STSWI,
    PPC_INST_STSWX,
    PPC_INST_STW,
    PPC_INST_STWBRX,
    PPC_INST_STWCX,
    PPC_INST_STWU,
    PPC_INST_STWUX,
    PPC_INST_STWX,
    PPC_INST_SUBF,
    PPC_INST_SUBFC,
    PPC_INST_SUBFE,
    PPC_INST_SUBFIC,
    PPC_INST_SUBFME,
    PPC_INST_SUBFZE,
    PPC_INST_SYNC,
    PPC_INST_TW,
    PPC_INST_TWI,
    PPC_INST_XOR,
    PPC_INST_XORI,
    PPC_INST_XORIS,
    // Paired single instructions (WiiU specific)
    PPC_INST_PS_ABS,
    PPC_INST_PS_ADD,
    PPC_INST_PS_CMPO0,
    PPC_INST_PS_CMPO1,
    PPC_INST_PS_CMPU0,
    PPC_INST_PS_CMPU1,
    PPC_INST_PS_DIV,
    PPC_INST_PS_MADD,
    PPC_INST_PS_MADDS0,
    PPC_INST_PS_MADDS1,
    PPC_INST_PS_MERGE00,
    PPC_INST_PS_MERGE01,
    PPC_INST_PS_MERGE10,
    PPC_INST_PS_MERGE11,
    PPC_INST_PS_MR,
    PPC_INST_PS_MSUB,
    PPC_INST_PS_MUL,
    PPC_INST_PS_MULS0,
    PPC_INST_PS_MULS1,
    PPC_INST_PS_NABS,
    PPC_INST_PS_NEG,
    PPC_INST_PS_NMADD,
    PPC_INST_PS_NMSUB,
    PPC_INST_PS_RES,
    PPC_INST_PS_RSQRTE,
    PPC_INST_PS_SEL,
    PPC_INST_PS_SUB,
    PPC_INST_PS_SUM0,
    PPC_INST_PS_SUM1,
    PPC_INST_PSQ_L,
    PPC_INST_PSQ_LU,
    PPC_INST_PSQ_LX,
    PPC_INST_PSQ_LUX,
    PPC_INST_PSQ_ST,
    PPC_INST_PSQ_STU,
    PPC_INST_PSQ_STX,
    PPC_INST_PSQ_STUX,
    PPC_INST_COUNT
};

// Instruction structure
struct ppc_opcode {
    const char* name;
    uint32_t opcode;
    uint32_t mask;
    int operands[4];
    ppc_insn_id id;
};

struct ppc_insn {
    uint32_t instruction;
    const ppc_opcode* opcode;
    uint32_t operands[4];
    char op_str[256];
};

// Symbol types
enum SymbolType : uint32_t {
    Symbol_Function = 1,
    Symbol_Data = 2,
    Symbol_Object = 3,
};

// Symbol structure
struct Symbol {
    std::string name;
    uint32_t address;
    uint32_t size;
    SymbolType type;

    Symbol() = default;
    Symbol(std::string name, uint32_t address, uint32_t size, SymbolType type)
        : name(std::move(name)), address(address), size(size), type(type) {
    }
};

// Section flags
enum SectionFlags : uint32_t {
    SectionFlags_Code = 0x20,
    SectionFlags_Data = 0x40,
    SectionFlags_BSS = 0x80,
};

// Utility functions
inline uint32_t ByteSwap(uint32_t value) {
    return BSWAP32(value);
}

inline uint16_t ByteSwap(uint16_t value) {
    return BSWAP16(value);
}

inline uint64_t ByteSwap(uint64_t value) {
    return BSWAP64(value);
}