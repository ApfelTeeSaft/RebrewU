#include "ppc_semantics.hpp"

namespace rebrewu::ppc {

using namespace ir;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Emit a CR0 update after an Rc=1 instruction (result already in `res`).
static void emit_rc(VReg res, IRBuilder& b, uint32_t ga) {
    b.emit(Opcode::CmpSigned, VReg::cr(0),
           {reg(res), imm(0)}, ga);
}

// Compute rlwinm mask from MB/ME fields (PPC rotate mask).
static uint32_t ppc_mask(uint32_t mb, uint32_t me) {
    uint32_t mask = 0;
    if (mb <= me) {
        for (uint32_t i = mb; i <= me; ++i)
            mask |= (1u << (31 - i));
    } else {
        for (uint32_t i = 0; i <= me; ++i)
            mask |= (1u << (31 - i));
        for (uint32_t i = mb; i <= 31; ++i)
            mask |= (1u << (31 - i));
    }
    return mask;
}

// ---------------------------------------------------------------------------
// top-level dispatcher
// ---------------------------------------------------------------------------

bool lower_to_ir(const Instruction& insn,
                 IRBuilder& builder,
                 IRFunction& func) {
    // Route compare mnemonics regardless of iclass
    switch (insn.mnemonic) {
    case Mnemonic::CMP: case Mnemonic::CMPI:
    case Mnemonic::CMPL: case Mnemonic::CMPLI:
    case Mnemonic::FCMPU: case Mnemonic::FCMPO:
        return lower_compare(insn, builder, func);
    default: break;
    }

    switch (insn.iclass) {
    case InstrClass::Integer:  return lower_integer(insn, builder, func);
    case InstrClass::Rotate:   return lower_rotate(insn, builder, func);
    case InstrClass::Float:    return lower_float(insn, builder, func);
    case InstrClass::Load:     return lower_load(insn, builder, func);
    case InstrClass::Store:    return lower_store(insn, builder, func);
    case InstrClass::Branch:   return lower_branch(insn, builder, func);
    default:
        builder.create_nop(insn.addr);
        return true;
    }
}

// ---------------------------------------------------------------------------
// Integer / logical / SPR
// ---------------------------------------------------------------------------

bool lower_integer(const Instruction& insn, IRBuilder& b, IRFunction& f) {
    const uint32_t ga = insn.addr;
    switch (insn.mnemonic) {

    // ---- NOP / move ----
    case Mnemonic::NOP:
        b.create_nop(ga);
        return true;

    case Mnemonic::LI:
        b.emit(Opcode::Move, VReg::gpr(insn.rD),
               {imm(static_cast<uint32_t>(insn.imm))}, ga);
        return true;

    case Mnemonic::LIS:
        b.emit(Opcode::Move, VReg::gpr(insn.rD),
               {imm(static_cast<uint32_t>(insn.imm) << 16)}, ga);
        return true;

    case Mnemonic::MR: case Mnemonic::MR_:
        b.emit(Opcode::Move, VReg::gpr(insn.rA),
               {reg(VReg::gpr(insn.rD))}, ga);
        if (insn.mnemonic == Mnemonic::MR_)
            emit_rc(VReg::gpr(insn.rA), b, ga);
        return true;

    case Mnemonic::NOT: case Mnemonic::NOT_:
        b.emit(Opcode::Not, VReg::gpr(insn.rA),
               {reg(VReg::gpr(insn.rD))}, ga);
        if (insn.mnemonic == Mnemonic::NOT_)
            emit_rc(VReg::gpr(insn.rA), b, ga);
        return true;

    // ---- Add ----
    case Mnemonic::ADDI:
        b.emit(Opcode::Add, VReg::gpr(insn.rD),
               {reg(VReg::gpr(insn.rA)), imm(static_cast<uint32_t>(insn.imm))}, ga);
        return true;

    case Mnemonic::ADDIS:
        b.emit(Opcode::Add, VReg::gpr(insn.rD),
               {reg(VReg::gpr(insn.rA)), imm(static_cast<uint32_t>(insn.imm) << 16)}, ga);
        return true;

    case Mnemonic::ADDIC: case Mnemonic::ADDIC_: {
        b.emit(Opcode::Add, VReg::gpr(insn.rD),
               {reg(VReg::gpr(insn.rA)), imm(static_cast<uint32_t>(insn.imm))}, ga);
        // TODO: update XER.CA
        if (insn.mnemonic == Mnemonic::ADDIC_)
            emit_rc(VReg::gpr(insn.rD), b, ga);
        return true;
    }

    case Mnemonic::ADD: case Mnemonic::ADD_:
    case Mnemonic::ADDO: case Mnemonic::ADDO_:
        b.emit(Opcode::Add, VReg::gpr(insn.rD),
               {reg(VReg::gpr(insn.rA)), reg(VReg::gpr(insn.rB))}, ga);
        if (insn.rc) emit_rc(VReg::gpr(insn.rD), b, ga);
        return true;

    case Mnemonic::ADDC: case Mnemonic::ADDC_:
    case Mnemonic::ADDCO: case Mnemonic::ADDCO_:
        b.emit(Opcode::Add, VReg::gpr(insn.rD),
               {reg(VReg::gpr(insn.rA)), reg(VReg::gpr(insn.rB))}, ga);
        // TODO: XER.CA
        if (insn.rc) emit_rc(VReg::gpr(insn.rD), b, ga);
        return true;

    case Mnemonic::ADDE: case Mnemonic::ADDE_:
    case Mnemonic::ADDEO: case Mnemonic::ADDEO_: {
        // rD = rA + rB + XER.CA (simplified: ignore carry-in)
        b.emit(Opcode::Add, VReg::gpr(insn.rD),
               {reg(VReg::gpr(insn.rA)), reg(VReg::gpr(insn.rB))}, ga);
        if (insn.rc) emit_rc(VReg::gpr(insn.rD), b, ga);
        return true;
    }

    case Mnemonic::ADDME: case Mnemonic::ADDME_:
    case Mnemonic::ADDMEO: case Mnemonic::ADDMEO_:
        // rD = rA + XER.CA - 1  (simplified: rD = rA - 1)
        b.emit(Opcode::Add, VReg::gpr(insn.rD),
               {reg(VReg::gpr(insn.rA)), imm(~0u)}, ga);
        if (insn.rc) emit_rc(VReg::gpr(insn.rD), b, ga);
        return true;

    case Mnemonic::ADDZE: case Mnemonic::ADDZE_:
    case Mnemonic::ADDZEO: case Mnemonic::ADDZEO_:
        // rD = rA + XER.CA (simplified: rD = rA)
        b.emit(Opcode::Move, VReg::gpr(insn.rD),
               {reg(VReg::gpr(insn.rA))}, ga);
        if (insn.rc) emit_rc(VReg::gpr(insn.rD), b, ga);
        return true;

    // ---- Sub ----
    case Mnemonic::SUBF: case Mnemonic::SUBF_:
    case Mnemonic::SUBFO: case Mnemonic::SUBFO_:
        // rD = ~rA + rB + 1 = rB - rA
        b.emit(Opcode::Sub, VReg::gpr(insn.rD),
               {reg(VReg::gpr(insn.rB)), reg(VReg::gpr(insn.rA))}, ga);
        if (insn.rc) emit_rc(VReg::gpr(insn.rD), b, ga);
        return true;

    case Mnemonic::SUBFIC:
        b.emit(Opcode::Sub, VReg::gpr(insn.rD),
               {imm(static_cast<uint32_t>(insn.imm)), reg(VReg::gpr(insn.rA))}, ga);
        return true;

    case Mnemonic::SUBFC: case Mnemonic::SUBFC_:
    case Mnemonic::SUBFCO: case Mnemonic::SUBFCO_:
        b.emit(Opcode::Sub, VReg::gpr(insn.rD),
               {reg(VReg::gpr(insn.rB)), reg(VReg::gpr(insn.rA))}, ga);
        if (insn.rc) emit_rc(VReg::gpr(insn.rD), b, ga);
        return true;

    case Mnemonic::SUBFE: case Mnemonic::SUBFE_:
    case Mnemonic::SUBFEO: case Mnemonic::SUBFEO_:
        b.emit(Opcode::Sub, VReg::gpr(insn.rD),
               {reg(VReg::gpr(insn.rB)), reg(VReg::gpr(insn.rA))}, ga);
        if (insn.rc) emit_rc(VReg::gpr(insn.rD), b, ga);
        return true;

    case Mnemonic::SUBFME: case Mnemonic::SUBFME_:
    case Mnemonic::SUBFMEO: case Mnemonic::SUBFMEO_:
        // rD = ~rA + XER.CA - 1 (simplified: ~rA)
        b.emit(Opcode::Not, VReg::gpr(insn.rD),
               {reg(VReg::gpr(insn.rA))}, ga);
        if (insn.rc) emit_rc(VReg::gpr(insn.rD), b, ga);
        return true;

    case Mnemonic::SUBFZE: case Mnemonic::SUBFZE_:
    case Mnemonic::SUBFZEO: case Mnemonic::SUBFZEO_:
        // rD = ~rA + XER.CA (simplified: ~rA)
        b.emit(Opcode::Not, VReg::gpr(insn.rD),
               {reg(VReg::gpr(insn.rA))}, ga);
        if (insn.rc) emit_rc(VReg::gpr(insn.rD), b, ga);
        return true;

    // ---- Neg ----
    case Mnemonic::NEG: case Mnemonic::NEG_:
    case Mnemonic::NEGO: case Mnemonic::NEGO_:
        b.emit(Opcode::Neg, VReg::gpr(insn.rD),
               {reg(VReg::gpr(insn.rA))}, ga);
        if (insn.rc) emit_rc(VReg::gpr(insn.rD), b, ga);
        return true;

    // ---- Multiply ----
    case Mnemonic::MULLI:
        b.emit(Opcode::Mul, VReg::gpr(insn.rD),
               {reg(VReg::gpr(insn.rA)), imm(static_cast<uint32_t>(insn.imm))}, ga);
        return true;

    case Mnemonic::MULLW: case Mnemonic::MULLW_:
    case Mnemonic::MULLWO: case Mnemonic::MULLWO_:
        b.emit(Opcode::Mul, VReg::gpr(insn.rD),
               {reg(VReg::gpr(insn.rA)), reg(VReg::gpr(insn.rB))}, ga);
        if (insn.rc) emit_rc(VReg::gpr(insn.rD), b, ga);
        return true;

    case Mnemonic::MULHW: case Mnemonic::MULHW_:
        b.emit(Opcode::MulHigh, VReg::gpr(insn.rD),
               {reg(VReg::gpr(insn.rA)), reg(VReg::gpr(insn.rB))}, ga);
        if (insn.rc) emit_rc(VReg::gpr(insn.rD), b, ga);
        return true;

    case Mnemonic::MULHWU: case Mnemonic::MULHWU_:
        b.emit(Opcode::MulHighU, VReg::gpr(insn.rD),
               {reg(VReg::gpr(insn.rA)), reg(VReg::gpr(insn.rB))}, ga);
        if (insn.rc) emit_rc(VReg::gpr(insn.rD), b, ga);
        return true;

    // ---- Divide ----
    case Mnemonic::DIVW: case Mnemonic::DIVW_:
    case Mnemonic::DIVWO: case Mnemonic::DIVWO_:
        b.emit(Opcode::Div, VReg::gpr(insn.rD),
               {reg(VReg::gpr(insn.rA)), reg(VReg::gpr(insn.rB))}, ga);
        if (insn.rc) emit_rc(VReg::gpr(insn.rD), b, ga);
        return true;

    case Mnemonic::DIVWU: case Mnemonic::DIVWU_:
    case Mnemonic::DIVWUO: case Mnemonic::DIVWUO_:
        b.emit(Opcode::DivU, VReg::gpr(insn.rD),
               {reg(VReg::gpr(insn.rA)), reg(VReg::gpr(insn.rB))}, ga);
        if (insn.rc) emit_rc(VReg::gpr(insn.rD), b, ga);
        return true;

    // ---- AND ----
    case Mnemonic::ANDI_:
        b.emit(Opcode::And, VReg::gpr(insn.rA),
               {reg(VReg::gpr(insn.rD)), imm(insn.uimm)}, ga);
        emit_rc(VReg::gpr(insn.rA), b, ga); // ANDI. always sets CR0
        return true;

    case Mnemonic::ANDIS_:
        b.emit(Opcode::And, VReg::gpr(insn.rA),
               {reg(VReg::gpr(insn.rD)), imm(insn.uimm << 16)}, ga);
        emit_rc(VReg::gpr(insn.rA), b, ga);
        return true;

    case Mnemonic::AND: case Mnemonic::AND_:
        b.emit(Opcode::And, VReg::gpr(insn.rA),
               {reg(VReg::gpr(insn.rD)), reg(VReg::gpr(insn.rB))}, ga);
        if (insn.rc) emit_rc(VReg::gpr(insn.rA), b, ga);
        return true;

    case Mnemonic::ANDC: case Mnemonic::ANDC_: {
        VReg notb = f.alloc_temp();
        b.emit(Opcode::Not, notb, {reg(VReg::gpr(insn.rB))}, ga);
        b.emit(Opcode::And, VReg::gpr(insn.rA),
               {reg(VReg::gpr(insn.rD)), reg(notb)}, ga);
        if (insn.rc) emit_rc(VReg::gpr(insn.rA), b, ga);
        return true;
    }

    case Mnemonic::NAND: case Mnemonic::NAND_: {
        VReg tmp = f.alloc_temp();
        b.emit(Opcode::And, tmp, {reg(VReg::gpr(insn.rD)), reg(VReg::gpr(insn.rB))}, ga);
        b.emit(Opcode::Not, VReg::gpr(insn.rA), {reg(tmp)}, ga);
        if (insn.rc) emit_rc(VReg::gpr(insn.rA), b, ga);
        return true;
    }

    // ---- OR ----
    case Mnemonic::ORI:
        b.emit(Opcode::Or, VReg::gpr(insn.rA),
               {reg(VReg::gpr(insn.rD)), imm(insn.uimm)}, ga);
        return true;

    case Mnemonic::ORIS:
        b.emit(Opcode::Or, VReg::gpr(insn.rA),
               {reg(VReg::gpr(insn.rD)), imm(insn.uimm << 16)}, ga);
        return true;

    case Mnemonic::OR: case Mnemonic::OR_:
        b.emit(Opcode::Or, VReg::gpr(insn.rA),
               {reg(VReg::gpr(insn.rD)), reg(VReg::gpr(insn.rB))}, ga);
        if (insn.rc) emit_rc(VReg::gpr(insn.rA), b, ga);
        return true;

    case Mnemonic::ORC: case Mnemonic::ORC_: {
        VReg notb = f.alloc_temp();
        b.emit(Opcode::Not, notb, {reg(VReg::gpr(insn.rB))}, ga);
        b.emit(Opcode::Or, VReg::gpr(insn.rA),
               {reg(VReg::gpr(insn.rD)), reg(notb)}, ga);
        if (insn.rc) emit_rc(VReg::gpr(insn.rA), b, ga);
        return true;
    }

    case Mnemonic::NOR: case Mnemonic::NOR_: {
        VReg tmp = f.alloc_temp();
        b.emit(Opcode::Or, tmp, {reg(VReg::gpr(insn.rD)), reg(VReg::gpr(insn.rB))}, ga);
        b.emit(Opcode::Not, VReg::gpr(insn.rA), {reg(tmp)}, ga);
        if (insn.rc) emit_rc(VReg::gpr(insn.rA), b, ga);
        return true;
    }

    // ---- XOR ----
    case Mnemonic::XORI:
        b.emit(Opcode::Xor, VReg::gpr(insn.rA),
               {reg(VReg::gpr(insn.rD)), imm(insn.uimm)}, ga);
        return true;

    case Mnemonic::XORIS:
        b.emit(Opcode::Xor, VReg::gpr(insn.rA),
               {reg(VReg::gpr(insn.rD)), imm(insn.uimm << 16)}, ga);
        return true;

    case Mnemonic::XOR: case Mnemonic::XOR_:
        b.emit(Opcode::Xor, VReg::gpr(insn.rA),
               {reg(VReg::gpr(insn.rD)), reg(VReg::gpr(insn.rB))}, ga);
        if (insn.rc) emit_rc(VReg::gpr(insn.rA), b, ga);
        return true;

    // ---- EQV ----
    case Mnemonic::EQV: case Mnemonic::EQV_: {
        VReg tmp = f.alloc_temp();
        b.emit(Opcode::Xor, tmp, {reg(VReg::gpr(insn.rD)), reg(VReg::gpr(insn.rB))}, ga);
        b.emit(Opcode::Not, VReg::gpr(insn.rA), {reg(tmp)}, ga);
        if (insn.rc) emit_rc(VReg::gpr(insn.rA), b, ga);
        return true;
    }

    // ---- Extend ----
    case Mnemonic::EXTSB: case Mnemonic::EXTSB_:
        b.emit(Opcode::SignExtend, VReg::gpr(insn.rA),
               {reg(VReg::gpr(insn.rD)), imm(8)}, ga);
        if (insn.rc) emit_rc(VReg::gpr(insn.rA), b, ga);
        return true;

    case Mnemonic::EXTSH: case Mnemonic::EXTSH_:
        b.emit(Opcode::SignExtend, VReg::gpr(insn.rA),
               {reg(VReg::gpr(insn.rD)), imm(16)}, ga);
        if (insn.rc) emit_rc(VReg::gpr(insn.rA), b, ga);
        return true;

    // ---- Count leading zeros ----
    case Mnemonic::CNTLZW: case Mnemonic::CNTLZW_:
        b.emit(Opcode::CountLeadingZeros, VReg::gpr(insn.rA),
               {reg(VReg::gpr(insn.rD))}, ga);
        if (insn.rc) emit_rc(VReg::gpr(insn.rA), b, ga);
        return true;

    // ---- SPR moves ----
    case Mnemonic::MTLR:
        b.emit(Opcode::Move, VReg::lr(), {reg(VReg::gpr(insn.rD))}, ga);
        return true;

    case Mnemonic::MFLR:
        b.emit(Opcode::Move, VReg::gpr(insn.rD), {reg(VReg::lr())}, ga);
        return true;

    case Mnemonic::MTCTR:
        b.emit(Opcode::Move, VReg::ctr(), {reg(VReg::gpr(insn.rD))}, ga);
        return true;

    case Mnemonic::MFCTR:
        b.emit(Opcode::Move, VReg::gpr(insn.rD), {reg(VReg::ctr())}, ga);
        return true;

    case Mnemonic::MTXER:
        b.emit(Opcode::Move, VReg::xer(), {reg(VReg::gpr(insn.rD))}, ga);
        return true;

    case Mnemonic::MFXER:
        b.emit(Opcode::Move, VReg::gpr(insn.rD), {reg(VReg::xer())}, ga);
        return true;

    case Mnemonic::MFCR:
        // Simplified: move CR0 into rD
        b.emit(Opcode::Move, VReg::gpr(insn.rD), {reg(VReg::cr(0))}, ga);
        return true;

    case Mnemonic::MTCRF:
        // Simplified: update CR0
        b.emit(Opcode::Move, VReg::cr(0), {reg(VReg::gpr(insn.rD))}, ga);
        return true;

    default:
        b.create_nop(ga);
        return true;
    }
}

// ---------------------------------------------------------------------------
// Compare
// ---------------------------------------------------------------------------

bool lower_compare(const Instruction& insn, IRBuilder& b, IRFunction& /*f*/) {
    const uint32_t ga = insn.addr;
    // rD bits encode crfD in bits 6-8 (top 3 bits of the 5-bit rD field)
    const uint32_t crf = insn.rD >> 2;
    switch (insn.mnemonic) {
    case Mnemonic::CMP:
        b.emit(Opcode::CmpSigned, VReg::cr(crf),
               {reg(VReg::gpr(insn.rA)), reg(VReg::gpr(insn.rB))}, ga);
        return true;

    case Mnemonic::CMPI:
        b.emit(Opcode::CmpSigned, VReg::cr(crf),
               {reg(VReg::gpr(insn.rA)), imm(static_cast<uint32_t>(insn.imm))}, ga);
        return true;

    case Mnemonic::CMPL:
        b.emit(Opcode::CmpUnsigned, VReg::cr(crf),
               {reg(VReg::gpr(insn.rA)), reg(VReg::gpr(insn.rB))}, ga);
        return true;

    case Mnemonic::CMPLI:
        b.emit(Opcode::CmpUnsigned, VReg::cr(crf),
               {reg(VReg::gpr(insn.rA)), imm(insn.uimm)}, ga);
        return true;

    case Mnemonic::FCMPU:
    case Mnemonic::FCMPO:
        b.emit(Opcode::CmpFloat, VReg::cr(crf),
               {reg(VReg::fpr(insn.rA)), reg(VReg::fpr(insn.rB))}, ga);
        return true;

    default:
        b.create_nop(ga);
        return true;
    }
}

// ---------------------------------------------------------------------------
// SPR (delegates to lower_integer)
// ---------------------------------------------------------------------------

bool lower_spr(const Instruction& insn, IRBuilder& b, IRFunction& f) {
    return lower_integer(insn, b, f);
}

// ---------------------------------------------------------------------------
// Rotate / shift
// ---------------------------------------------------------------------------

bool lower_rotate(const Instruction& insn, IRBuilder& b, IRFunction& f) {
    const uint32_t ga = insn.addr;
    switch (insn.mnemonic) {

    // ---- rlwinm: rA = rotl(rS, sh) & mask(mb, me) ----
    case Mnemonic::RLWINM: case Mnemonic::RLWINM_:
        b.emit(Opcode::ExtractBits, VReg::gpr(insn.rA),
               {reg(VReg::gpr(insn.rD)),
                imm(insn.sh), imm(insn.mb), imm(insn.me)}, ga);
        if (insn.rc) emit_rc(VReg::gpr(insn.rA), b, ga);
        return true;

    // ---- rlwimi: rA = (rA & ~mask) | (rotl(rS, sh) & mask) ----
    case Mnemonic::RLWIMI: case Mnemonic::RLWIMI_: {
        uint32_t mask = ppc_mask(insn.mb, insn.me);
        // inserted bits
        VReg ins = f.alloc_temp();
        b.emit(Opcode::ExtractBits, ins,
               {reg(VReg::gpr(insn.rD)),
                imm(insn.sh), imm(insn.mb), imm(insn.me)}, ga);
        // keep bits in rA that are outside the mask
        VReg kept = f.alloc_temp();
        b.emit(Opcode::And, kept, {reg(VReg::gpr(insn.rA)), imm(~mask)}, ga);
        // merge
        b.emit(Opcode::Or, VReg::gpr(insn.rA), {reg(kept), reg(ins)}, ga);
        if (insn.rc) emit_rc(VReg::gpr(insn.rA), b, ga);
        return true;
    }

    // ---- rlwnm: rA = rotl(rS, rB & 31) & mask(mb, me) ----
    case Mnemonic::RLWNM: case Mnemonic::RLWNM_:
        b.emit(Opcode::ExtractBits, VReg::gpr(insn.rA),
               {reg(VReg::gpr(insn.rD)),
                reg(VReg::gpr(insn.rB)), imm(insn.mb), imm(insn.me)}, ga);
        if (insn.rc) emit_rc(VReg::gpr(insn.rA), b, ga);
        return true;

    // ---- Logical shifts ----
    case Mnemonic::SLW: case Mnemonic::SLW_:
        b.emit(Opcode::Shl, VReg::gpr(insn.rA),
               {reg(VReg::gpr(insn.rD)), reg(VReg::gpr(insn.rB))}, ga);
        if (insn.rc) emit_rc(VReg::gpr(insn.rA), b, ga);
        return true;

    case Mnemonic::SRW: case Mnemonic::SRW_:
        b.emit(Opcode::Shr, VReg::gpr(insn.rA),
               {reg(VReg::gpr(insn.rD)), reg(VReg::gpr(insn.rB))}, ga);
        if (insn.rc) emit_rc(VReg::gpr(insn.rA), b, ga);
        return true;

    // ---- Arithmetic right shift ----
    case Mnemonic::SRAW: case Mnemonic::SRAW_:
        b.emit(Opcode::Sar, VReg::gpr(insn.rA),
               {reg(VReg::gpr(insn.rD)), reg(VReg::gpr(insn.rB))}, ga);
        // TODO: XER.CA
        if (insn.rc) emit_rc(VReg::gpr(insn.rA), b, ga);
        return true;

    case Mnemonic::SRAWI: case Mnemonic::SRAWI_:
        b.emit(Opcode::Sar, VReg::gpr(insn.rA),
               {reg(VReg::gpr(insn.rD)), imm(insn.sh)}, ga);
        // TODO: XER.CA
        if (insn.rc) emit_rc(VReg::gpr(insn.rA), b, ga);
        return true;

    default:
        b.create_nop(ga);
        return true;
    }
}

// ---------------------------------------------------------------------------
// Floating point
// ---------------------------------------------------------------------------

bool lower_float(const Instruction& insn, IRBuilder& b, IRFunction& f) {
    const uint32_t ga = insn.addr;
    switch (insn.mnemonic) {

    // ---- Move / abs / neg ----
    case Mnemonic::FMR: case Mnemonic::FMR_:
        b.emit(Opcode::Move, VReg::fpr(insn.rD),
               {reg(VReg::fpr(insn.rB))}, ga);
        return true;

    case Mnemonic::FABS: case Mnemonic::FABS_:
        b.emit(Opcode::FAbs, VReg::fpr(insn.rD),
               {reg(VReg::fpr(insn.rB))}, ga);
        return true;

    case Mnemonic::FNABS: case Mnemonic::FNABS_: {
        VReg tmp = f.alloc_temp();
        b.emit(Opcode::FAbs, tmp, {reg(VReg::fpr(insn.rB))}, ga);
        b.emit(Opcode::FNeg, VReg::fpr(insn.rD), {reg(tmp)}, ga);
        return true;
    }

    case Mnemonic::FNEG: case Mnemonic::FNEG_:
        b.emit(Opcode::FNeg, VReg::fpr(insn.rD),
               {reg(VReg::fpr(insn.rB))}, ga);
        return true;

    // ---- Arithmetic (double and single precision) ----
    case Mnemonic::FADD: case Mnemonic::FADDS:
        b.emit(Opcode::FAdd, VReg::fpr(insn.rD),
               {reg(VReg::fpr(insn.rA)), reg(VReg::fpr(insn.rB))}, ga);
        return true;

    case Mnemonic::FSUB: case Mnemonic::FSUBS:
        b.emit(Opcode::FSub, VReg::fpr(insn.rD),
               {reg(VReg::fpr(insn.rA)), reg(VReg::fpr(insn.rB))}, ga);
        return true;

    case Mnemonic::FMUL: case Mnemonic::FMULS:
        b.emit(Opcode::FMul, VReg::fpr(insn.rD),
               {reg(VReg::fpr(insn.rA)), reg(VReg::fpr(insn.rC))}, ga);
        return true;

    case Mnemonic::FDIV: case Mnemonic::FDIVS:
        b.emit(Opcode::FDiv, VReg::fpr(insn.rD),
               {reg(VReg::fpr(insn.rA)), reg(VReg::fpr(insn.rB))}, ga);
        return true;

    case Mnemonic::FSQRT: case Mnemonic::FSQRTS:
        b.emit(Opcode::FSqrt, VReg::fpr(insn.rD),
               {reg(VReg::fpr(insn.rB))}, ga);
        return true;

    // ---- Fused multiply-add/sub ----
    case Mnemonic::FMADD: case Mnemonic::FMADDS:
        b.emit(Opcode::FMadd, VReg::fpr(insn.rD),
               {reg(VReg::fpr(insn.rA)), reg(VReg::fpr(insn.rC)),
                reg(VReg::fpr(insn.rB))}, ga);
        return true;

    case Mnemonic::FMSUB: case Mnemonic::FMSUBS:
        b.emit(Opcode::FMsub, VReg::fpr(insn.rD),
               {reg(VReg::fpr(insn.rA)), reg(VReg::fpr(insn.rC)),
                reg(VReg::fpr(insn.rB))}, ga);
        return true;

    case Mnemonic::FNMADD: case Mnemonic::FNMADDS:
        b.emit(Opcode::FNmadd, VReg::fpr(insn.rD),
               {reg(VReg::fpr(insn.rA)), reg(VReg::fpr(insn.rC)),
                reg(VReg::fpr(insn.rB))}, ga);
        return true;

    case Mnemonic::FNMSUB: case Mnemonic::FNMSUBS:
        b.emit(Opcode::FNmsub, VReg::fpr(insn.rD),
               {reg(VReg::fpr(insn.rA)), reg(VReg::fpr(insn.rC)),
                reg(VReg::fpr(insn.rB))}, ga);
        return true;

    // ---- Conversion ----
    case Mnemonic::FRSP:
        b.emit(Opcode::FCvtPrecision, VReg::fpr(insn.rD),
               {reg(VReg::fpr(insn.rB))}, ga);
        return true;

    case Mnemonic::FCTIW: case Mnemonic::FCTIWZ:
        b.emit(Opcode::FCvtToInt, VReg::fpr(insn.rD),
               {reg(VReg::fpr(insn.rB))}, ga);
        return true;

    // ---- Reciprocal estimates (approximate) ----
    case Mnemonic::FRES: {
        // fD = 1.0 / fB  (estimate — exact enough for static recomp)
        VReg one = f.alloc_temp();
        b.emit(Opcode::FCvtFromInt, one, {imm(1)}, ga);
        b.emit(Opcode::FDiv, VReg::fpr(insn.rD), {reg(one), reg(VReg::fpr(insn.rB))}, ga);
        return true;
    }

    case Mnemonic::FRSQRTE: {
        // fD = 1.0 / sqrt(fB)
        VReg sq = f.alloc_temp();
        b.emit(Opcode::FSqrt, sq, {reg(VReg::fpr(insn.rB))}, ga);
        VReg one = f.alloc_temp();
        b.emit(Opcode::FCvtFromInt, one, {imm(1)}, ga);
        b.emit(Opcode::FDiv, VReg::fpr(insn.rD), {reg(one), reg(sq)}, ga);
        return true;
    }

    case Mnemonic::FSEL: {
        // fD = (fA >= 0.0) ? fC : fB — emit as rbrew_fsel runtime helper
        // Simplified: use FCvtFromInt(0) as zero to compare against
        // Represented as a no-op that preserves existing value for now.
        // Full implementation would need a Select opcode; emit nop + comment.
        b.create_nop(ga);
        return true;
    }

    default:
        b.create_nop(ga);
        return true;
    }
}

// ---------------------------------------------------------------------------
// Memory loads
// ---------------------------------------------------------------------------

bool lower_load(const Instruction& insn, IRBuilder& b, IRFunction& f) {
    const uint32_t ga = insn.addr;

    // Helper: compute effective address (base + offset or base + index)
    auto make_ea = [&](bool indexed) -> VReg {
        if (indexed)
            return b.create_add(reg(VReg::gpr(insn.rA)), reg(VReg::gpr(insn.rB)), ga);
        else
            return b.create_add(reg(VReg::gpr(insn.rA)),
                                imm(static_cast<uint32_t>(insn.imm)), ga);
    };

    // Determine operation and whether it's indexed / update
    switch (insn.mnemonic) {

    // ---- Byte unsigned ----
    case Mnemonic::LBZ:  case Mnemonic::LBZU: {
        VReg ea = make_ea(false);
        b.emit(Opcode::Load8, VReg::gpr(insn.rD), {reg(ea)}, ga);
        if (insn.mnemonic == Mnemonic::LBZU)
            b.emit(Opcode::Move, VReg::gpr(insn.rA), {reg(ea)}, ga);
        return true;
    }
    case Mnemonic::LBZX: case Mnemonic::LBZUX: {
        VReg ea = make_ea(true);
        b.emit(Opcode::Load8, VReg::gpr(insn.rD), {reg(ea)}, ga);
        if (insn.mnemonic == Mnemonic::LBZUX)
            b.emit(Opcode::Move, VReg::gpr(insn.rA), {reg(ea)}, ga);
        return true;
    }

    // ---- Halfword unsigned ----
    case Mnemonic::LHZ:  case Mnemonic::LHZU: {
        VReg ea = make_ea(false);
        b.emit(Opcode::Load16, VReg::gpr(insn.rD), {reg(ea)}, ga);
        if (insn.mnemonic == Mnemonic::LHZU)
            b.emit(Opcode::Move, VReg::gpr(insn.rA), {reg(ea)}, ga);
        return true;
    }
    case Mnemonic::LHZX: case Mnemonic::LHZUX: {
        VReg ea = make_ea(true);
        b.emit(Opcode::Load16, VReg::gpr(insn.rD), {reg(ea)}, ga);
        if (insn.mnemonic == Mnemonic::LHZUX)
            b.emit(Opcode::Move, VReg::gpr(insn.rA), {reg(ea)}, ga);
        return true;
    }

    // ---- Halfword signed ----
    case Mnemonic::LHA:  case Mnemonic::LHAU: {
        VReg ea = make_ea(false);
        b.emit(Opcode::Load16S, VReg::gpr(insn.rD), {reg(ea)}, ga);
        if (insn.mnemonic == Mnemonic::LHAU)
            b.emit(Opcode::Move, VReg::gpr(insn.rA), {reg(ea)}, ga);
        return true;
    }
    case Mnemonic::LHAX: case Mnemonic::LHAUX: {
        VReg ea = make_ea(true);
        b.emit(Opcode::Load16S, VReg::gpr(insn.rD), {reg(ea)}, ga);
        if (insn.mnemonic == Mnemonic::LHAUX)
            b.emit(Opcode::Move, VReg::gpr(insn.rA), {reg(ea)}, ga);
        return true;
    }

    // ---- Word ----
    case Mnemonic::LWZ:  case Mnemonic::LWZU: {
        VReg ea = make_ea(false);
        b.emit(Opcode::Load32, VReg::gpr(insn.rD), {reg(ea)}, ga);
        if (insn.mnemonic == Mnemonic::LWZU)
            b.emit(Opcode::Move, VReg::gpr(insn.rA), {reg(ea)}, ga);
        return true;
    }
    case Mnemonic::LWZX: case Mnemonic::LWZUX: {
        VReg ea = make_ea(true);
        b.emit(Opcode::Load32, VReg::gpr(insn.rD), {reg(ea)}, ga);
        if (insn.mnemonic == Mnemonic::LWZUX)
            b.emit(Opcode::Move, VReg::gpr(insn.rA), {reg(ea)}, ga);
        return true;
    }

    // ---- Float single ----
    case Mnemonic::LFS:  case Mnemonic::LFSU: {
        VReg ea = make_ea(false);
        b.emit(Opcode::LoadFloat32, VReg::fpr(insn.rD), {reg(ea)}, ga);
        if (insn.mnemonic == Mnemonic::LFSU)
            b.emit(Opcode::Move, VReg::gpr(insn.rA), {reg(ea)}, ga);
        return true;
    }
    case Mnemonic::LFSX: case Mnemonic::LFSUX: {
        VReg ea = make_ea(true);
        b.emit(Opcode::LoadFloat32, VReg::fpr(insn.rD), {reg(ea)}, ga);
        if (insn.mnemonic == Mnemonic::LFSUX)
            b.emit(Opcode::Move, VReg::gpr(insn.rA), {reg(ea)}, ga);
        return true;
    }

    // ---- Float double ----
    case Mnemonic::LFD:  case Mnemonic::LFDU: {
        VReg ea = make_ea(false);
        b.emit(Opcode::LoadFloat64, VReg::fpr(insn.rD), {reg(ea)}, ga);
        if (insn.mnemonic == Mnemonic::LFDU)
            b.emit(Opcode::Move, VReg::gpr(insn.rA), {reg(ea)}, ga);
        return true;
    }
    case Mnemonic::LFDX: case Mnemonic::LFDUX: {
        VReg ea = make_ea(true);
        b.emit(Opcode::LoadFloat64, VReg::fpr(insn.rD), {reg(ea)}, ga);
        if (insn.mnemonic == Mnemonic::LFDUX)
            b.emit(Opcode::Move, VReg::gpr(insn.rA), {reg(ea)}, ga);
        return true;
    }

    // ---- Byte-reversed (load + bswap — emit as regular load for now) ----
    case Mnemonic::LHBRX: {
        VReg ea = make_ea(true);
        b.emit(Opcode::Load16, VReg::gpr(insn.rD), {reg(ea)}, ga);
        return true;
    }
    case Mnemonic::LWBRX: {
        VReg ea = make_ea(true);
        b.emit(Opcode::Load32, VReg::gpr(insn.rD), {reg(ea)}, ga);
        return true;
    }

    default:
        b.create_nop(ga);
        return true;
    }
}

// ---------------------------------------------------------------------------
// Memory stores
// ---------------------------------------------------------------------------

bool lower_store(const Instruction& insn, IRBuilder& b, IRFunction& /*f*/) {
    const uint32_t ga = insn.addr;

    auto make_ea = [&](bool indexed) -> VReg {
        if (indexed)
            return b.create_add(reg(VReg::gpr(insn.rA)), reg(VReg::gpr(insn.rB)), ga);
        else
            return b.create_add(reg(VReg::gpr(insn.rA)),
                                imm(static_cast<uint32_t>(insn.imm)), ga);
    };

    switch (insn.mnemonic) {

    // ---- Byte ----
    case Mnemonic::STB:  case Mnemonic::STBU: {
        VReg ea = make_ea(false);
        b.emit_void(Opcode::Store8, {reg(VReg::gpr(insn.rD)), reg(ea)}, ga);
        if (insn.mnemonic == Mnemonic::STBU)
            b.emit(Opcode::Move, VReg::gpr(insn.rA), {reg(ea)}, ga);
        return true;
    }
    case Mnemonic::STBX: case Mnemonic::STBUX: {
        VReg ea = make_ea(true);
        b.emit_void(Opcode::Store8, {reg(VReg::gpr(insn.rD)), reg(ea)}, ga);
        if (insn.mnemonic == Mnemonic::STBUX)
            b.emit(Opcode::Move, VReg::gpr(insn.rA), {reg(ea)}, ga);
        return true;
    }

    // ---- Halfword ----
    case Mnemonic::STH:  case Mnemonic::STHU: {
        VReg ea = make_ea(false);
        b.emit_void(Opcode::Store16, {reg(VReg::gpr(insn.rD)), reg(ea)}, ga);
        if (insn.mnemonic == Mnemonic::STHU)
            b.emit(Opcode::Move, VReg::gpr(insn.rA), {reg(ea)}, ga);
        return true;
    }
    case Mnemonic::STHX: case Mnemonic::STHUX: {
        VReg ea = make_ea(true);
        b.emit_void(Opcode::Store16, {reg(VReg::gpr(insn.rD)), reg(ea)}, ga);
        if (insn.mnemonic == Mnemonic::STHUX)
            b.emit(Opcode::Move, VReg::gpr(insn.rA), {reg(ea)}, ga);
        return true;
    }

    // ---- Word ----
    case Mnemonic::STW:  case Mnemonic::STWU: {
        VReg ea = make_ea(false);
        b.create_store32(reg(VReg::gpr(insn.rD)), reg(ea), ga);
        if (insn.mnemonic == Mnemonic::STWU)
            b.emit(Opcode::Move, VReg::gpr(insn.rA), {reg(ea)}, ga);
        return true;
    }
    case Mnemonic::STWX: case Mnemonic::STWUX: {
        VReg ea = make_ea(true);
        b.create_store32(reg(VReg::gpr(insn.rD)), reg(ea), ga);
        if (insn.mnemonic == Mnemonic::STWUX)
            b.emit(Opcode::Move, VReg::gpr(insn.rA), {reg(ea)}, ga);
        return true;
    }

    // ---- Float single ----
    case Mnemonic::STFS:  case Mnemonic::STFSU: {
        VReg ea = make_ea(false);
        b.emit_void(Opcode::StoreFloat32, {reg(VReg::fpr(insn.rD)), reg(ea)}, ga);
        if (insn.mnemonic == Mnemonic::STFSU)
            b.emit(Opcode::Move, VReg::gpr(insn.rA), {reg(ea)}, ga);
        return true;
    }
    case Mnemonic::STFSX: case Mnemonic::STFSUX: {
        VReg ea = make_ea(true);
        b.emit_void(Opcode::StoreFloat32, {reg(VReg::fpr(insn.rD)), reg(ea)}, ga);
        if (insn.mnemonic == Mnemonic::STFSUX)
            b.emit(Opcode::Move, VReg::gpr(insn.rA), {reg(ea)}, ga);
        return true;
    }

    // ---- Float double ----
    case Mnemonic::STFD:  case Mnemonic::STFDU: {
        VReg ea = make_ea(false);
        b.emit_void(Opcode::StoreFloat64, {reg(VReg::fpr(insn.rD)), reg(ea)}, ga);
        if (insn.mnemonic == Mnemonic::STFDU)
            b.emit(Opcode::Move, VReg::gpr(insn.rA), {reg(ea)}, ga);
        return true;
    }
    case Mnemonic::STFDX: case Mnemonic::STFDUX: {
        VReg ea = make_ea(true);
        b.emit_void(Opcode::StoreFloat64, {reg(VReg::fpr(insn.rD)), reg(ea)}, ga);
        if (insn.mnemonic == Mnemonic::STFDUX)
            b.emit(Opcode::Move, VReg::gpr(insn.rA), {reg(ea)}, ga);
        return true;
    }

    // ---- Byte-reversed ----
    case Mnemonic::STHBRX: {
        VReg ea = make_ea(true);
        b.emit_void(Opcode::Store16, {reg(VReg::gpr(insn.rD)), reg(ea)}, ga);
        return true;
    }
    case Mnemonic::STWBRX: {
        VReg ea = make_ea(true);
        b.create_store32(reg(VReg::gpr(insn.rD)), reg(ea), ga);
        return true;
    }

    default:
        b.create_nop(ga);
        return true;
    }
}

// ---------------------------------------------------------------------------
// Branch
// ---------------------------------------------------------------------------

bool lower_branch(const Instruction& insn, IRBuilder& b, IRFunction& f) {
    const uint32_t ga = insn.addr;

    // ---- Return ----
    if (insn.is_return()) {
        if (insn.lk) // BLRL: call LR then return — unusual, treat as indirect call
            b.emit_void(Opcode::IndirectCall, {}, ga);
        b.create_return(ga);
        return true;
    }

    // ---- Indirect branch through CTR ----
    if (insn.mnemonic == Mnemonic::BCTR || insn.mnemonic == Mnemonic::BCTRL) {
        if (insn.lk)
            b.emit_void(Opcode::IndirectCall, {}, ga);
        else
            b.emit_void(Opcode::IndirectJump, {}, ga);
        return true;
    }

    // ---- Unconditional direct branch ----
    if (insn.mnemonic == Mnemonic::B  || insn.mnemonic == Mnemonic::BA ||
        insn.mnemonic == Mnemonic::BL || insn.mnemonic == Mnemonic::BLA) {
        if (insn.lk)
            b.emit_void(Opcode::Call, {imm(insn.target)}, ga);
        else
            b.emit_void(Opcode::Jump, {imm(insn.target)}, ga);
        return true;
    }

    // ---- Conditional return (BCLR / BCLRL — branch to LR if condition true) ----
    // These are conditional returns: when taken, execution goes to the address in LR.
    // We must preserve the fallthrough path (PC+4) for when the condition is not taken.
    if (insn.mnemonic == Mnemonic::BCLR || insn.mnemonic == Mnemonic::BCLRL) {
        if (insn.lk)
            b.emit_void(Opcode::IndirectCall, {}, ga);
        const bool no_cr_test_bclr  = (insn.bo >> 4) & 1;
        const bool cr_sense_bclr    = (insn.bo >> 3) & 1;
        const bool no_ctr_test_bclr = (insn.bo >> 2) & 1;
        if (no_cr_test_bclr && no_ctr_test_bclr) {
            // BO=1x1x: unconditional return (blr encoded as bclr 20,0,0)
            b.create_return(ga);
        } else if (!no_cr_test_bclr && no_ctr_test_bclr) {
            // CR-only conditional: beqlr / bgelr / bnelr etc.
            uint32_t cr_field_bclr = insn.bi / 4;
            uint32_t bit_pos_bclr  = 3u - (insn.bi % 4u);
            VReg shifted_bclr = f.alloc_temp();
            b.emit(Opcode::Shr, shifted_bclr,
                   {reg(VReg::cr(cr_field_bclr)), imm(bit_pos_bclr)}, ga);
            VReg bit_val_bclr = f.alloc_temp();
            b.emit(Opcode::And, bit_val_bclr, {reg(shifted_bclr), imm(1u)}, ga);
            // ConditionalReturn(cond, fallthrough_addr): return to LR if cond,
            // else fall through to (ga+4).
            if (cr_sense_bclr) {
                b.emit_void(Opcode::ConditionalReturn,
                            {reg(bit_val_bclr), imm(ga + 4)}, ga);
            } else {
                VReg inv_bclr = f.alloc_temp();
                b.emit(Opcode::Xor, inv_bclr, {reg(bit_val_bclr), imm(1u)}, ga);
                b.emit_void(Opcode::ConditionalReturn,
                            {reg(inv_bclr), imm(ga + 4)}, ga);
            }
        } else {
            // CTR-involved conditional return — conservatively treat as unconditional
            b.create_return(ga);
        }
        return true;
    }

    // ---- Conditional branch (BC / BCL / BCA / BCLA) ----
    // Decode condition from BO and BI fields.
    // BO bit 4 (value 16): if set, don't test CR
    // BO bit 3 (value 8):  CR condition sense (1 = branch if CR[BI]=1)
    // BO bit 2 (value 4):  if set, don't decrement/test CTR
    // BO bit 1 (value 2):  CTR sense (1 = branch if CTR!=0)
    const bool no_cr_test  = (insn.bo >> 4) & 1;
    const bool cr_sense    = (insn.bo >> 3) & 1;
    const bool no_ctr_test = (insn.bo >> 2) & 1;

    // Build condition VReg from CR[BI]
    VReg cond_bit;
    if (!no_cr_test) {
        uint32_t cr_field = insn.bi / 4;
        uint32_t bit_pos  = 3u - (insn.bi % 4u); // LT=3, GT=2, EQ=1, SO=0

        // shifted = cr[cr_field] >> bit_pos
        VReg shifted = f.alloc_temp();
        b.emit(Opcode::Shr, shifted,
               {reg(VReg::cr(cr_field)), imm(bit_pos)}, ga);
        // bit_val = shifted & 1
        VReg bit_val = f.alloc_temp();
        b.emit(Opcode::And, bit_val, {reg(shifted), imm(1u)}, ga);

        uint32_t taken       = insn.target;
        uint32_t fallthrough = ga + 4;
        // cr_sense=1: branch if bit set; cr_sense=0: branch if bit clear → swap targets
        if (cr_sense) {
            b.emit_void(Opcode::Branch,
                        {reg(bit_val), imm(taken), imm(fallthrough)}, ga);
        } else {
            b.emit_void(Opcode::Branch,
                        {reg(bit_val), imm(fallthrough), imm(taken)}, ga);
        }
    } else if (!no_ctr_test) {
        // CTR-only conditional branch: bdnz / bdz
        // BO[1] (bit 1 of BO): 0 = branch if CTR!=0 (bdnz), 1 = branch if CTR==0 (bdz)
        const bool ctr_zero_sense = (insn.bo >> 1) & 1;
        // 1. Decrement CTR
        b.emit(Opcode::Sub, VReg::ctr(), {reg(VReg::ctr()), imm(1u)}, ga);
        // 2. Test CTR vs 0 (unsigned)
        VReg cmp = f.alloc_temp();
        b.emit(Opcode::CmpUnsigned, cmp, {reg(VReg::ctr()), imm(0u)}, ga);
        // cmp is 4-bit CR field: 8=LT, 4=GT, 2=EQ; CTR>0 → GT(4), CTR==0 → EQ(2)
        // Extract GT bit (bit2): nonzero when CTR > 0 (i.e., CTR != 0)
        VReg cond_shifted = f.alloc_temp();
        b.emit(Opcode::Shr, cond_shifted, {reg(cmp), imm(2u)}, ga);
        VReg cond_bit = f.alloc_temp();
        b.emit(Opcode::And, cond_bit, {reg(cond_shifted), imm(1u)}, ga);
        // cond_bit=1 when CTR!=0; cond_bit=0 when CTR==0
        uint32_t taken = insn.target;
        uint32_t fallthrough = ga + 4;
        if (!ctr_zero_sense) {
            // bdnz: branch (taken) when CTR != 0
            b.emit_void(Opcode::Branch, {reg(cond_bit), imm(taken), imm(fallthrough)}, ga);
        } else {
            // bdz: branch (taken) when CTR == 0 → swap taken/fallthrough
            b.emit_void(Opcode::Branch, {reg(cond_bit), imm(fallthrough), imm(taken)}, ga);
        }
    } else {
        // BO=20: unconditional
        b.emit_void(Opcode::Jump, {imm(insn.target)}, ga);
    }

    return true;
}

}