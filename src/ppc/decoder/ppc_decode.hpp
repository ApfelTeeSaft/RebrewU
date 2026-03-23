#pragma once

#include "../instructions/ppc_fields.hpp"
#include <cstdint>
#include <string>
#include <optional>
#include <span>
#include <vector>

// ============================================================================
// RebrewU — Wii U static recompilation framework
// ppc_decode.hpp — PowerPC Espresso (750CL) instruction decoder
//
// Decodes a 32-bit PPC instruction word into an `Instruction` structure that
// names the opcode, identifies operands, and pre-computes commonly-needed
// values (sign-extended immediates, branch targets, etc.).
// ============================================================================

namespace rebrewu::ppc {

// ============================================================================
// PPC instruction categories
// ============================================================================

enum class InstrClass : uint8_t {
    Unknown,
    Integer,       // arithmetic / logical / compare
    Float,         // floating-point
    Load,          // memory load
    Store,         // memory store
    LoadMultiple,  // lmw
    StoreMultiple, // stmw
    Branch,        // b / bc / bclr / bcctr
    System,        // sc / rfi / sync / isync
    CacheControl,  // dcbf / dcbi / icbi / dcbst / dcbz / ...
    Rotate,        // rlwinm / rlwimi / rlwnm
    AltiVec,       // Altivec/VMX (unused on Espresso but present in binaries)
    Paired,        // Paired Singles (ps_*)
};

// ============================================================================
// Instruction mnemonic
// ============================================================================

enum class Mnemonic : uint16_t {
    UNKNOWN = 0,

    // Integer arithmetic
    ADD, ADDO, ADD_, ADDO_,
    ADDC, ADDCO, ADDC_, ADDCO_,
    ADDE, ADDEO, ADDE_, ADDEO_,
    ADDI, ADDIC, ADDIC_, ADDIS,
    ADDME, ADDMEO, ADDME_, ADDMEO_,
    ADDZE, ADDZEO, ADDZE_, ADDZEO_,
    SUBF, SUBFO, SUBF_, SUBFO_,
    SUBFC, SUBFCO, SUBFC_, SUBFCO_,
    SUBFE, SUBFEO, SUBFE_, SUBFEO_,
    SUBFIC,
    SUBFME, SUBFMEO, SUBFME_, SUBFMEO_,
    SUBFZE, SUBFZEO, SUBFZE_, SUBFZEO_,
    NEG, NEGO, NEG_, NEGO_,
    MULLI, MULLW, MULLWO, MULLW_, MULLWO_,
    MULHW, MULHW_, MULHWU, MULHWU_,
    DIVW, DIVWO, DIVW_, DIVWO_,
    DIVWU, DIVWUO, DIVWU_, DIVWUO_,

    // Integer logical
    AND, AND_, ANDC, ANDC_,
    OR, OR_, ORC, ORC_,
    XOR, XOR_,
    NAND, NAND_, NOR, NOR_,
    EQV, EQV_,
    ANDI_, ANDIS_,
    ORI, ORIS,
    XORI, XORIS,
    EXTSB, EXTSB_, EXTSH, EXTSH_,
    CNTLZW, CNTLZW_,

    // Rotate / shift
    RLWINM, RLWINM_, RLWIMI, RLWIMI_, RLWNM, RLWNM_,
    SLW, SLW_, SRW, SRW_, SRAW, SRAW_, SRAWI, SRAWI_,

    // Compare
    CMP, CMPI, CMPL, CMPLI,
    FCMPU, FCMPO,

    // Condition register
    CRAND, CRANDC, CROR, CRORC, CRXOR, CRNAND, CRNOR, CREQV,
    MCRF, MCRFS, MCRXR,
    MTCRF, MFCR,

    // Branch
    B, BL, BA, BLA,
    BC, BCL, BCA, BCLA,
    BLR, BLRL, BCTR, BCTRL,
    BCLR, BCLRL,

    // Trap
    TW, TWI,

    // Special purpose registers
    MTSPR, MFSPR, MFMSR, MTMSR,
    MTLR, MFLR, MTCTR, MFCTR,
    MTXER, MFXER,

    // Memory: integer load
    LBZ, LBZX, LBZU, LBZUX,
    LHZ, LHZX, LHZU, LHZUX,
    LHA, LHAX, LHAU, LHAUX,
    LWZ, LWZX, LWZU, LWZUX,
    LHBRX, LWBRX,
    LMW,

    // Memory: integer store
    STB, STBX, STBU, STBUX,
    STH, STHX, STHU, STHUX,
    STW, STWX, STWU, STWUX,
    STHBRX, STWBRX,
    STMW,

    // Memory: float load / store
    LFS, LFSX, LFSU, LFSUX,
    LFD, LFDX, LFDU, LFDUX,
    STFS, STFSX, STFSU, STFSUX,
    STFD, STFDX, STFDU, STFDUX,
    STFIWX,

