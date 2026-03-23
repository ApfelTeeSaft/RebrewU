#pragma once

#include "../decoder/ppc_decode.hpp"
#include "../../ir/ir_types.hpp"
#include "../../ir/ir_builder.hpp"
#include "../../ir/ir_function.hpp"

// ============================================================================
// RebrewU — Wii U static recompilation framework
// ppc_semantics.hpp — Lift PPC instructions to IR
//
// PpcSemantics contains one lowering function per instruction class.  It is
// called by CFGBuilder during the basic-block construction phase.  Each
// lowering function appends one or more IRInstrs to the current basic block
// via the provided IRBuilder.
// ============================================================================

namespace rebrewu::ppc {

/// Lowers a single decoded PPC instruction to zero or more IR instructions.
///
/// Returns false if the instruction could not be lowered (the caller may then
/// emit an Opcode::Undefined marker and continue or abort).
bool lower_to_ir(const Instruction& insn,
                 ir::IRBuilder&     builder,
                 ir::IRFunction&    func);

/// Lower an integer arithmetic / logical instruction.
bool lower_integer(const Instruction& insn,
                   ir::IRBuilder& b, ir::IRFunction& f);

/// Lower a floating-point instruction.
bool lower_float(const Instruction& insn,
                 ir::IRBuilder& b, ir::IRFunction& f);

/// Lower a memory load instruction.
bool lower_load(const Instruction& insn,
                ir::IRBuilder& b, ir::IRFunction& f);

/// Lower a memory store instruction.
bool lower_store(const Instruction& insn,
                 ir::IRBuilder& b, ir::IRFunction& f);

/// Lower a branch instruction.
bool lower_branch(const Instruction& insn,
                  ir::IRBuilder& b, ir::IRFunction& f);

/// Lower a compare instruction (updates a CR field VReg).
bool lower_compare(const Instruction& insn,
                   ir::IRBuilder& b, ir::IRFunction& f);

/// Lower an SPR move (mtspr / mfspr / mtlr / mflr / mtctr / mfctr …).
bool lower_spr(const Instruction& insn,
               ir::IRBuilder& b, ir::IRFunction& f);

/// Lower a rotate / shift instruction.
bool lower_rotate(const Instruction& insn,
                  ir::IRBuilder& b, ir::IRFunction& f);

}