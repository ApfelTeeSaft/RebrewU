#pragma once

#include "ir_function.hpp"
#include <cassert>

// ============================================================================
// RebrewU — Wii U static recompilation framework
// ir_builder.hpp — Fluent builder helper for constructing IR
// ============================================================================

namespace rebrewu::ir {

/// Provides a cursor-based API for appending IRInstrs to a BasicBlock.
class IRBuilder {
public:
    explicit IRBuilder(IRFunction& func) : m_func(func) {}

    // -----------------------------------------------------------------
    // Block management
    // -----------------------------------------------------------------

    void set_insert_point(uint32_t block_id) {
        m_block = m_func.block_by_id(block_id);
        assert(m_block && "block not found");
    }

    void set_insert_point(BasicBlock& block) { m_block = &block; }

    BasicBlock& current_block() {
        assert(m_block && "no insert point");
        return *m_block;
    }

    // -----------------------------------------------------------------
    // Instruction builders
    // -----------------------------------------------------------------

    VReg create_move(VReg src, uint32_t guest_addr = 0) {
        VReg dst = m_func.alloc_temp();
        emit(Opcode::Move, dst, {reg(src)}, guest_addr);
        return dst;
    }

    VReg create_add(IROperand lhs, IROperand rhs, uint32_t guest_addr = 0) {
        VReg dst = m_func.alloc_temp();
        emit(Opcode::Add, dst, {lhs, rhs}, guest_addr);
        return dst;
    }

    VReg create_sub(IROperand lhs, IROperand rhs, uint32_t guest_addr = 0) {
        VReg dst = m_func.alloc_temp();
        emit(Opcode::Sub, dst, {lhs, rhs}, guest_addr);
        return dst;
    }

    VReg create_load32(IROperand addr_op, uint32_t guest_addr = 0) {
        VReg dst = m_func.alloc_temp();
        emit(Opcode::Load32, dst, {addr_op}, guest_addr);
        return dst;
    }

    void create_store32(IROperand val, IROperand addr_op, uint32_t guest_addr = 0) {
        emit_void(Opcode::Store32, {val, addr_op}, guest_addr);
    }

    void create_jump(uint32_t target_block_id, uint32_t guest_addr = 0) {
        emit_void(Opcode::Jump, {label(target_block_id)}, guest_addr);
    }

    void create_branch(IROperand cond, uint32_t true_id, uint32_t false_id,
                       uint32_t guest_addr = 0) {
        emit_void(Opcode::Branch, {cond, label(true_id), label(false_id)}, guest_addr);
    }

    void create_return(uint32_t guest_addr = 0) {
        emit_void(Opcode::Return, {}, guest_addr);
    }

    void create_nop(uint32_t guest_addr = 0) {
        emit_void(Opcode::Nop, {}, guest_addr);
    }

    // -----------------------------------------------------------------
    // Raw emission
    // -----------------------------------------------------------------

    void emit(Opcode op, VReg dst, std::vector<IROperand> ops, uint32_t ga = 0) {
        assert(m_block);
        m_block->append(IRInstr(op, dst, std::move(ops), ga));
    }

    void emit_void(Opcode op, std::vector<IROperand> ops, uint32_t ga = 0) {
        assert(m_block);
        m_block->append(IRInstr(op, std::nullopt, std::move(ops), ga));
    }

private:
    IRFunction& m_func;
    BasicBlock* m_block{nullptr};
};

}