    // Float arithmetic
    FADD, FADDS, FSUB, FSUBS,
    FMUL, FMULS, FDIV, FDIVS,
    FMADD, FMADDS, FMSUB, FMSUBS,
    FNMADD, FNMADDS, FNMSUB, FNMSUBS,
    FSQRT, FSQRTS, FRES, FRSQRTE, FSEL,
    FRSP, FCTIW, FCTIWZ,
    FABS, FABS_, FNABS, FNABS_, FNEG, FNEG_, FMR, FMR_,
    FMOVS, FMOVD,

    // System
    SC, RFI, ISYNC, SYNC, EIEIO, TLBIA, TLBIE, TLBSYNC,

    // Cache
    DCBF, DCBI, DCBST, DCBT, DCBTST, DCBZ, ICBI,

    // Misc
    NOP,
    LI, LIS,       // synthetic: addi/addis with ra=0
    MR, MR_,       // synthetic: or rD,rA,rA
    NOT, NOT_,     // synthetic: nor rD,rA,rA

    // Paired Singles (subset used by Wii U games)
    PS_ABS, PS_ADD, PS_CMPO0, PS_CMPO1, PS_CMPU0, PS_CMPU1,
    PS_DIV, PS_MADD, PS_MADDS0, PS_MADDS1,
    PS_MERGE00, PS_MERGE01, PS_MERGE10, PS_MERGE11,
    PS_MOV, PS_MUL, PS_MULS0, PS_MULS1,
    PS_NABS, PS_NEG, PS_NMADD, PS_NMADDS0, PS_NMADDS1,
    PS_NMSUB, PS_NMSUBS0, PS_NMSUBS1,
    PS_RES, PS_RSQRTE, PS_SEL,
    PS_SUB, PS_SUM0, PS_SUM1,
    PSQ_L, PSQ_LU, PSQ_LX, PSQ_LUX,
    PSQ_ST, PSQ_STU, PSQ_STX, PSQ_STUX,
};

// ============================================================================
// Decoded instruction
// ============================================================================

struct Instruction {
    uint32_t  addr{0};        // guest PC of this instruction
    uint32_t  word{0};        // raw instruction word (big-endian, as in the binary)
    Mnemonic  mnemonic{Mnemonic::UNKNOWN};
    InstrClass iclass{InstrClass::Unknown};

    // Common field values (decoded from the instruction word)
    uint8_t  rD{0}, rA{0}, rB{0}, rC{0}; // GPR / FPR operands
    uint8_t  crf{0};          // condition register field destination
    int32_t  imm{0};          // sign-extended 16-bit immediate / offset
    uint32_t uimm{0};         // zero-extended 16-bit immediate
    uint32_t target{0};       // branch target (absolute address after resolution)
    int32_t  branch_offset{0};// branch displacement (signed, as in the insn)
    bool     aa{false};       // absolute address bit
    bool     lk{false};       // link bit (sets LR = PC+4)
    bool     oe{false};       // overflow enable bit
    bool     rc{false};       // record bit (sets CR0)
    uint8_t  spr_num{0};      // SPR number (0..1023)
    uint8_t  sh{0}, mb{0}, me{0}; // rotate/shift fields

    // Branch condition encoding
    uint8_t  bo{0}, bi{0};

    // -----------------------------------------------------------------
    // Derived helpers
    // -----------------------------------------------------------------

    bool is_branch()     const noexcept { return iclass == InstrClass::Branch; }
    bool is_load()       const noexcept { return iclass == InstrClass::Load; }
    bool is_store()      const noexcept { return iclass == InstrClass::Store; }
    bool is_float()      const noexcept { return iclass == InstrClass::Float; }
    bool is_nop()        const noexcept { return mnemonic == Mnemonic::NOP; }

    bool is_unconditional_branch() const noexcept {
        return mnemonic == Mnemonic::B  || mnemonic == Mnemonic::BA  ||
               mnemonic == Mnemonic::BL || mnemonic == Mnemonic::BLA ||
               mnemonic == Mnemonic::BCTR || mnemonic == Mnemonic::BCTRL ||
               mnemonic == Mnemonic::BLR  || mnemonic == Mnemonic::BLRL;
    }

    bool is_call() const noexcept {
        return lk;
    }

    bool is_return() const noexcept {
        return mnemonic == Mnemonic::BLR || mnemonic == Mnemonic::BLRL;
    }

    bool is_indirect_branch() const noexcept {
        return mnemonic == Mnemonic::BCTR || mnemonic == Mnemonic::BCTRL ||
               mnemonic == Mnemonic::BLR  || mnemonic == Mnemonic::BLRL  ||
               mnemonic == Mnemonic::BCLR || mnemonic == Mnemonic::BCLRL;
    }
};

// ============================================================================
// Decoder interface
// ============================================================================

/// Decode a single 32-bit PPC instruction word at guest address `pc`.
/// Returns nullopt for words that cannot be decoded (truly invalid encodings).
std::optional<Instruction> decode(uint32_t word, uint32_t pc) noexcept;

/// Decode a sequence of instructions from a big-endian byte span.
/// `base_addr` is the guest address of the first byte.
std::vector<Instruction> decode_block(std::span<const uint8_t> bytes,
                                       uint32_t base_addr);

/// Return a human-readable disassembly string for an instruction.
std::string disassemble(const Instruction& insn);

}