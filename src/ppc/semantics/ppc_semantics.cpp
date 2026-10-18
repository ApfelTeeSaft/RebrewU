#include "ppc_semantics.hpp"

namespace rebrewu::ppc {

using namespace ir;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Architectural integer CR writes also copy XER.SO. Keep this separate from
// IR comparison temporaries, whose LT/GT/EQ bits serve arithmetic predicates.
static void emit_cr_so(VReg field, IRBuilder& b, uint32_t ga) {
    VReg so = b.create_move(VReg::xer(), ga);
    b.emit(Opcode::Shr, so, {reg(so), imm(31)}, ga);
    b.emit(Opcode::Or, field, {reg(field), reg(so)}, ga);
}

// Emit a CR0 update after an Rc=1 instruction (result already in `res`).
static void emit_rc(VReg res, IRBuilder& b, uint32_t ga) {
    b.emit(Opcode::CmpSigned, VReg::cr(0),
           {reg(res), imm(0)}, ga);
    emit_cr_so(VReg::cr(0), b, ga);
}

// All carrying arithmetic is a 32-bit addition of two terms plus 0/1.
// Subtract-from uses the one's complement of RA. Keep intermediate values
// until CA and the result are ready, including when RT aliases RA or RB.
static void emit_carry_sum(const Instruction& insn, IRBuilder& b, IRFunction& f,
                           IROperand lhs, IROperand rhs, bool complement,
                           bool carry_in, uint32_t fixed_carry = 0) {
    const uint32_t ga = insn.addr;
    auto temp = [&](Opcode op, std::vector<IROperand> args) {
        VReg value = f.alloc_temp(); b.emit(op, value, std::move(args), ga); return value;
    };
    VReg a = temp(complement ? Opcode::Not : Opcode::Move, {lhs});
    VReg c = carry_in
        ? temp(Opcode::And, {reg(temp(Opcode::Shr, {reg(VReg::xer()), imm(29)})), imm(1)})
        : temp(Opcode::Move, {imm(fixed_carry)});
    VReg sum = b.create_add(reg(a), rhs, ga);
    VReg result = b.create_add(reg(sum), reg(c), ga);
    VReg overflow1 = temp(Opcode::CmpUnsigned, {reg(sum), reg(a)});
    VReg overflow2 = temp(Opcode::CmpUnsigned, {reg(result), reg(sum)});
    VReg ca = temp(Opcode::And, {reg(temp(Opcode::Or, {reg(overflow1), reg(overflow2)})), imm(8)});
    ca = temp(Opcode::Shl, {reg(ca), imm(26)}); // CR.LT bit 3 -> XER.CA bit 29
    VReg xer = temp(Opcode::And, {reg(VReg::xer()), imm(~0x20000000u)});
    xer = temp(Opcode::Or, {reg(xer), reg(ca)});
    const bool xo_form = (insn.word >> 26) == 31;
    if (xo_form && insn.oe) {
        VReg same_sign = temp(Opcode::Not, {reg(temp(Opcode::Xor, {reg(a), rhs}))});
        VReg changed_sign = temp(Opcode::Xor, {reg(a), reg(result)});
        VReg ov = temp(Opcode::And, {reg(same_sign), reg(changed_sign)});
        ov = temp(Opcode::Shr, {reg(ov), imm(31)});
        xer = temp(Opcode::And, {reg(xer), imm(~0x40000000u)});
        xer = temp(Opcode::Or, {reg(xer), reg(temp(Opcode::Shl, {reg(ov), imm(30)}))});
        xer = temp(Opcode::Or, {reg(xer), reg(temp(Opcode::Shl, {reg(ov), imm(31)}))});
    }
    b.emit(Opcode::Move, VReg::xer(), {reg(xer)}, ga);
    b.emit(Opcode::Move, VReg::gpr(insn.rD), {reg(result)}, ga);
    if ((xo_form && insn.rc) || insn.mnemonic == Mnemonic::ADDIC_) {
        emit_rc(VReg::gpr(insn.rD), b, ga);
        VReg so = temp(Opcode::Shr, {reg(VReg::xer()), imm(31)});
        b.emit(Opcode::Or, VReg::cr(0), {reg(VReg::cr(0)), reg(so)}, ga);
    }
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

static bool lower_core(const Instruction& insn,
                 IRBuilder& builder,
                 IRFunction& func) {
    // Paired singles and GQR moves run through runtime rbrew_ps (decaf
    // semantics); unnamed opcode-4 forms stay unlowered.
    if (insn.iclass == InstrClass::Paired) {
        if (insn.mnemonic == Mnemonic::UNKNOWN) return false;
        builder.emit_void(Opcode::PairedSingle, {imm(insn.word)}, insn.addr);
        return true;
    }
    if (insn.mnemonic == Mnemonic::TW || insn.mnemonic == Mnemonic::TWI) {
        builder.emit_void(Opcode::Trap, {imm(insn.word), imm(insn.addr)}, insn.addr);
        return true;
    }
    if (insn.mnemonic == Mnemonic::MTSPR || insn.mnemonic == Mnemonic::MFSPR) {
        const uint32_t spr = ((insn.word >> 16) & 31u) | (((insn.word >> 11) & 31u) << 5);
        if ((spr >= 912 && spr <= 919) || (spr >= 896 && spr <= 903)) {
            builder.emit_void(Opcode::PairedSingle, {imm(insn.word)}, insn.addr);
            return true;
        }
    }
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
    case InstrClass::LoadMultiple:
    case InstrClass::StoreMultiple: {
        // Only D-form multiple-word transfers belong in this path.
        if ((insn.word >> 26) != 46 && (insn.word >> 26) != 47) return false;
        // LMW/STMW transfer consecutive 32-bit registers through r31.
        // Snapshot EA before loading any register that could alias rA.
        VReg base = builder.create_add(insn.rA ? reg(VReg::gpr(insn.rA)) : imm(0u),
            imm(static_cast<uint32_t>(insn.imm)), insn.addr);
        for (uint32_t r = insn.rD; r < 32; ++r) {
            VReg ea = builder.create_add(reg(base), imm((r - insn.rD) * 4u), insn.addr);
            if (insn.iclass == InstrClass::LoadMultiple)
                builder.emit(Opcode::Load32, VReg::gpr(r), {reg(ea)}, insn.addr);
            else
                builder.create_store32(reg(VReg::gpr(r)), reg(ea), insn.addr);
        }
        return true;
    }
    case InstrClass::Branch:   return lower_branch(insn, builder, func);
    case InstrClass::CacheControl:
        if (insn.mnemonic == Mnemonic::DCBZ) {
            // dcbz zeroes the 32-byte block containing (rA|0) + rB.
            VReg ea = builder.create_add(insn.rA ? reg(VReg::gpr(insn.rA)) : imm(0u),
                                         reg(VReg::gpr(insn.rB)), insn.addr);
            VReg line = func.alloc_temp();
            builder.emit(Opcode::And, line, {reg(ea), imm(~31u)}, insn.addr);
            for (uint32_t off = 0; off < 32; off += 4) {
                VReg at = builder.create_add(reg(line), imm(off), insn.addr);
                builder.emit_void(Opcode::Store32, {imm(0u), reg(at)}, insn.addr);
            }
            return true;
        }
        // Other cache maintenance has no effect on the host's coherent memory.
        builder.create_nop(insn.addr);
        return true;
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
        emit_carry_sum(insn, b, f, reg(VReg::gpr(insn.rA)),
                       imm(static_cast<uint32_t>(insn.imm)), false, false);
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
        emit_carry_sum(insn, b, f, reg(VReg::gpr(insn.rA)),
                       reg(VReg::gpr(insn.rB)), false, false);
        return true;

    case Mnemonic::ADDE: case Mnemonic::ADDE_:
    case Mnemonic::ADDEO: case Mnemonic::ADDEO_: {
        emit_carry_sum(insn, b, f, reg(VReg::gpr(insn.rA)),
                       reg(VReg::gpr(insn.rB)), false, true);
        return true;
    }

    case Mnemonic::ADDME: case Mnemonic::ADDME_:
    case Mnemonic::ADDMEO: case Mnemonic::ADDMEO_:
        emit_carry_sum(insn, b, f, reg(VReg::gpr(insn.rA)), imm(~0u), false, true);
        return true;

    case Mnemonic::ADDZE: case Mnemonic::ADDZE_:
    case Mnemonic::ADDZEO: case Mnemonic::ADDZEO_:
        emit_carry_sum(insn, b, f, reg(VReg::gpr(insn.rA)), imm(0), false, true);
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
        emit_carry_sum(insn, b, f, reg(VReg::gpr(insn.rA)),
                       imm(static_cast<uint32_t>(insn.imm)), true, false, 1);
        return true;

    case Mnemonic::SUBFC: case Mnemonic::SUBFC_:
    case Mnemonic::SUBFCO: case Mnemonic::SUBFCO_:
        emit_carry_sum(insn, b, f, reg(VReg::gpr(insn.rA)),
                       reg(VReg::gpr(insn.rB)), true, false, 1);
        return true;

    case Mnemonic::SUBFE: case Mnemonic::SUBFE_:
    case Mnemonic::SUBFEO: case Mnemonic::SUBFEO_:
        emit_carry_sum(insn, b, f, reg(VReg::gpr(insn.rA)),
                       reg(VReg::gpr(insn.rB)), true, true);
        return true;

    case Mnemonic::SUBFME: case Mnemonic::SUBFME_:
    case Mnemonic::SUBFMEO: case Mnemonic::SUBFMEO_:
        emit_carry_sum(insn, b, f, reg(VReg::gpr(insn.rA)), imm(~0u), true, true);
        return true;

    case Mnemonic::SUBFZE: case Mnemonic::SUBFZE_:
    case Mnemonic::SUBFZEO: case Mnemonic::SUBFZEO_:
        emit_carry_sum(insn, b, f, reg(VReg::gpr(insn.rA)), imm(0), true, true);
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

    case Mnemonic::MFCR: {
        // rD = CR0..CR7 as nibbles, CR0 in the most significant (LT GT EQ SO).
        VReg acc = f.alloc_temp();
        b.emit(Opcode::Move, acc, {imm(0u)}, ga);
        for (uint32_t i = 0; i < 8; ++i) {
            VReg field = f.alloc_temp(), shifted = f.alloc_temp();
            b.emit(Opcode::And, field, {reg(VReg::cr(i)), imm(0xFu)}, ga);
            b.emit(Opcode::Shl, shifted, {reg(field), imm(28u - 4u * i)}, ga);
            b.emit(Opcode::Or, acc, {reg(acc), reg(shifted)}, ga);
        }
        b.emit(Opcode::Move, VReg::gpr(insn.rD), {reg(acc)}, ga);
        return true;
    }

    case Mnemonic::MTCRF: {
        // Update each CR field selected by FXM (bit 0x80 = CR0) from rS.
        const uint32_t fxm = (insn.word >> 12) & 0xFFu;
        for (uint32_t i = 0; i < 8; ++i) {
            if (!(fxm & (0x80u >> i))) continue;
            VReg shifted = f.alloc_temp();
            b.emit(Opcode::Shr, shifted, {reg(VReg::gpr(insn.rD)), imm(28u - 4u * i)}, ga);
            b.emit(Opcode::And, VReg::cr(i), {reg(shifted), imm(0xFu)}, ga);
        }
        return true;
    }

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
        emit_cr_so(VReg::cr(crf), b, ga);
        return true;

    case Mnemonic::CMPI:
        b.emit(Opcode::CmpSigned, VReg::cr(crf),
               {reg(VReg::gpr(insn.rA)), imm(static_cast<uint32_t>(insn.imm))}, ga);
        emit_cr_so(VReg::cr(crf), b, ga);
        return true;

    case Mnemonic::CMPL:
        b.emit(Opcode::CmpUnsigned, VReg::cr(crf),
               {reg(VReg::gpr(insn.rA)), reg(VReg::gpr(insn.rB))}, ga);
        emit_cr_so(VReg::cr(crf), b, ga);
        return true;

    case Mnemonic::CMPLI:
        b.emit(Opcode::CmpUnsigned, VReg::cr(crf),
               {reg(VReg::gpr(insn.rA)), imm(insn.uimm)}, ga);
        emit_cr_so(VReg::cr(crf), b, ga);
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
    auto temp = [&](Opcode op, std::vector<IROperand> args) {
        VReg value = f.alloc_temp(); b.emit(op, value, std::move(args), ga); return value;
    };
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

    // ---- Logical shifts: amount is rB[26:31]; 32..63 shift every bit out ----
    case Mnemonic::SLW: case Mnemonic::SLW_:
    case Mnemonic::SRW: case Mnemonic::SRW_: {
        const bool left = insn.mnemonic == Mnemonic::SLW || insn.mnemonic == Mnemonic::SLW_;
        VReg amount = temp(Opcode::And, {reg(VReg::gpr(insn.rB)), imm(63)});
        VReg shifted = temp(left ? Opcode::Shl : Opcode::Shr, {reg(VReg::gpr(insn.rD)), reg(amount)});
        // keep = 0xFFFFFFFF when amount < 32, else 0 (amount bit 5 clear).
        VReg keep = temp(Opcode::Sub, {reg(temp(Opcode::And,
            {reg(temp(Opcode::Shr, {reg(amount), imm(5)})), imm(1)})), imm(1)});
        b.emit(Opcode::And, VReg::gpr(insn.rA), {reg(shifted), reg(keep)}, ga);
        if (insn.rc) emit_rc(VReg::gpr(insn.rA), b, ga);
        return true;
    }

    // ---- Algebraic right shift: CA = RS negative and a 1 bit shifted out ----
    case Mnemonic::SRAW: case Mnemonic::SRAW_:
    case Mnemonic::SRAWI: case Mnemonic::SRAWI_: {
        const bool immediate = insn.mnemonic == Mnemonic::SRAWI || insn.mnemonic == Mnemonic::SRAWI_;
        // Capture RS and the amount before RA is written (RA may alias them).
        VReg source = temp(Opcode::Move, {reg(VReg::gpr(insn.rD))});
        VReg amount = immediate ? temp(Opcode::Move, {imm(insn.sh & 31u)})
                                : temp(Opcode::And, {reg(VReg::gpr(insn.rB)), imm(63)});
        VReg sign = temp(Opcode::Sar, {reg(source), imm(31)});           // 0 or ~0
        VReg wide = temp(Opcode::And, {reg(temp(Opcode::Shr, {reg(amount), imm(5)})), imm(1)});
        VReg narrow_mask = temp(Opcode::Sub, {reg(wide), imm(1)});      // ~0 if amount < 32
        VReg shifted = temp(Opcode::Sar, {reg(source), reg(amount)});    // uses amount & 31
        VReg result = temp(Opcode::Or, {reg(temp(Opcode::And, {reg(shifted), reg(narrow_mask)})),
            reg(temp(Opcode::And, {reg(sign), reg(temp(Opcode::Not, {reg(narrow_mask)}))}))});
        // Bits lost below the shift: (1 << n) - 1 for n < 32, all bits otherwise.
        VReg low_mask = temp(Opcode::Sub, {reg(temp(Opcode::Shl, {imm(1), reg(amount)})), imm(1)});
        low_mask = temp(Opcode::Or, {reg(low_mask), reg(temp(Opcode::Not, {reg(narrow_mask)}))});
        VReg lost = temp(Opcode::And, {reg(source), reg(low_mask)});
        // CmpUnsigned(lost, 0) has GT (bit 2) exactly when lost != 0.
        VReg any_lost = temp(Opcode::And, {reg(temp(Opcode::Shr,
            {reg(temp(Opcode::CmpUnsigned, {reg(lost), imm(0)})), imm(2)})), imm(1)});
        VReg ca = temp(Opcode::And, {reg(any_lost), reg(temp(Opcode::Shr, {reg(source), imm(31)}))});
        VReg xer = temp(Opcode::And, {reg(VReg::xer()), imm(~0x20000000u)});
        b.emit(Opcode::Move, VReg::xer(),
               {reg(temp(Opcode::Or, {reg(xer), reg(temp(Opcode::Shl, {reg(ca), imm(29)}))}))}, ga);
        b.emit(Opcode::Move, VReg::gpr(insn.rA), {reg(result)}, ga);
        if (insn.rc) emit_rc(VReg::gpr(insn.rA), b, ga);
        return true;
    }

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

    case Mnemonic::FCTIWZ:
        b.emit(Opcode::FCvtToIntZero, VReg::fpr(insn.rD),
               {reg(VReg::fpr(insn.rB)), imm(insn.rc ? 1u : 0u)}, ga);
        return true;

    case Mnemonic::FCTIW:
        // RN-controlled FCTIW remains a separate incomplete semantics class.
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
        // Rc requires CR1 from FPSCR, which this runtime does not yet model.
        // Keep that form visibly unsupported in the audit rather than claim
        // full semantics from the ordinary nonrecord value-selection rule.
        if (insn.rc) { b.create_nop(ga); return true; }
        b.emit(Opcode::FSelect, VReg::fpr(insn.rD),
               {reg(VReg::fpr(insn.rA)), reg(VReg::fpr(insn.rC)),
                reg(VReg::fpr(insn.rB))}, ga);
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

// Byte-reverse the low 16 bits (half) or the full word of v into a new temp.
static VReg emit_byte_reverse(IRBuilder& b, IRFunction& f, VReg v, bool half, uint32_t ga) {
    auto part = [&](uint32_t shift, bool left, uint32_t mask) {
        VReg shifted = f.alloc_temp(), masked = f.alloc_temp();
        b.emit(left ? Opcode::Shl : Opcode::Shr, shifted, {reg(v), imm(shift)}, ga);
        b.emit(Opcode::And, masked, {reg(shifted), imm(mask)}, ga);
        return masked;
    };
    VReg out = f.alloc_temp();
    if (half) {
        VReg lo = part(8, true, 0xFF00u), hi = part(8, false, 0x00FFu);
        b.emit(Opcode::Or, out, {reg(lo), reg(hi)}, ga);
        return out;
    }
    VReg a = part(24, false, 0x000000FFu), c = part(8, false, 0x0000FF00u);
    VReg d = part(8, true, 0x00FF0000u), e = part(24, true, 0xFF000000u);
    VReg ac = f.alloc_temp(), de = f.alloc_temp();
    b.emit(Opcode::Or, ac, {reg(a), reg(c)}, ga);
    b.emit(Opcode::Or, de, {reg(d), reg(e)}, ga);
    b.emit(Opcode::Or, out, {reg(ac), reg(de)}, ga);
    return out;
}

bool lower_load(const Instruction& insn, IRBuilder& b, IRFunction& f) {
    const uint32_t ga = insn.addr;

    if (insn.mnemonic == Mnemonic::LWARX) {
        VReg ea = b.create_add(insn.rA ? reg(VReg::gpr(insn.rA)) : imm(0u),
                              reg(VReg::gpr(insn.rB)), ga);
        b.emit(Opcode::LoadReserve32, VReg::gpr(insn.rD), {reg(ea)}, ga);
        return true;
    }

    if (insn.mnemonic == Mnemonic::LSWI) {
        const uint32_t count = insn.rB ? insn.rB : 32u; // NB field
        const uint32_t registers = (count + 3u) / 4u;
        // These operand combinations are invalid in the architecture.
        if (insn.rD == 0 && insn.rA == 0) return false;
        for (uint32_t n = 0; n < registers; ++n)
            if (insn.rA && ((insn.rD + n) & 31u) == insn.rA) return false;
        VReg base = b.create_add(insn.rA ? reg(VReg::gpr(insn.rA)) : imm(0u),
                                imm(0u), ga);
        for (uint32_t n = 0; n < count; ++n) {
            VReg dst = VReg::gpr((insn.rD + n / 4u) & 31u);
            if (n % 4u == 0) b.emit(Opcode::Move, dst, {imm(0u)}, ga);
            VReg ea = b.create_add(reg(base), imm(n), ga);
            VReg byte = f.alloc_temp();
            b.emit(Opcode::Load8, byte, {reg(ea)}, ga);
            VReg shifted = f.alloc_temp();
            b.emit(Opcode::Shl, shifted, {reg(byte), imm(24u - (n % 4u) * 8u)}, ga);
            b.emit(Opcode::Or, dst, {reg(dst), reg(shifted)}, ga);
        }
        return true;
    }

    // Helper: compute effective address (base + offset or base + index)
    // rA = 0 names the literal 0 (non-update forms), not r0.
    auto base_a = [&]() { return insn.rA ? reg(VReg::gpr(insn.rA)) : imm(0u); };
    auto make_ea = [&](bool indexed) -> VReg {
        if (indexed)
            return b.create_add(base_a(), reg(VReg::gpr(insn.rB)), ga);
        else
            return b.create_add(base_a(),
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
        VReg ea = make_ea(true), raw = f.alloc_temp();
        b.emit(Opcode::Load16, raw, {reg(ea)}, ga);
        b.emit(Opcode::Move, VReg::gpr(insn.rD), {reg(emit_byte_reverse(b, f, raw, true, ga))}, ga);
        return true;
    }
    case Mnemonic::LWBRX: {
        VReg ea = make_ea(true), raw = f.alloc_temp();
        b.emit(Opcode::Load32, raw, {reg(ea)}, ga);
        b.emit(Opcode::Move, VReg::gpr(insn.rD), {reg(emit_byte_reverse(b, f, raw, false, ga))}, ga);
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

bool lower_store(const Instruction& insn, IRBuilder& b, IRFunction& f) {
    const uint32_t ga = insn.addr;

    if (insn.mnemonic == Mnemonic::STSWI) {
        // Store NB bytes (0 = 32) from rS, rS+1, ... high byte first.
        const uint32_t count = insn.rB ? insn.rB : 32u; // NB field
        VReg base = b.create_add(insn.rA ? reg(VReg::gpr(insn.rA)) : imm(0u), imm(0u), ga);
        for (uint32_t n = 0; n < count; ++n) {
            VReg src = VReg::gpr((insn.rD + n / 4u) & 31u);
            VReg byte = f.alloc_temp();
            b.emit(Opcode::Shr, byte, {reg(src), imm(24u - (n % 4u) * 8u)}, ga);
            VReg ea = b.create_add(reg(base), imm(n), ga);
            b.emit_void(Opcode::Store8, {reg(byte), reg(ea)}, ga);
        }
        return true;
    }

    if (insn.mnemonic == Mnemonic::STFIWX) {
        VReg ea = b.create_add(insn.rA ? reg(VReg::gpr(insn.rA)) : imm(0u),
                              reg(VReg::gpr(insn.rB)), ga);
        b.emit_void(Opcode::StoreFloatInteger32, {reg(VReg::fpr(insn.rD)), reg(ea)}, ga);
        return true;
    }

    if (insn.mnemonic == Mnemonic::STWCX) {
        VReg ea = b.create_add(insn.rA ? reg(VReg::gpr(insn.rA)) : imm(0u),
                              reg(VReg::gpr(insn.rB)), ga);
        b.emit_void(Opcode::StoreConditional32, {reg(VReg::gpr(insn.rD)), reg(ea)}, ga);
        return true;
    }

    // rA = 0 names the literal 0 (non-update forms), not r0.
    auto base_a = [&]() { return insn.rA ? reg(VReg::gpr(insn.rA)) : imm(0u); };
    auto make_ea = [&](bool indexed) -> VReg {
        if (indexed)
            return b.create_add(base_a(), reg(VReg::gpr(insn.rB)), ga);
        else
            return b.create_add(base_a(),
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
        VReg swapped = emit_byte_reverse(b, f, VReg::gpr(insn.rD), true, ga);
        b.emit_void(Opcode::Store16, {reg(swapped), reg(ea)}, ga);
        return true;
    }
    case Mnemonic::STWBRX: {
        VReg ea = make_ea(true);
        VReg swapped = emit_byte_reverse(b, f, VReg::gpr(insn.rD), false, ga);
        b.create_store32(reg(swapped), reg(ea), ga);
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

    // A linked conditional branch is a call, not a conditional tail jump.
    // CFG deliberately keeps its PC+4 continuation in the current block.
    if (insn.lk) {
        VReg taken = f.alloc_temp();
        b.emit(Opcode::Move, taken, {imm(1u)}, ga);
        if (!no_cr_test) {
            VReg shifted = f.alloc_temp(), bit = f.alloc_temp();
            b.emit(Opcode::Shr, shifted,
                   {reg(VReg::cr(insn.bi / 4)), imm(3u - insn.bi % 4u)}, ga);
            b.emit(Opcode::And, bit, {reg(shifted), imm(1u)}, ga);
            if (!cr_sense)
                b.emit(Opcode::Xor, bit, {reg(bit), imm(1u)}, ga);
            b.emit(Opcode::And, taken, {reg(taken), reg(bit)}, ga);
        }
        if (!no_ctr_test) {
            b.emit(Opcode::Sub, VReg::ctr(), {reg(VReg::ctr()), imm(1u)}, ga);
            VReg cmp = f.alloc_temp(), shifted = f.alloc_temp(), bit = f.alloc_temp();
            b.emit(Opcode::CmpUnsigned, cmp, {reg(VReg::ctr()), imm(0u)}, ga);
            b.emit(Opcode::Shr, shifted, {reg(cmp), imm(2u)}, ga);
            b.emit(Opcode::And, bit, {reg(shifted), imm(1u)}, ga);
            if (insn.bo & 2u)
                b.emit(Opcode::Xor, bit, {reg(bit), imm(1u)}, ga);
            b.emit(Opcode::And, taken, {reg(taken), reg(bit)}, ga);
        }
        b.emit_void(Opcode::ConditionalCall, {reg(taken), imm(insn.target)}, ga);
        return true;
    }

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

bool lower_to_ir(const Instruction& insn, IRBuilder& builder, IRFunction& func) {
    if (!lower_core(insn, builder, func)) return false;
    // Espresso writes single-precision scalar results (and lfs loads) to both
    // paired-single slots; double-precision ops and fmr leave ps1 alone.
    switch (insn.mnemonic) {
    case Mnemonic::LFS: case Mnemonic::LFSX: case Mnemonic::LFSU: case Mnemonic::LFSUX:
    case Mnemonic::FADDS: case Mnemonic::FSUBS: case Mnemonic::FMULS: case Mnemonic::FDIVS:
    case Mnemonic::FMADDS: case Mnemonic::FMSUBS: case Mnemonic::FNMADDS: case Mnemonic::FNMSUBS:
    case Mnemonic::FRES: case Mnemonic::FRSP: case Mnemonic::FSQRTS:
        builder.emit_void(Opcode::PairedFill, {imm(insn.rD)}, insn.addr);
        break;
    default: break;
    }
    return true;
}

}
