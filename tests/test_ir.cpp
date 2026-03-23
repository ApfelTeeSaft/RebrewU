#include <catch2/catch_test_macros.hpp>
#include "ir/ir_types.hpp"
#include "ir/ir_function.hpp"
#include "ir/ir_module.hpp"
#include "ir/ir_builder.hpp"

using namespace rebrewu::ir;

TEST_CASE("VReg factory helpers produce correct kind/index", "[ir_types]") {
    auto r0 = VReg::gpr(0);
    REQUIRE(r0.kind  == RegKind::GPR);
    REQUIRE(r0.index == 0);

    auto f31 = VReg::fpr(31);
    REQUIRE(f31.kind  == RegKind::FPR);
    REQUIRE(f31.index == 31);

    auto lr = VReg::lr();
    REQUIRE(lr.kind == RegKind::LR);

    auto t5 = VReg::temp(5);
    REQUIRE(t5.kind  == RegKind::Temp);
    REQUIRE(t5.index == 5);
}

TEST_CASE("VReg equality operator", "[ir_types]") {
    REQUIRE(VReg::gpr(3) == VReg::gpr(3));
    REQUIRE_FALSE(VReg::gpr(3) == VReg::gpr(4));
    REQUIRE_FALSE(VReg::gpr(3) == VReg::fpr(3));
}

TEST_CASE("IRFunction adds blocks sequentially", "[ir_function]") {
    IRFunction func;
    func.entry_addr = 0x0200'0000;

    auto& b0 = func.add_block(0x0200'0000);
    auto& b1 = func.add_block(0x0200'0004);

    REQUIRE(func.blocks.size() == 2);
    REQUIRE(b0.id == 0);
    REQUIRE(b1.id == 1);
}

TEST_CASE("IRFunction::block_by_id returns correct block", "[ir_function]") {
    IRFunction func;
    func.add_block(0x100);
    func.add_block(0x104);

    auto* b = func.block_by_id(1);
    REQUIRE(b != nullptr);
    REQUIRE(b->guest_start == 0x104);

    REQUIRE(func.block_by_id(99) == nullptr);
}

TEST_CASE("IRFunction::block_at_addr lookup", "[ir_function]") {
    IRFunction func;
    func.add_block(0x0200'0000);

    auto* b = func.block_at_addr(0x0200'0000);
    REQUIRE(b != nullptr);
    REQUIRE(b->guest_start == 0x0200'0000);

    REQUIRE(func.block_at_addr(0xDEAD'BEEF) == nullptr);
}

TEST_CASE("IRFunction::alloc_temp allocates unique registers", "[ir_function]") {
    IRFunction func;
    auto t0 = func.alloc_temp();
    auto t1 = func.alloc_temp();
    REQUIRE(t0 != t1);
    REQUIRE(t0.kind == RegKind::Temp);
}

TEST_CASE("IRBuilder emits NOP instruction", "[ir_builder]") {
    IRFunction func;
    auto& blk = func.add_block(0x0200'0000);

    IRBuilder builder(func);
    builder.set_insert_point(blk);
    builder.create_nop(0x0200'0000);

    REQUIRE(blk.instrs.size() == 1);
    REQUIRE(blk.instrs[0].opcode == Opcode::Nop);
}

TEST_CASE("IRBuilder emits ADD instruction with correct operands", "[ir_builder]") {
    IRFunction func;
    auto& blk = func.add_block(0x0);

    IRBuilder builder(func);
    builder.set_insert_point(blk);
    auto result = builder.create_add(reg(VReg::gpr(3)), reg(VReg::gpr(4)));

    REQUIRE(result.kind  == RegKind::Temp);
    REQUIRE(blk.instrs.size() == 1);
    REQUIRE(blk.instrs[0].opcode == Opcode::Add);
    REQUIRE(blk.instrs[0].result.has_value());
}

TEST_CASE("IRBuilder create_jump emits correct instruction", "[ir_builder]") {
    IRFunction func;
    auto& blk = func.add_block(0x0);

    IRBuilder builder(func);
    builder.set_insert_point(blk);
    builder.create_jump(42u);

    REQUIRE(blk.instrs.back().opcode == Opcode::Jump);
    auto* la = std::get_if<LabelOp>(&blk.instrs.back().operands[0]);
    REQUIRE(la != nullptr);
    REQUIRE(la->block_id == 42u);
}

TEST_CASE("IRModule::add_function stores function and returns ref", "[ir_module]") {
    IRModule mod;
    auto& f = mod.add_function("main", 0x0200'0000);
    REQUIRE(f.name       == "main");
    REQUIRE(f.entry_addr == 0x0200'0000);
    REQUIRE(mod.functions.size() == 1);
}

TEST_CASE("IRModule::function_at lookup", "[ir_module]") {
    IRModule mod;
    mod.add_function("foo", 0x100);
    mod.add_function("bar", 0x200);

    REQUIRE(mod.function_at(0x100) != nullptr);
    REQUIRE(mod.function_at(0x200) != nullptr);
    REQUIRE(mod.function_at(0x300) == nullptr);
}

TEST_CASE("IRModule::add_symbol and symbol_at", "[ir_module]") {
    IRModule mod;
    mod.add_symbol(0x0200'0000, "entry");
    auto sym = mod.symbol_at(0x0200'0000);
    REQUIRE(sym.has_value());
    REQUIRE(*sym == "entry");
    REQUIRE_FALSE(mod.symbol_at(0x1234).has_value());
}
