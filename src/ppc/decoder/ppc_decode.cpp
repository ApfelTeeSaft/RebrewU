#include "ppc_decode.hpp"
#include <cstring>
#include <sstream>
#include <iomanip>

namespace rebrewu::ppc {

// ============================================================================
// Primary opcode table (bits 0-5 of the instruction word)
// ============================================================================

static Mnemonic classify_primary(uint32_t word, Instruction& out) noexcept {
    using namespace rebrewu::ppc;
    const uint32_t op = ppc_field(word, 0, 5);

    out.rD  = static_cast<uint8_t>(ppc_field(word, 6, 10));
    out.rA  = static_cast<uint8_t>(ppc_field(word, 11, 15));
    out.rB  = static_cast<uint8_t>(ppc_field(word, 16, 20));
    out.imm = sign_extend_16(ppc_field(word, 16, 31));
    out.uimm= ppc_field(word, 16, 31);
    out.rc  = ppc_bit(word, 31) != 0;
    out.oe  = ppc_bit(word, 21) != 0;

    switch (op) {
    case 10: out.iclass = InstrClass::Integer;  return Mnemonic::CMPLI;
    case 11: out.iclass = InstrClass::Integer;  return Mnemonic::CMPI;
    case 12: out.iclass = InstrClass::Integer;  return Mnemonic::ADDIC;
    case 13: out.iclass = InstrClass::Integer;  return Mnemonic::ADDIC_;
    case 14: out.iclass = InstrClass::Integer;
             return out.rA == 0 ? Mnemonic::LI : Mnemonic::ADDI;
    case 15: out.iclass = InstrClass::Integer;
             return out.rA == 0 ? Mnemonic::LIS : Mnemonic::ADDIS;
    case 16: {  // bc
        out.iclass = InstrClass::Branch;
        out.bo = out.rD; out.bi = out.rA;
        out.aa  = ppc_bit(word, 30) != 0;
        out.lk  = ppc_bit(word, 31) != 0;
        int32_t bd = sign_extend(ppc_field(word, 16, 29) << 2, 16);
        out.branch_offset = bd;
        out.target = out.aa ? static_cast<uint32_t>(bd)
                            : out.addr + static_cast<uint32_t>(bd);
        if (out.aa && out.lk) return Mnemonic::BCLA;
        if (out.aa)           return Mnemonic::BCA;
        if (out.lk)           return Mnemonic::BCL;
        return Mnemonic::BC;
    }
    case 18: {  // b
        out.iclass = InstrClass::Branch;
        out.aa = ppc_bit(word, 30) != 0;
        out.lk = ppc_bit(word, 31) != 0;
        int32_t li = sign_extend(ppc_field(word, 6, 29) << 2, 26);
        out.branch_offset = li;
        out.target = out.aa ? static_cast<uint32_t>(li)
                            : out.addr + static_cast<uint32_t>(li);
        if (out.aa && out.lk) return Mnemonic::BLA;
        if (out.aa)           return Mnemonic::BA;
        if (out.lk)           return Mnemonic::BL;
        return Mnemonic::B;
    }
    case 20: out.iclass = InstrClass::Rotate;
             out.sh = out.rB; out.mb = static_cast<uint8_t>(ppc_field(word,21,25));
             out.me = static_cast<uint8_t>(ppc_field(word,26,30));
             return out.rc ? Mnemonic::RLWIMI_ : Mnemonic::RLWIMI;
    case 21: out.iclass = InstrClass::Rotate;
             out.sh = out.rB; out.mb = static_cast<uint8_t>(ppc_field(word,21,25));
             out.me = static_cast<uint8_t>(ppc_field(word,26,30));
             return out.rc ? Mnemonic::RLWINM_ : Mnemonic::RLWINM;
    case 23: out.iclass = InstrClass::Rotate;
             out.mb = static_cast<uint8_t>(ppc_field(word,21,25));
             out.me = static_cast<uint8_t>(ppc_field(word,26,30));
             return out.rc ? Mnemonic::RLWNM_ : Mnemonic::RLWNM;
    case 24: out.iclass = InstrClass::Integer;  return Mnemonic::ORI;
    case 25: out.iclass = InstrClass::Integer;  return Mnemonic::ORIS;
    case 26: out.iclass = InstrClass::Integer;  return Mnemonic::XORI;
    case 27: out.iclass = InstrClass::Integer;  return Mnemonic::XORIS;
    case 28: out.iclass = InstrClass::Integer;  return Mnemonic::ANDI_;
    case 29: out.iclass = InstrClass::Integer;  return Mnemonic::ANDIS_;
    case 32: out.iclass = InstrClass::Load;     return Mnemonic::LWZ;
    case 33: out.iclass = InstrClass::Load;     return Mnemonic::LWZU;
    case 34: out.iclass = InstrClass::Load;     return Mnemonic::LBZ;
    case 35: out.iclass = InstrClass::Load;     return Mnemonic::LBZU;
    case 36: out.iclass = InstrClass::Store;    return Mnemonic::STW;
    case 37: out.iclass = InstrClass::Store;    return Mnemonic::STWU;
    case 38: out.iclass = InstrClass::Store;    return Mnemonic::STB;
    case 39: out.iclass = InstrClass::Store;    return Mnemonic::STBU;
    case 40: out.iclass = InstrClass::Load;     return Mnemonic::LHZ;
    case 41: out.iclass = InstrClass::Load;     return Mnemonic::LHZU;
    case 42: out.iclass = InstrClass::Load;     return Mnemonic::LHA;
    case 43: out.iclass = InstrClass::Load;     return Mnemonic::LHAU;
    case 44: out.iclass = InstrClass::Store;    return Mnemonic::STH;
    case 45: out.iclass = InstrClass::Store;    return Mnemonic::STHU;
    case 46: out.iclass = InstrClass::LoadMultiple;  return Mnemonic::LMW;
    case 47: out.iclass = InstrClass::StoreMultiple; return Mnemonic::STMW;
    case 48: out.iclass = InstrClass::Load;     return Mnemonic::LFS;
    case 49: out.iclass = InstrClass::Load;     return Mnemonic::LFSU;
    case 50: out.iclass = InstrClass::Load;     return Mnemonic::LFD;
    case 51: out.iclass = InstrClass::Load;     return Mnemonic::LFDU;
    case 52: out.iclass = InstrClass::Store;    return Mnemonic::STFS;
    case 53: out.iclass = InstrClass::Store;    return Mnemonic::STFSU;
    case 54: out.iclass = InstrClass::Store;    return Mnemonic::STFD;
    case 55: out.iclass = InstrClass::Store;    return Mnemonic::STFDU;
    default: return Mnemonic::UNKNOWN;
    }
}

// ============================================================================
// Extended opcode 19 (branch register instructions)
// ============================================================================

static Mnemonic decode_op19(uint32_t word, Instruction& out) noexcept {
    out.iclass = InstrClass::Branch;
    out.bo = static_cast<uint8_t>(ppc_field(word, 6, 10));
    out.bi = static_cast<uint8_t>(ppc_field(word, 11, 15));
    out.lk = ppc_bit(word, 31) != 0;
    uint32_t xop = ppc_field(word, 21, 30);
    switch (xop) {
    case 16:
        // bo==20 means unconditional (BLR / BLRL); anything else is conditional (BCLR / BCLRL)
        if (out.bo == 20)
            return out.lk ? Mnemonic::BLRL : Mnemonic::BLR;
        return out.lk ? Mnemonic::BCLRL : Mnemonic::BCLR;
    case 528: return out.lk ? Mnemonic::BCTRL : Mnemonic::BCTR;
    case 0:   out.iclass = InstrClass::Integer; return Mnemonic::MCRF;
    default:  return Mnemonic::UNKNOWN;
    }
}

// ============================================================================
// Extended opcode 31 (integer register-register)
// ============================================================================

static Mnemonic decode_op31(uint32_t word, Instruction& out) noexcept {
    out.iclass = InstrClass::Integer;
    out.rD  = static_cast<uint8_t>(ppc_field(word, 6, 10));
    out.rA  = static_cast<uint8_t>(ppc_field(word, 11, 15));
    out.rB  = static_cast<uint8_t>(ppc_field(word, 16, 20));
    out.oe  = ppc_bit(word, 21) != 0;
    out.rc  = ppc_bit(word, 31) != 0;
    uint32_t xop = ppc_field(word, 22, 30);

    switch (xop) {
    case 0:   out.iclass = InstrClass::Integer; return Mnemonic::CMP;
    case 8:   return out.oe ? (out.rc ? Mnemonic::SUBFC_  : Mnemonic::SUBFC )
                            : (out.rc ? Mnemonic::SUBFC_  : Mnemonic::SUBFC );
    case 10:  return out.oe ? Mnemonic::ADDCO_ : Mnemonic::ADDC;
    case 20:  out.iclass = InstrClass::Load; return Mnemonic::LWZX; // LWARX mapped to LWZX for now
    case 23:  out.iclass = InstrClass::Load;   return Mnemonic::LWZX;
    case 24:  return out.rc ? Mnemonic::SLW_ : Mnemonic::SLW;
    case 26:  return out.rc ? Mnemonic::CNTLZW_ : Mnemonic::CNTLZW;
    case 28:  return out.rc ? Mnemonic::AND_ : Mnemonic::AND;
    case 32:  out.iclass = InstrClass::Integer; return Mnemonic::CMPL;
    case 40:  return out.oe ? (out.rc ? Mnemonic::SUBFO_  : Mnemonic::SUBFO )
                            : (out.rc ? Mnemonic::SUBF_   : Mnemonic::SUBF  );
    case 55:  out.iclass = InstrClass::Load;   return Mnemonic::LWZUX;
    case 60:  return out.rc ? Mnemonic::ANDC_ : Mnemonic::ANDC;
    case 75:  return out.rc ? Mnemonic::MULHW_ : Mnemonic::MULHW;
    case 87:  out.iclass = InstrClass::Load;   return Mnemonic::LBZX;
    case 104: return out.oe ? Mnemonic::NEGO_ : (out.rc ? Mnemonic::NEG_ : Mnemonic::NEG);
    case 119: out.iclass = InstrClass::Load;   return Mnemonic::LBZUX;
    case 124: return out.rc ? Mnemonic::NOR_ : Mnemonic::NOR;
    case 136: return out.oe ? Mnemonic::SUBFEO_ : (out.rc ? Mnemonic::SUBFE_ : Mnemonic::SUBFE);
    case 138: return out.oe ? Mnemonic::ADDEO_  : (out.rc ? Mnemonic::ADDE_  : Mnemonic::ADDE);
    case 144: out.iclass = InstrClass::Integer; return Mnemonic::MTCRF;
    case 146: out.iclass = InstrClass::System;  return Mnemonic::MTMSR;
    case 151: out.iclass = InstrClass::Store;   return Mnemonic::STWX;
    case 183: out.iclass = InstrClass::Store;   return Mnemonic::STWUX;
    case 200: return out.oe ? Mnemonic::SUBFZEO_ : (out.rc ? Mnemonic::SUBFZE_ : Mnemonic::SUBFZE);
    case 202: return out.oe ? Mnemonic::ADDZEO_  : (out.rc ? Mnemonic::ADDZE_  : Mnemonic::ADDZE);
    case 215: out.iclass = InstrClass::Store;   return Mnemonic::STBX;
    case 232: return out.oe ? Mnemonic::SUBFMEO_ : (out.rc ? Mnemonic::SUBFME_ : Mnemonic::SUBFME);
    case 234: return out.oe ? Mnemonic::ADDMEO_  : (out.rc ? Mnemonic::ADDME_  : Mnemonic::ADDME);
    case 235: return out.oe ? Mnemonic::MULLWO_  : (out.rc ? Mnemonic::MULLW_  : Mnemonic::MULLW);
    case 247: out.iclass = InstrClass::Store;   return Mnemonic::STBUX;
    case 266: return out.oe ? Mnemonic::ADDO_   : (out.rc ? Mnemonic::ADD_    : Mnemonic::ADD);
    case 279: out.iclass = InstrClass::Load;    return Mnemonic::LHZX;
    case 284: return out.rc ? Mnemonic::EQV_ : Mnemonic::EQV;
    case 311: out.iclass = InstrClass::Load;    return Mnemonic::LHZUX;
    case 316: return out.rc ? Mnemonic::XOR_ : Mnemonic::XOR;
    case 339: {   // mfspr
        out.iclass = InstrClass::Integer;
        // SPR encoding: spr[4:0] in bits 11-15, spr[9:5] in bits 16-20
        uint32_t spr_raw = ppc_field(word,11,15) | (ppc_field(word,16,20) << 5);
        out.spr_num = static_cast<uint8_t>(spr_raw & 0xFF);
        if (spr_raw == 8)  return Mnemonic::MFLR;
        if (spr_raw == 9)  return Mnemonic::MFCTR;
        if (spr_raw == 1)  return Mnemonic::MFXER;
        return Mnemonic::MFSPR;
    }
    case 343: out.iclass = InstrClass::Load;    return Mnemonic::LHAX;
    case 375: out.iclass = InstrClass::Load;    return Mnemonic::LHAUX;
    case 407: out.iclass = InstrClass::Store;   return Mnemonic::STHX;
    case 412: return out.rc ? Mnemonic::ORC_ : Mnemonic::ORC;
    case 439: out.iclass = InstrClass::Store;   return Mnemonic::STHUX;
    case 444: {
        // or rA, rS, rB — might be MR if rS == rB
        out.iclass = InstrClass::Integer;
        if (out.rD == out.rB) return out.rc ? Mnemonic::MR_ : Mnemonic::MR;
        return out.rc ? Mnemonic::OR_ : Mnemonic::OR;
    }
    case 459: return out.oe ? Mnemonic::DIVWUO_ : (out.rc ? Mnemonic::DIVWU_ : Mnemonic::DIVWU);
    case 467: {   // mtspr
        out.iclass = InstrClass::Integer;
        // SPR encoding: spr[4:0] in bits 11-15, spr[9:5] in bits 16-20
        uint32_t spr_raw = ppc_field(word,11,15) | (ppc_field(word,16,20) << 5);
        out.spr_num = static_cast<uint8_t>(spr_raw & 0xFF);
        if (spr_raw == 8)  return Mnemonic::MTLR;
        if (spr_raw == 9)  return Mnemonic::MTCTR;
        if (spr_raw == 1)  return Mnemonic::MTXER;
        return Mnemonic::MTSPR;
    }
    case 476: {
        out.iclass = InstrClass::Integer;
        if (out.rD == out.rB) return out.rc ? Mnemonic::NOT_ : Mnemonic::NOT;
        return out.rc ? Mnemonic::NAND_ : Mnemonic::NAND;
    }
    case 491: return out.oe ? Mnemonic::DIVWO_  : (out.rc ? Mnemonic::DIVW_  : Mnemonic::DIVW);
    case 512: out.iclass = InstrClass::Integer; return Mnemonic::MCRXR;
    case 535: out.iclass = InstrClass::Load;    return Mnemonic::LFSX;
    case 536: return out.rc ? Mnemonic::SRW_ : Mnemonic::SRW;
    case 567: out.iclass = InstrClass::Load;    return Mnemonic::LFSUX;
    case 597: out.iclass = InstrClass::LoadMultiple; return Mnemonic::LMW; // LSWI mapped to LMW
    case 599: out.iclass = InstrClass::Load;    return Mnemonic::LFDX;
    case 631: out.iclass = InstrClass::Load;    return Mnemonic::LFDUX;
    case 663: out.iclass = InstrClass::Store;   return Mnemonic::STFSX;
    case 695: out.iclass = InstrClass::Store;   return Mnemonic::STFSUX;
    case 727: out.iclass = InstrClass::Store;   return Mnemonic::STFDX;
    case 759: out.iclass = InstrClass::Store;   return Mnemonic::STFDUX;
    case 792: return out.rc ? Mnemonic::SRAW_ : Mnemonic::SRAW;
    case 824: {
        out.sh = static_cast<uint8_t>(ppc_field(word, 16, 20));
        return out.rc ? Mnemonic::SRAWI_ : Mnemonic::SRAWI;
    }
    case 854: out.iclass = InstrClass::System;  return Mnemonic::EIEIO;
    case 922: return out.rc ? Mnemonic::EXTSH_ : Mnemonic::EXTSH;
    case 954: return out.rc ? Mnemonic::EXTSB_ : Mnemonic::EXTSB;
    case 982: out.iclass = InstrClass::CacheControl; return Mnemonic::ICBI;
    case 983: out.iclass = InstrClass::Store;   return Mnemonic::STFIWX;
    case 1014:out.iclass = InstrClass::CacheControl; return Mnemonic::DCBZ;
    default:  return Mnemonic::UNKNOWN;
    }
}

// ============================================================================
// Extended opcode 59 (single-precision floating point — A-form)
// ============================================================================

static Mnemonic decode_op59(uint32_t word, Instruction& out) noexcept {
    out.iclass = InstrClass::Float;
    out.rD = static_cast<uint8_t>(ppc_field(word, 6, 10));
    out.rA = static_cast<uint8_t>(ppc_field(word, 11, 15));
    out.rB = static_cast<uint8_t>(ppc_field(word, 16, 20));
    out.rC = static_cast<uint8_t>(ppc_field(word, 21, 25));
    out.rc = ppc_bit(word, 31) != 0;
    uint32_t xop5 = ppc_field(word, 26, 30);
    switch (xop5) {
    case 18: return Mnemonic::FDIVS;
    case 20: return Mnemonic::FSUBS;
    case 21: return Mnemonic::FADDS;
    case 22: return Mnemonic::FSQRTS;
    case 24: return Mnemonic::FRES;
    case 25: return Mnemonic::FMULS;
    case 26: return Mnemonic::FRSQRTE;
    case 28: return Mnemonic::FMSUBS;
    case 29: return Mnemonic::FMADDS;
    case 30: return Mnemonic::FNMSUBS;
    case 31: return Mnemonic::FNMADDS;
    default: return Mnemonic::UNKNOWN;
    }
}

// ============================================================================
// Extended opcode 63 (double-precision floating point — X-form and A-form)
//
// X-form instructions: extended opcode = bits 21-30 (10 bits)
// A-form instructions: extended opcode = bits 26-30 (5 bits), bits 21-25 = rC
// Strategy: check xop10 for known X-form values first; fall through to xop5.
// ============================================================================

static Mnemonic decode_op63(uint32_t word, Instruction& out) noexcept {
    out.iclass = InstrClass::Float;
    out.rD = static_cast<uint8_t>(ppc_field(word, 6, 10));
    out.rA = static_cast<uint8_t>(ppc_field(word, 11, 15));
    out.rB = static_cast<uint8_t>(ppc_field(word, 16, 20));
    out.rC = static_cast<uint8_t>(ppc_field(word, 21, 25));
    out.rc = ppc_bit(word, 31) != 0;
    uint32_t xop10 = ppc_field(word, 21, 30);
    uint32_t xop5  = ppc_field(word, 26, 30);

    // X-form ops (extended xop uses all 10 bits; rC field is irrelevant)
    switch (xop10) {
    case 0:   out.crf = static_cast<uint8_t>(out.rD >> 2); return Mnemonic::FCMPU;
    case 12:  return Mnemonic::FRSP;
    case 14:  return Mnemonic::FCTIW;
    case 15:  return Mnemonic::FCTIWZ;
    case 32:  out.crf = static_cast<uint8_t>(out.rD >> 2); return Mnemonic::FCMPO;
    case 40:  return out.rc ? Mnemonic::FNEG_  : Mnemonic::FNEG;
    case 72:  return out.rc ? Mnemonic::FMR_   : Mnemonic::FMR;
    case 136: return out.rc ? Mnemonic::FNABS_ : Mnemonic::FNABS;
    case 264: return out.rc ? Mnemonic::FABS_  : Mnemonic::FABS;
    default:  break;
    }

    // A-form ops (bits 26-30 are the sub-opcode; bits 21-25 are rC)
    switch (xop5) {
    case 18: return Mnemonic::FDIV;
    case 20: return Mnemonic::FSUB;
    case 21: return Mnemonic::FADD;
    case 22: return Mnemonic::FSQRT;
    case 23: return Mnemonic::FSEL;
    case 25: return Mnemonic::FMUL;
    case 26: return Mnemonic::FRSQRTE;
    case 28: return Mnemonic::FMSUB;
    case 29: return Mnemonic::FMADD;
    case 30: return Mnemonic::FNMSUB;
    case 31: return Mnemonic::FNMADD;
    default: return Mnemonic::UNKNOWN;
    }
}

// ============================================================================
// Public decode function
// ============================================================================

std::optional<Instruction> decode(uint32_t word, uint32_t pc) noexcept {
    // NOP is a special case: ori 0,0,0
    if (word == 0x60000000u) {
        Instruction nop;
        nop.addr     = pc;
        nop.word     = word;
        nop.mnemonic = Mnemonic::NOP;
        nop.iclass   = InstrClass::Integer;
        return nop;
    }

    Instruction out;
    out.addr = pc;
    out.word = word;

    uint32_t op = ppc_field(word, 0, 5);
    switch (op) {
    case 3:  out.iclass = InstrClass::System; out.mnemonic = Mnemonic::TWI; break;
    case 7:  out.iclass = InstrClass::Integer; out.mnemonic = Mnemonic::MULLI; break;
    case 8:  out.iclass = InstrClass::Integer; out.mnemonic = Mnemonic::SUBFIC; break;
    case 17: out.iclass = InstrClass::System;  out.mnemonic = Mnemonic::SC;    break;
    case 19: out.mnemonic = decode_op19(word, out); break;
    case 31: out.mnemonic = decode_op31(word, out); break;
    case 59: out.mnemonic = decode_op59(word, out); break;
    case 63: out.mnemonic = decode_op63(word, out); break;
    default:
        out.mnemonic = classify_primary(word, out);
        break;
    }

    // Fill common fields for primary opcodes not handled above
    if (op != 19 && op != 31 && op != 59 && op != 63) {
        out.rD   = static_cast<uint8_t>(ppc_field(word, 6, 10));
        out.rA   = static_cast<uint8_t>(ppc_field(word, 11, 15));
        out.rB   = static_cast<uint8_t>(ppc_field(word, 16, 20));
        out.imm  = sign_extend_16(ppc_field(word, 16, 31));
        out.uimm = ppc_field(word, 16, 31);
    }

    return out;
}

std::vector<Instruction> decode_block(std::span<const uint8_t> bytes,
                                       uint32_t base_addr) {
    std::vector<Instruction> result;
    result.reserve(bytes.size() / 4);
    for (size_t i = 0; i + 4 <= bytes.size(); i += 4) {
        uint32_t word = (uint32_t(bytes[i])   << 24) |
                        (uint32_t(bytes[i+1]) << 16) |
                        (uint32_t(bytes[i+2]) <<  8) |
                         uint32_t(bytes[i+3]);
        auto insn = decode(word, base_addr + static_cast<uint32_t>(i));
        if (insn) result.push_back(*insn);
    }
    return result;
}

std::string disassemble(const Instruction& insn) {
    // Minimal disassembly — just hex + mnemonic index for now
    std::ostringstream oss;
    oss << std::hex << std::setw(8) << std::setfill('0') << insn.addr
        << ": " << std::setw(8) << insn.word;
    return oss.str();
}

}